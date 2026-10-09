// Per-corner rendering hook on Render::IElementRenderer::drawSurface.
//
// Approach:
//
//   1. Paint the window body once through the original function, with its damage region
//      shrunk to exclude the 4 corner boxes and `dontRound` forced on, so the body is a
//      plain square outside the corners. (Native rounding is already 0, because the
//      plugin requires `decoration:rounding = 0`.)
//   2. Redraw the same window texture once per corner with g_pHyprOpenGL->renderTexture(),
//      limited to that corner's box by a damage region and passing that corner's live
//      radius as `.round`. Hyprland's rounded-rect shader draws the curve, so no custom
//      GLSL is needed.
//   3. The corner draws use the same UV mapping as the body, recomputed through Hyprland's
//      exported IElementRenderer::calculateUVForSurface. Corners and body then sample the
//      same texels, even during open/resize/reflow animations, when the client's buffer
//      size lags the animated window box on most frames.
//
// This works because g_pHyprRenderer->m_renderData.damage is what restricts drawing;
// drawSurface never reads its `damage` parameter (checked in source and by testing). It
// doesn't work when m_renderData.clipBox is set, for example on a floating window during
// a slide animation. For that frame the window is drawn natively with square corners.
// Surfaces Hyprland marks dontRound (internal fullscreen) are also drawn natively.
//
// Subsurfaces are drawn natively by default too, which is why Firefox-based browsers
// (Firefox, Zen, ...) stay square. They draw the whole page into a wl_subsurface that
// covers the main surface and hides its rounded corners. With
// plugin:seam:force_round_risky_surfaces = true, a subsurface that reaches a window corner
// gets that corner rounded (see subsurfaceCorners()).
#define WLR_USE_UNSTABLE

#include "SeamHook.hpp"
#include "SeamState.hpp"
#include "globals.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/types/GeometricMovableAnimated.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace {
    CFunctionHook* g_hook = nullptr;

    // From /usr/include/hyprland/src/render/ElementRenderer.hpp (Hyprland 0.56.2):
    //   void Render::IElementRenderer::drawSurface(WP<CSurfacePassElement> element, const CRegion& damage);
    // It's a private, non-virtual member. Under the Itanium C++ ABI, a non-virtual member
    // is called like a free function with the object pointer as the first argument, so
    // `this` is an explicit leading void* here. WP<> isn't trivially copyable, so both the
    // original and the replacement take it by invisible reference; declaring the same C++
    // parameter types gives both sides the same ABI.
    using drawSurface_t = void (*)(void* thisptr, WP<CSurfacePassElement> element, const CRegion& damage);

    constexpr const char* TARGET_DEMANGLED = "Render::IElementRenderer::drawSurface(Hyprutils::Memory::CWeakPointer<CSurfacePassElement>, Hyprutils::Math::CRegion const&)";

    // Per ElementRenderer.hpp (0.56.2) and `nm -DC /usr/bin/Hyprland`, calculateUVForSurface
    // is a private, non-virtual member of IElementRenderer whose symbol is exported.
    // findFunctionsByName can find it, and it's called with the same leading-`this`
    // convention as drawSurface. SP<> arguments aren't trivially copyable, so they go by
    // invisible reference; the same C++ parameter types give a matching ABI.
    using calculateUV_t = void (*)(void* thisptr, PHLWINDOW, SP<CWLSurfaceResource>, PHLMONITOR, bool main, const Vector2D& projSize, const Vector2D& projSizeUnscaled,
                                   bool fixMisalignedFSV1);

    constexpr const char* CALCUV_DEMANGLED =
        "Render::IElementRenderer::calculateUVForSurface(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Hyprutils::Memory::CSharedPointer<CWLSurfaceResource>, "
        "Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, bool, Hyprutils::Math::Vector2D const&, Hyprutils::Math::Vector2D const&, bool)";

    calculateUV_t g_calculateUV = nullptr;

    // One corner's redraw box, in the same monitor-scaled, rounded pixel space as the
    // windowBox drawSurface computes, plus the radius for that corner's shader call.
    //
    // The box snaps outward to whole pixels: its size is ceil(radius), anchored on the
    // window's integer edge, and windowBox is already integral. CRegion(CBox) passes double
    // coordinates to pixman_region32_init_rect, which truncates x and width separately. With
    // a fractional radius (fractional scale, or mid-animation), a right or bottom patch at
    // `edge - 16.5` becomes [edge-17, edge-1), which leaves the outer pixel column or row out
    // of the pruned region, and the square body shows through as a 1px sliver beside the
    // curve. The damage subtraction and the corner draw use the same box, so they always
    // agree. `.round` is the real radius rounded to an int, since it sets the curve; the
    // corner draw paints the extra <1px of box as plain body.
    struct SCornerPatch {
        CBox box;
        int  round = 0; // scaled px; 0 means "no rounding needed here, leave it to the square body paint"
    };

    SCornerPatch makeCornerPatch(const CBox& windowBox, bool left, bool top, double liveRadiusLogicalPx, double monitorScale) {
        // liveRadiusLogicalPx comes from SeamState::liveCornersFor, already clamped to the
        // window's logical size (clampCornerRadius in Adjacency.hpp). Clamp again against
        // windowBox, which is scaled and rounded and can differ slightly from that logical
        // size (min-size floor, rounding to whole scaled pixels).
        double       r    = std::max(0.0, liveRadiusLogicalPx) * monitorScale;
        const double maxR = std::floor(std::min(windowBox.width, windowBox.height) / 2.0);
        r                 = std::clamp(r, 0.0, maxR);

        const int round = static_cast<int>(std::lround(r));
        if (round <= 0)
            return SCornerPatch{};

        const double size = std::ceil(r); // >= round, <= maxR (maxR is integral)
        const double x    = left ? windowBox.x : windowBox.x + windowBox.width - size;
        const double y    = top ? windowBox.y : windowBox.y + windowBox.height - size;

        return SCornerPatch{CBox{x, y, size, size}, round};
    }

    struct SCornerPatches {
        SCornerPatch topLeft, topRight, bottomLeft, bottomRight;
    };

    SCornerPatches computeCornerPatches(const CBox& windowBox, const SeamState::SLiveCorners& radii, double scale) {
        return SCornerPatches{
            makeCornerPatch(windowBox, true, true, radii.topLeft, scale),
            makeCornerPatch(windowBox, false, true, radii.topRight, scale),
            makeCornerPatch(windowBox, true, false, radii.bottomLeft, scale),
            makeCornerPatch(windowBox, false, false, radii.bottomRight, scale),
        };
    }

    // Subtracts each non-empty corner patch from `savedDamage`, giving the region the body
    // paints into: everything except the 4 corner squares.
    CRegion pruneDamageForPatches(const CRegion& savedDamage, const SCornerPatches& patches) {
        CRegion pruned = savedDamage.copy();
        if (patches.topLeft.round > 0)
            pruned.subtract(CRegion(patches.topLeft.box));
        if (patches.topRight.round > 0)
            pruned.subtract(CRegion(patches.topRight.box));
        if (patches.bottomLeft.round > 0)
            pruned.subtract(CRegion(patches.bottomLeft.box));
        if (patches.bottomRight.round > 0)
            pruned.subtract(CRegion(patches.bottomRight.box));
        return pruned;
    }

    struct SUVMapping {
        Vector2D topLeft, bottomRight;
    };

    // Redraws each corner with its own live radius, limited to that corner's box (intersected
    // with this frame's damage). Calling Hyprland's rounded-rect shader once per corner is
    // what gives each corner its own radius.
    void drawCorners(const SCornerPatches& patches, const CSurfacePassElement::SRenderData& data, const CBox& windowBox, const CRegion& savedDamage,
                      const SUVMapping& uv, float roundingPower) {
        auto        PSURFACE = Desktop::View::CWLSurface::fromResource(data.surface);
        const float ALPHA    = data.alpha * data.fadeAlpha * (PSURFACE ? PSURFACE->m_alphaModifier : 1.F);
        const float OVERALLA = PSURFACE ? PSURFACE->m_overallOpacity : 1.F;

        auto drawCorner = [&](const SCornerPatch& patch) {
            if (patch.round <= 0)
                return;

            CRegion cornerRegion = savedDamage.copy().intersect(CRegion(patch.box));
            if (cornerRegion.empty())
                return;

            Render::GL::g_pHyprOpenGL->renderTexture(data.texture, windowBox,
                                                      Render::GL::CHyprOpenGLImpl::STextureRenderData{
                                                          .damage        = &cornerRegion,
                                                          .surface       = data.surface,
                                                          .a             = ALPHA * OVERALLA,
                                                          .round         = patch.round,
                                                          .roundingPower = roundingPower,
                                                          .allowCustomUV = true,
                                                          .wrapX         = data.wrapX,
                                                          .wrapY         = data.wrapY,
                                                          .primarySurfaceUVTopLeft     = uv.topLeft,
                                                          .primarySurfaceUVBottomRight = uv.bottomRight,
                                                      });
        };

        drawCorner(patches.topLeft);
        drawCorner(patches.topRight);
        drawCorner(patches.bottomLeft);
        drawCorner(patches.bottomRight);
    }

    bool forceRoundRiskySurfaces() {
        return vars.forceRoundRiskySurfaces && vars.forceRoundRiskySurfaces->value();
    }

    // The window's frame box (data.pos + data.w/h: the main surface's box without the
    // small-surface centering), in the same monitor-scaled, rounded pixel space as
    // hkDrawSurface's windowBox. Built the same way getTexBox() builds its box.
    CBox windowFrameBox(const CSurfacePassElement::SRenderData& data) {
        const auto& mon = data.pMonitor;
        CBox        box{sc<int>(-mon->m_position.x) + data.pos.x, sc<int>(-mon->m_position.y) + data.pos.y, data.w, data.h};
        box.scale(mon->m_scale);
        box.round();
        return box;
    }

    // For a subsurface (only with force_round_risky_surfaces on): keep a corner's live radius
    // only where the subsurface's corner sits on the window's corner, and zero the rest. A
    // subsurface that reaches a window corner covers the main surface's rounded corner, so it
    // needs the same cut or the window looks square. One that reaches no corner (a video, a
    // toolbar strip in the middle) must not be rounded. The 1px slack absorbs rounding
    // differences between the two boxes.
    SeamState::SLiveCorners subsurfaceCorners(const SeamState::SLiveCorners& live, const CBox& surfBox, const CBox& frame) {
        auto near = [](double a, double b) { return std::abs(a - b) <= 1.0; };

        const bool L = near(surfBox.x, frame.x);
        const bool R = near(surfBox.x + surfBox.width, frame.x + frame.width);
        const bool T = near(surfBox.y, frame.y);
        const bool B = near(surfBox.y + surfBox.height, frame.y + frame.height);

        return SeamState::SLiveCorners{
            .topLeft     = L && T ? live.topLeft : 0.0,
            .topRight    = R && T ? live.topRight : 0.0,
            .bottomLeft  = L && B ? live.bottomLeft : 0.0,
            .bottomRight = R && B ? live.bottomRight : 0.0,
        };
    }

    // Copies drawSurface's MISALIGNEDFSV1 check (ElementRenderer.cpp, 0.56.2) from the same
    // public fields drawSurface reads, so calculateUVForSurface gets the same
    // `fixMisalignedFSV1` argument the body draw used.
    bool misalignedFSv1(const CSurfacePassElement::SRenderData& data, const CBox& windowBox) {
        const auto& surf = data.surface->m_current;

        const bool  interactiveResizeInProgress =
            data.pWindow && g_layoutManager->dragController()->target() && g_layoutManager->dragController()->mode() == MBIND_RESIZE;

        return std::floor(data.pMonitor->m_scale) != data.pMonitor->m_scale && surf.scale == 1 && windowBox.size() != surf.bufferSize &&
            DELTALESSTHAN(windowBox.width, surf.bufferSize.x, 3) && DELTALESSTHAN(windowBox.height, surf.bufferSize.y, 3) &&
            (!data.pWindow || (!data.pWindow->sizeAnimation()->isBeingAnimated() && !interactiveResizeInProgress)) &&
            (!data.pLS || (!data.pLS->sizeAnimation()->isBeingAnimated()));
    }

    // Reproduces the UV mapping the body paint used. drawSurface computes it with
    // calculateUVForSurface into m_renderData.primarySurfaceUV*, then a scope guard resets
    // those to (-1,-1) on exit. So call it again with the same arguments (same windowBox,
    // unscaled tex-box size, and the MISALIGNEDFSV1 copy), read the result, and reset the
    // globals to the "no custom UV" state. This covers every case calculateUV handles
    // (viewporter source crops, expand_undersized_textures, the resize/animation RATIO crop,
    // fractional-scale misalignment), so the corners sample the same texels as the body next
    // to them, even while the buffer size lags the animated box.
    SUVMapping recomputeUV(void* thisptr, const CSurfacePassElement::SRenderData& data, const CBox& windowBox, const Vector2D& projSizeUnscaled,
                           Render::SRenderData& renderData) {
        (*g_calculateUV)(thisptr, data.pWindow, data.surface, data.pMonitor.lock(), data.mainSurface, windowBox.size(), projSizeUnscaled,
                          misalignedFSv1(data, windowBox));
        const SUVMapping uv{renderData.primarySurfaceUVTopLeft, renderData.primarySurfaceUVBottomRight};
        renderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
        renderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
        return uv;
    }

    void hkDrawSurface(void* thisptr, WP<CSurfacePassElement> element, const CRegion& damage) {
        // Never .lock() this WP. It points at a UP<>-owned render-pass element, and
        // WP::lock() asserts on a CUniquePointer. Use expired()/get(), and don't keep the
        // raw pointer past this call.
        const auto callOriginal = [&]() { (*reinterpret_cast<drawSurface_t>(g_hook->m_original))(thisptr, element, damage); };

        if (element.expired()) {
            callOriginal();
            return;
        }

        auto* el   = element.get();
        auto& data = el->m_data;

        // Only window surfaces. Popups and layer-shell surfaces (popup true, or pWindow null)
        // go straight to the native draw.
        if (!data.pWindow || data.popup || !data.pMonitor || !data.texture || !data.surface) {
            callOriginal();
            return;
        }

        // Subsurfaces are drawn natively (square) unless force_round_risky_surfaces is on. This
        // check is why Firefox/Zen stay square: their page content is a full-window subsurface.
        const bool isSubsurface = !data.mainSurface;
        if (isSubsurface && !forceRoundRiskySurfaces()) {
            callOriginal();
            return;
        }

        // Hyprland has decided this surface isn't rounded this frame. Renderer.cpp sets
        // dontRound for a window in internal FSMODE_FULLSCREEN, and it's the SRenderData
        // default for anything renderWindow doesn't round. Draw it natively.
        if (data.dontRound) {
            callOriginal();
            return;
        }

        // drawSurface returns early if the texture failed to upload, but the renderTexture
        // calls below would RASSERT on a bad texture, so hand it to the original instead.
        if (!data.texture->ok()) {
            callOriginal();
            return;
        }

        const auto corners = SeamState::liveCornersFor(data.pWindow);
        if (!corners) {
            callOriginal();
            return;
        }

        auto& renderData = g_pHyprRenderer->m_renderData;

        // preDrawSurface has already copied element->m_data.clipBox into
        // g_pHyprRenderer->m_renderData.clipBox. When it's set (for example, a floating window
        // during a workspace slide), renderTextureInternal ignores damage and clips to
        // clipBox∩clipRegion. A single CBox can't express "window minus 4 corner squares", so
        // draw natively, with square corners, for that frame.
        if (!renderData.clipBox.empty()) {
            callOriginal();
            return;
        }

        // The same windowBox drawSurface computes. getTexBox() is cached on the element after
        // its first call in a frame, so this call and the body draw see the same box.
        CBox windowBox = el->getTexBox();
        windowBox.scale(data.pMonitor->m_scale);
        windowBox.round();

        if (windowBox.width <= 1 || windowBox.height <= 1) {
            callOriginal();
            return;
        }

        // A main surface gets all four live corners. A subsurface gets only the corners it
        // shares with the window, and one that shares none is drawn natively.
        const SeamState::SLiveCorners radii = isSubsurface ? subsurfaceCorners(*corners, windowBox, windowFrameBox(data)) : *corners;
        if (isSubsurface && radii.topLeft <= 0 && radii.topRight <= 0 && radii.bottomLeft <= 0 && radii.bottomRight <= 0) {
            callOriginal();
            return;
        }

        static auto  PPOWER       = CConfigValue<Config::FLOAT>("plugin:seam:rounding_power");
        const float  roundingPower = *PPOWER;
        const double scale         = data.pMonitor->m_scale;

        const SCornerPatches patches = computeCornerPatches(windowBox, radii, scale);

        // Save the region the original draws into (m_renderData.damage, not the unused
        // `damage` parameter), subtract the 4 corner boxes, and force the body to paint as a
        // plain square. Both are restored right after the call: later pass elements in this
        // frame read m_renderData.damage, and the corner draws intersect against the
        // unpruned damage.
        const CRegion savedDamage    = renderData.damage;
        const bool    savedDontRound = data.dontRound;

        renderData.damage = pruneDamageForPatches(savedDamage, patches);
        data.dontRound    = true;

        callOriginal();

        renderData.damage = savedDamage;
        data.dontRound    = savedDontRound;

        const Vector2D   projSizeUnscaled = el->getTexBox().size();
        const SUVMapping uv               = recomputeUV(thisptr, data, windowBox, projSizeUnscaled, renderData);

        drawCorners(patches, data, windowBox, savedDamage, uv, roundingPower);
    }

    // ---------------------------------------------------------------------------------------
    // Second hook: CSurfacePassElement::opaqueRegion(), which fixes black or stale corners.
    //
    // From /usr/include/hyprland/src/render/pass/SurfacePassElement.hpp (0.56.2):
    //   virtual CRegion CSurfacePassElement::opaqueRegion();   // public, virtual, no params
    // and the exported symbol `CSurfacePassElement::opaqueRegion()` (nm -DC /usr/bin/Hyprland).
    // It returns a CRegion by value, which isn't trivially copyable, so the Itanium ABI passes
    // a hidden result slot in rdi and moves `this` to rsi. Disassembling the installed
    // function shows rdi kept as the result slot and [rsi+0x50] read as m_data.surface. A free
    // function `CRegion f(void*)` lowers the same way (sret in rdi, the void* in rsi), so the
    // same C++ return type on both sides gives a matching ABI. Hooking the function body
    // instead of the vtable slot covers every virtual call, since the vtable points at this
    // body. CSurfacePassElement inherits singly and non-virtually from IPassElement, so `this`
    // needs no adjustment.
    //
    // With `decoration:rounding = 0`, m_data.rounding is 0 and the original reports the whole
    // window box as opaque. CRenderPass::simplify() subtracts that from the damage of
    // everything underneath (wallpaper, other windows, the clear pass), so the corner cutouts
    // hkDrawSurface leaves are never repainted and show black or stale content. Subtracting
    // each live corner's box from the region gets the backdrop under the corners drawn again.
    // A smaller opaque region only causes extra drawing underneath, so it's always safe.
    using opaqueRegion_t = CRegion (*)(void* thisptr);

    constexpr const char* OPAQUE_DEMANGLED = "CSurfacePassElement::opaqueRegion()";

    CFunctionHook* g_opaqueHook = nullptr;

    CRegion hkOpaqueRegion(void* thisptr) {
        CRegion region = (*reinterpret_cast<opaqueRegion_t>(g_opaqueHook->m_original))(thisptr);
        if (region.empty())
            return region;

        auto*       el   = static_cast<CSurfacePassElement*>(thisptr);
        const auto& data = el->m_data;

        // Same filter as hkDrawSurface: only surfaces the render hook may round. dontRound
        // surfaces (internal fullscreen) are drawn square, so their native opaque region is
        // already right, and a fullscreen window keeps fully occluding what's beneath it.
        if (!data.pWindow || data.popup || data.dontRound || !data.pMonitor)
            return region;
        if (!data.mainSurface && !forceRoundRiskySurfaces())
            return region;

        const auto corners = SeamState::liveCornersFor(data.pWindow);
        if (!corners)
            return region;

        // opaqueRegion() is in monitor-local logical coordinates (PassElement.hpp), and
        // getTexBox() is the logical box hkDrawSurface scales and rounds into its windowBox.
        // simplify() later scales and rounds every rect, and hkDrawSurface's patches are
        // ceil(radius * scale) scaled px anchored on a rounded edge, so each cut-out is padded
        // by 2 logical px and snapped outward to whole logical px. Cutting too much paints a
        // few extra backdrop pixels that the corner draw then covers; cutting too little leaves
        // a black or stale fringe.
        // A subsurface (force_round_risky_surfaces only) gets the window's corner boxes cut
        // out, since that's where hkDrawSurface may round it. For a subsurface that doesn't
        // reach a corner, that only costs a little extra backdrop drawing.
        const CBox texBox = data.mainSurface ? el->getTexBox() : CBox{data.pos - data.pMonitor->m_position, Vector2D{data.w, data.h}};
        if (texBox.width <= 0 || texBox.height <= 0)
            return region;

        auto cut = [&](double radius, bool left, bool top) {
            if (!(radius > 0.0))
                return;
            const double size = std::ceil(radius) + 2.0;
            const double x0   = std::floor(left ? texBox.x : texBox.x + texBox.width - size);
            const double y0   = std::floor(top ? texBox.y : texBox.y + texBox.height - size);
            const double x1   = std::ceil(left ? texBox.x + size : texBox.x + texBox.width);
            const double y1   = std::ceil(top ? texBox.y + size : texBox.y + texBox.height);
            region.subtract(CRegion(CBox{x0, y0, x1 - x0, y1 - y0}));
        };

        cut(corners->topLeft, true, true);
        cut(corners->topRight, false, true);
        cut(corners->bottomLeft, true, false);
        cut(corners->bottomRight, false, false);

        return region;
    }
} // namespace

namespace {
    enum class eLookup {
        Found,
        NotFound,
        Ambiguous,
    };

    const CHyprColor kErrorColor{1.0, 0.2, 0.2, 1.0};
    const CHyprColor kWarnColor{1.0, 0.6, 0.2, 1.0};
    constexpr int     kNotifyTimeoutMs = 8000;

    struct SLookupResult {
        eLookup status;
        void*   address = nullptr;
    };

    // Finds the one function whose demangled name equals `demangled`. Returns Ambiguous if
    // there is more than one exact match.
    SLookupResult findExact(const std::string& shortName, const char* demangled) {
        void* found = nullptr;
        for (const auto& m : HyprlandAPI::findFunctionsByName(PHANDLE, shortName)) {
            Log::logger->log(Log::DEBUG, "[hypr-seam] {} candidate: {} @ {}", shortName, m.demangled, m.address);
            if (m.demangled != demangled)
                continue;
            if (found)
                return SLookupResult{eLookup::Ambiguous};
            found = m.address;
        }
        return found ? SLookupResult{eLookup::Found, found} : SLookupResult{eLookup::NotFound};
    }

    bool reportLookupFailure(eLookup result, const std::string& what) {
        if (result == eLookup::Found)
            return false;

        const std::string msg = result == eLookup::Ambiguous ?
            std::format("[hypr-seam] Found more than one exact match for {} — refusing to guess which to use. Rendering will not work.", what) :
            std::format("[hypr-seam] Could not locate {} to hook — rendering will not work.", what);
        Log::logger->log(Log::ERR, "{}", msg);
        HyprlandAPI::addNotification(PHANDLE, msg, kErrorColor, kNotifyTimeoutMs);
        return true;
    }
}

bool SeamHook::install() {
    const auto drawSurfaceLookup = findExact("drawSurface", TARGET_DEMANGLED);
    if (reportLookupFailure(drawSurfaceLookup.status, "drawSurface"))
        return false;
    void* target = drawSurfaceLookup.address;

    // Also required. Without it the corner patches can't reproduce the body's UV mapping
    // (see hkDrawSurface), so skip the render hook instead of drawing mismatched corners.
    const auto calcUVLookup = findExact("calculateUVForSurface", CALCUV_DEMANGLED);
    if (reportLookupFailure(calcUVLookup.status, "calculateUVForSurface"))
        return false;
    g_calculateUV = reinterpret_cast<calculateUV_t>(calcUVLookup.address);

    g_hook = HyprlandAPI::createFunctionHook(PHANDLE, target, reinterpret_cast<void*>(&hkDrawSurface));
    if (!g_hook || !g_hook->hook()) {
        Log::logger->log(Log::ERR, "[hypr-seam] failed to install drawSurface hook");
        HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] Failed to hook drawSurface — rendering will not work.", kErrorColor, kNotifyTimeoutMs);
        if (g_hook)
            HyprlandAPI::removeFunctionHook(PHANDLE, g_hook);
        g_hook        = nullptr;
        g_calculateUV = nullptr;
        return false;
    }

    Log::logger->log(Log::DEBUG, "[hypr-seam] drawSurface hook installed @ {}", target);

    // opaqueRegion hook (black/stale corner fix). If it can't be installed, corners still
    // render but may show black or stale content behind opaque windows, so warn and keep
    // the render hook.
    const auto  opaqueLookup = findExact("opaqueRegion", OPAQUE_DEMANGLED);
    void* const opaqueTarget = opaqueLookup.address;
    if (opaqueLookup.status != eLookup::Found) {
        const std::string msg = opaqueLookup.status == eLookup::Ambiguous ? "[hypr-seam] Found more than one exact match for CSurfacePassElement::opaqueRegion — not hooking it; corners may show black/stale content." :
                                                                             "[hypr-seam] Could not locate CSurfacePassElement::opaqueRegion — corners may show black/stale content.";
        Log::logger->log(Log::ERR, "{}", msg);
        HyprlandAPI::addNotification(PHANDLE, msg, kWarnColor, kNotifyTimeoutMs);
        return true;
    }

    g_opaqueHook = HyprlandAPI::createFunctionHook(PHANDLE, opaqueTarget, reinterpret_cast<void*>(&hkOpaqueRegion));
    if (!g_opaqueHook || !g_opaqueHook->hook()) {
        Log::logger->log(Log::ERR, "[hypr-seam] failed to install opaqueRegion hook");
        HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] Failed to hook opaqueRegion — corners may show black/stale content.", kWarnColor, kNotifyTimeoutMs);
        if (g_opaqueHook)
            HyprlandAPI::removeFunctionHook(PHANDLE, g_opaqueHook);
        g_opaqueHook = nullptr;
        return true;
    }

    Log::logger->log(Log::DEBUG, "[hypr-seam] opaqueRegion hook installed @ {}", opaqueTarget);
    return true;
}

void SeamHook::remove() {
    if (g_opaqueHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_opaqueHook);
    g_opaqueHook = nullptr;
    if (g_hook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_hook);
    g_hook        = nullptr;
    g_calculateUV = nullptr;
}
