// Real per-corner rendering hook on Render::IElementRenderer::drawSurface (Task 7).
//
// See docs/hook-notes.md (Task 6) for the confirmed signature, calling convention, and the
// three "viable levers" this body is built from. Summary of the approach (lever 3 there):
//
//   1. Paint the main window body once, through the real original, with its damage region
//      temporarily shrunk to exclude the 4 live corner boxes, and `dontRound` forced on so
//      the body paints as a plain square everywhere except those corners (native rounding
//      is globally forced to 0 anyway via `decoration:rounding = 0`).
//   2. Separately, once per corner, redraw the SAME window texture via Hyprland's own
//      g_pHyprOpenGL->renderTexture(), restricted (via a damage region containing only that
//      corner's box) to just that corner, passing THAT corner's own live radius as `.round`.
//      Hyprland's own rounded-rect shader does the rest — no custom GLSL needed.
//
//   3. The corner draws reuse the exact UV mapping the body paint used, recomputed through
//      Hyprland's own (exported) IElementRenderer::calculateUVForSurface, so corners and body
//      always sample the same texels — including during open/resize/reflow animations, when
//      the client's buffer size lags the animated window box on nearly every frame.
//
// This relies on g_pHyprRenderer->m_renderData.damage being the thing that actually
// restricts drawing (confirmed empirically + from source: the `damage` *parameter* to
// drawSurface is never read). It does NOT work when m_renderData.clipBox is non-empty
// (e.g. a floating window mid slide-animation) — in that rare case we fall back to the
// native, square-cornered draw for that one frame rather than risk a corrupted clip.
// Surfaces Hyprland itself marks dontRound (internal fullscreen) are also left native.
//
// Subsurfaces are left native by default too. That is why Firefox-based browsers (Firefox,
// Zen, ...) stay square: they draw their whole page into a wl_subsurface that exactly covers
// the main surface, so the main surface's rounded corners are hidden underneath it. With
// plugin:seam:force_round_risky_surfaces = true, a subsurface that reaches one or more of the
// window's own corners gets those corners rounded too (see subsurfaceCorners()).
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

    // Confirmed against /usr/include/hyprland/src/render/ElementRenderer.hpp (Hyprland 0.56.2):
    //   void Render::IElementRenderer::drawSurface(WP<CSurfacePassElement> element, const CRegion& damage);
    // A private, NON-virtual member function. Under the Itanium C++ ABI a non-virtual
    // member is called exactly like a free function with the object pointer prepended
    // as the first argument, so `this` is modelled as an explicit leading void*.
    // WP<> is non-trivially-copyable, so it is passed by invisible reference in both
    // the original and our replacement; declaring the same C++ parameter types makes
    // the compiler produce a matching ABI on both sides.
    using drawSurface_t = void (*)(void* thisptr, WP<CSurfacePassElement> element, const CRegion& damage);

    constexpr const char* TARGET_DEMANGLED = "Render::IElementRenderer::drawSurface(Hyprutils::Memory::CWeakPointer<CSurfacePassElement>, Hyprutils::Math::CRegion const&)";

    // Confirmed against /usr/include/hyprland/src/render/ElementRenderer.hpp (0.56.2) and the
    // binary's dynamic symbol table (`nm -DC /usr/bin/Hyprland`): calculateUVForSurface is a
    // private, non-virtual member of IElementRenderer, but its symbol IS exported, so it can be
    // located via findFunctionsByName and called with the same leading-`this` convention as
    // drawSurface above. SP<> arguments are non-trivially-copyable, so they're passed by
    // invisible reference on both sides — declaring the same C++ parameter types gives a
    // matching ABI.
    using calculateUV_t = void (*)(void* thisptr, PHLWINDOW, SP<CWLSurfaceResource>, PHLMONITOR, bool main, const Vector2D& projSize, const Vector2D& projSizeUnscaled,
                                   bool fixMisalignedFSV1);

    constexpr const char* CALCUV_DEMANGLED =
        "Render::IElementRenderer::calculateUVForSurface(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Hyprutils::Memory::CSharedPointer<CWLSurfaceResource>, "
        "Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, bool, Hyprutils::Math::Vector2D const&, Hyprutils::Math::Vector2D const&, bool)";

    calculateUV_t g_calculateUV = nullptr;

    // One corner's redraw box (in the same monitor-scaled, rounded pixel space as the
    // windowBox drawSurface itself computes) plus the radius to feed Hyprland's own
    // rounded-rect shader for just that corner.
    //
    // The box is always snapped OUTWARD to whole pixels (size = ceil(radius), anchored on the
    // window's own integer edge). windowBox is already rounded to integers, so this makes the
    // box exactly integral. That matters because CRegion(CBox) hands the double coordinates
    // straight to pixman_region32_init_rect, which truncates x and width independently: with a
    // fractional radius (fractional monitor scale, or mid-animation) a right/bottom patch at
    // `edge - 16.5` became [edge-17, edge-1), leaving the outermost pixel column/row OUT of the
    // pruned-away region, so the square body paint showed through as a 1px sliver beside the
    // curve. The same integral box is used for both the damage subtraction and the corner-draw
    // restriction, so the two can never disagree. `.round` stays the (rounded) real radius,
    // since it controls the actual curve shape; the extra <1px of box beyond the curve is just
    // painted as plain body by the corner draw.
    struct SCornerPatch {
        CBox box;
        int  round = 0; // scaled px; 0 means "no rounding needed here, leave it to the square body paint"
    };

    SCornerPatch makeCornerPatch(const CBox& windowBox, bool left, bool top, double liveRadiusLogicalPx, double monitorScale) {
        // liveRadiusLogicalPx comes from SeamState::liveCornersFor, already clamped against
        // the window's logical size (Task 5's clampCornerRadius). Re-clamp here defensively
        // against windowBox's own (scaled, rounded-to-int) dimensions anyway, since windowBox
        // can differ very slightly from the logical size SeamState clamped against (min-size
        // floor, rounding to whole scaled pixels).
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

    // Subtracts each non-empty corner patch's box from `savedDamage`, producing the region
    // the main body should actually paint into (everything except the 4 corner squares).
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

    // Redraws each corner with its own live radius, restricted to just that corner's box via
    // a damage region containing only it (intersected with what was actually damaged this
    // frame). Hyprland's own rounded-rect shader (invoked once per corner with that corner's
    // own radius) is the per-corner-radius mechanism — no custom GLSL needed.
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

    // The window's own frame box (data.pos + data.w/h, i.e. the main surface's box without
    // the small-surface centering), in the same monitor-scaled, rounded pixel space as
    // hkDrawSurface's windowBox. Built the same way getTexBox() builds a box.
    CBox windowFrameBox(const CSurfacePassElement::SRenderData& data) {
        const auto& mon = data.pMonitor;
        CBox        box{sc<int>(-mon->m_position.x) + data.pos.x, sc<int>(-mon->m_position.y) + data.pos.y, data.w, data.h};
        box.scale(mon->m_scale);
        box.round();
        return box;
    }

    // For a subsurface (only reached with force_round_risky_surfaces on): keep a corner's live
    // radius only where the subsurface's own corner sits on the window's corner, zero the rest.
    // A subsurface that reaches a window corner covers the main surface's rounded corner there,
    // so it has to be cut the same way or the window looks square. A subsurface that doesn't
    // reach a corner (a video, a toolbar strip in the middle) must NOT be rounded at all. The
    // 1px slack absorbs rounding differences between the two boxes.
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

    // Replicates drawSurface's own MISALIGNEDFSV1 boolean exactly (ElementRenderer.cpp,
    // confirmed against the pinned 0.56.2 source) from the same public fields drawSurface
    // reads, so we can hand calculateUVForSurface the same `fixMisalignedFSV1` argument the
    // real body draw used.
    bool misalignedFSv1(const CSurfacePassElement::SRenderData& data, const CBox& windowBox) {
        const auto& surf = data.surface->m_current;

        const bool  interactiveResizeInProgress =
            data.pWindow && g_layoutManager->dragController()->target() && g_layoutManager->dragController()->mode() == MBIND_RESIZE;

        return std::floor(data.pMonitor->m_scale) != data.pMonitor->m_scale && surf.scale == 1 && windowBox.size() != surf.bufferSize &&
            DELTALESSTHAN(windowBox.width, surf.bufferSize.x, 3) && DELTALESSTHAN(windowBox.height, surf.bufferSize.y, 3) &&
            (!data.pWindow || (!data.pWindow->sizeAnimation()->isBeingAnimated() && !interactiveResizeInProgress)) &&
            (!data.pLS || (!data.pLS->sizeAnimation()->isBeingAnimated()));
    }

    // Reproduces the exact UV mapping the body paint just used. drawSurface computes it via
    // calculateUVForSurface into m_renderData.primarySurfaceUV* and then resets those back
    // to (-1,-1) in a scope guard on exit, so it's gone by now — recompute it with the very
    // same arguments drawSurface passed (same windowBox, unscaled tex-box size, and our
    // exact MISALIGNEDFSV1 replica), read it out, and put the globals back to the "no
    // custom UV" state drawSurface left them in. This covers every case calculateUV
    // handles (viewporter source crops, expand_undersized_textures, the resize/animation
    // RATIO crop, fractional-scale misalignment) without guessing, so the corners always
    // sample the same texels as the body right next to them — including during
    // open/resize/reflow animations, where the buffer size lags the animated box on
    // nearly every frame.
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
        // Never .lock() this WP: it points at a UP<>-owned render-pass element, and
        // WP::lock() hard-asserts over a CUniquePointer (docs/hook-notes.md). Use
        // expired()/get() instead, and never retain the raw pointer past this call.
        const auto callOriginal = [&]() { (*reinterpret_cast<drawSurface_t>(g_hook->m_original))(thisptr, element, damage); };

        if (element.expired()) {
            callOriginal();
            return;
        }

        auto* el   = element.get();
        auto& data = el->m_data;

        // Only act on window surfaces. Popups and layer-shell surfaces (pWindow null, or popup
        // true) always pass straight through to native handling untouched
        // (docs/hook-notes.md).
        if (!data.pWindow || data.popup || !data.pMonitor || !data.texture || !data.surface) {
            callOriginal();
            return;
        }

        // Subsurfaces are native (square) unless force_round_risky_surfaces is on. This is the
        // check that leaves Firefox/Zen square: their page content is a full-window subsurface.
        const bool isSubsurface = !data.mainSurface;
        if (isSubsurface && !forceRoundRiskySurfaces()) {
            callOriginal();
            return;
        }

        // Hyprland already decided this surface must not be rounded at all this frame
        // (Renderer.cpp sets dontRound for a window in internal FSMODE_FULLSCREEN; it's also
        // the SRenderData default for anything renderWindow doesn't explicitly round). Respect
        // that: draw exactly as native would, with no per-corner rounding of our own.
        if (data.dontRound) {
            callOriginal();
            return;
        }

        // The real drawSurface discards early if the texture failed to upload; our own
        // renderTexture calls below would instead hard RASSERT on a not-ok texture. Bail
        // out the same way the original does rather than risk that.
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

        // preDrawSurface already copied element->m_data.clipBox into
        // g_pHyprRenderer->m_renderData.clipBox before this hook ran. When it's non-empty
        // (e.g. a floating window mid workspace-slide-animation), renderTextureInternal
        // bypasses damage entirely and clips to clipBox∩clipRegion instead — a single CBox
        // that can't express "window minus 4 corner squares". Fall back to the native,
        // square-cornered draw for that one frame rather than risk a corrupted clip
        // (docs/hook-notes.md, lever 2's caveat).
        if (!renderData.clipBox.empty()) {
            callOriginal();
            return;
        }

        // Same windowBox drawSurface itself computes right before using it (getTexBox() is
        // cached on the element after the first call within this frame, so our call here and
        // the real body's later call both see the identical box).
        CBox windowBox = el->getTexBox();
        windowBox.scale(data.pMonitor->m_scale);
        windowBox.round();

        if (windowBox.width <= 1 || windowBox.height <= 1) {
            callOriginal();
            return;
        }

        // Main surface: all four live corners. Subsurface: only the corners it shares with the
        // window; one that reaches none of them is drawn natively.
        const SeamState::SLiveCorners radii = isSubsurface ? subsurfaceCorners(*corners, windowBox, windowFrameBox(data)) : *corners;
        if (isSubsurface && radii.topLeft <= 0 && radii.topRight <= 0 && radii.bottomLeft <= 0 && radii.bottomRight <= 0) {
            callOriginal();
            return;
        }

        static auto  PPOWER       = CConfigValue<Config::FLOAT>("plugin:seam:rounding_power");
        const float  roundingPower = *PPOWER;
        const double scale         = data.pMonitor->m_scale;

        const SCornerPatches patches = computeCornerPatches(windowBox, radii, scale);

        // Save the actual region the original draws into (the `damage` parameter is never
        // read by drawSurface — confirmed in docs/hook-notes.md), subtract the 4 corner
        // boxes, and force the main body to paint as a plain square. Restored immediately
        // after the call below: later pass elements in this same frame rely on
        // m_renderData.damage, and our own corner draws need the ORIGINAL (unpruned) damage
        // to intersect against, not the pruned one.
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
    // Second hook: CSurfacePassElement::opaqueRegion() (black/stale-corner fix).
    //
    // Confirmed against /usr/include/hyprland/src/render/pass/SurfacePassElement.hpp (0.56.2):
    //   virtual CRegion CSurfacePassElement::opaqueRegion();   // public, VIRTUAL, no params
    // and the exported symbol `CSurfacePassElement::opaqueRegion()` (nm -DC /usr/bin/Hyprland).
    // Different convention from drawSurface: it RETURNS a non-trivially-copyable CRegion by
    // value, so the Itanium ABI returns it through a hidden caller-allocated slot passed in
    // rdi, with `this` moved to rsi (confirmed by disassembling the installed function:
    // rdi is kept as the result slot, [rsi+0x50] is read as m_data.surface). A free function
    // `CRegion f(void*)` lowers identically (sret in rdi, the void* in rsi), so declaring the
    // same C++ return type on both sides gives a matching ABI. Hooking the function body
    // (rather than the vtable slot) covers every virtual dispatch, since the vtable points at
    // this body; CSurfacePassElement has single, non-virtual inheritance from IPassElement,
    // so `this` is the CSurfacePassElement* with no adjustment.
    //
    // Why: with `decoration:rounding = 0`, m_data.rounding is 0, so the original reports the
    // whole window box as opaque. CRenderPass::simplify() then subtracts that from the damage
    // of everything underneath (wallpaper, other windows, the clear pass), so the corner
    // cutouts our drawSurface hook leaves are never repainted and show black/stale content.
    // We subtract each live corner's box from the reported region so the backdrop under the
    // corners is drawn again. Making the opaque region SMALLER is always safe: it can only
    // cause extra (correct) drawing underneath, never missing drawing.
    using opaqueRegion_t = CRegion (*)(void* thisptr);

    constexpr const char* OPAQUE_DEMANGLED = "CSurfacePassElement::opaqueRegion()";

    CFunctionHook* g_opaqueHook = nullptr;

    CRegion hkOpaqueRegion(void* thisptr) {
        CRegion region = (*reinterpret_cast<opaqueRegion_t>(g_opaqueHook->m_original))(thisptr);
        if (region.empty())
            return region;

        auto*       el   = static_cast<CSurfacePassElement*>(thisptr);
        const auto& data = el->m_data;

        // Same filter as hkDrawSurface: only main window surfaces our render hook may round.
        // dontRound surfaces (internal fullscreen) are drawn square by hkDrawSurface, so their
        // native opaque region is already correct; leave them alone so a fullscreen window
        // keeps occluding everything beneath it at full efficiency.
        if (!data.pWindow || data.popup || data.dontRound || !data.pMonitor)
            return region;
        if (!data.mainSurface && !forceRoundRiskySurfaces())
            return region;

        const auto corners = SeamState::liveCornersFor(data.pWindow);
        if (!corners)
            return region;

        // opaqueRegion() is in monitor-local LOGICAL coordinates (PassElement.hpp); getTexBox()
        // is the same logical box hkDrawSurface scales/rounds into its windowBox. simplify()
        // later scales every rect by the monitor scale and rounds it, and hkDrawSurface's own
        // patches are ceil(radius * scale) scaled px anchored on a rounded edge, so pad each
        // cut-out by 2 logical px and snap it outward to whole logical px. Over-cutting a sliver
        // only means a few extra backdrop pixels get painted (then covered by our corner draw);
        // under-cutting would leave a black/stale fringe.
        // A subsurface (force_round_risky_surfaces only) gets the WINDOW's corner boxes cut out,
        // not its own: those are where hkDrawSurface may round it. Cutting them from a
        // subsurface that doesn't actually reach a corner just costs a little extra backdrop
        // drawing, which is always safe.
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

    // Finds exactly one function whose demangled name equals `demangled`. Refuses (Ambiguous)
    // rather than guessing if more than one exact match exists.
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

    // Required too: without it the corner patches can't reproduce the body's UV mapping (see
    // hkDrawSurface), so don't install the render hook at all rather than draw mismatched
    // corners.
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

    // opaqueRegion hook (black/stale-corner fix). Not fatal if it can't be installed: the
    // corners still render, they just may show black/stale content behind opaque windows.
    // Warn loudly instead of tearing down the working render hook.
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
