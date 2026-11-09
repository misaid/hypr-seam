// Per-corner rounding via Render::IElementRenderer::drawSurface.
#define WLR_USE_UNSTABLE

#include "SeamHook.hpp"
#include "SeamState.hpp"
#include "globals.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
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

    // ElementRenderer.hpp (0.57.0): private non-virtual member; WP<> passed by invisible reference.
    using drawSurface_t = void (*)(void* thisptr, Render::CRenderContext& ctx, WP<CSurfacePassElement> element, const CRegion& damage);

    constexpr const char* TARGET_DEMANGLED =
        "Render::IElementRenderer::drawSurface(Render::CRenderContext&, Hyprutils::Memory::CWeakPointer<CSurfacePassElement>, Hyprutils::Math::CRegion const&)";

    // IElementRenderer::calculateUVForSurface (0.57.0); SP<> passed by invisible reference.
    using calculateUV_t = void (*)(void* thisptr, Render::CRenderContext& ctx, PHLWINDOW, SP<CWLSurfaceResource>, PHLMONITOR, bool main, const Vector2D& projSize,
                                   const Vector2D& projSizeUnscaled, bool fixMisalignedFSV1);

    constexpr const char* CALCUV_DEMANGLED =
        "Render::IElementRenderer::calculateUVForSurface(Render::CRenderContext&, Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, "
        "Hyprutils::Memory::CSharedPointer<CWLSurfaceResource>, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, bool, Hyprutils::Math::Vector2D const&, "
        "Hyprutils::Math::Vector2D const&, bool)";

    calculateUV_t g_calculateUV = nullptr;

    struct SCornerPatch {
        CBox box;
        int  round = 0; // scaled px; 0 means "no rounding needed here, leave it to the square body paint"
    };

    SCornerPatch makeCornerPatch(const CBox& windowBox, bool left, bool top, double liveRadiusLogicalPx, double monitorScale) {
        double       r    = std::max(0.0, liveRadiusLogicalPx) * monitorScale;
        const double maxR = std::floor(std::min(windowBox.width, windowBox.height) / 2.0);
        r                 = std::clamp(r, 0.0, maxR);

        const int round = static_cast<int>(std::lround(r));
        if (round <= 0)
            return SCornerPatch{};

        const double size = std::ceil(r);
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

    void drawCorners(Render::CRenderContext& ctx, const SCornerPatches& patches, const CSurfacePassElement::SRenderData& data, const CBox& windowBox,
                      const CRegion& savedDamage, const SUVMapping& uv, float roundingPower) {
        auto        PSURFACE = Desktop::View::CWLSurface::fromResource(data.surface);
        const float ALPHA    = data.alpha * data.fadeAlpha * (PSURFACE ? PSURFACE->m_alphaModifier : 1.F);
        const float OVERALLA = PSURFACE ? PSURFACE->m_overallOpacity : 1.F;

        auto drawCorner = [&](const SCornerPatch& patch) {
            if (patch.round <= 0)
                return;

            CRegion cornerRegion = savedDamage.copy().intersect(CRegion(patch.box));
            if (cornerRegion.empty())
                return;

            Render::GL::g_pHyprOpenGL->renderTexture(ctx, data.texture, windowBox,
                                                      Render::GL::CHyprOpenGLImpl::STextureRenderData{
                                                          .damage        = &cornerRegion,
                                                          .surface       = data.surface,
                                                          .a             = ALPHA * OVERALLA,
                                                                                                                    .round         = std::max(0, patch.round - 1),
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

    CBox windowFrameBox(const CSurfacePassElement::SRenderData& data) {
        const auto& mon = data.pMonitor;
        CBox        box{sc<int>(-mon->m_position.x) + data.pos.x, sc<int>(-mon->m_position.y) + data.pos.y, data.w, data.h};
        box.scale(mon->m_scale);
        box.round();
        return box;
    }

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

    bool misalignedFSv1(const CSurfacePassElement::SRenderData& data, const CBox& windowBox) {
        const auto& surf = data.surface->m_current;

        const bool  interactiveResizeInProgress =
            data.pWindow && g_layoutManager->dragController()->target() && g_layoutManager->dragController()->mode() == MBIND_RESIZE;

        return std::floor(data.pMonitor->m_scale) != data.pMonitor->m_scale && surf.scale == 1 && windowBox.size() != surf.bufferSize &&
            DELTALESSTHAN(windowBox.width, surf.bufferSize.x, 3) && DELTALESSTHAN(windowBox.height, surf.bufferSize.y, 3) &&
            (!data.pWindow || (!data.pWindow->sizeAnimation()->isBeingAnimated() && !interactiveResizeInProgress)) &&
            (!data.pLS || (!data.pLS->sizeAnimation()->isBeingAnimated()));
    }

    SUVMapping recomputeUV(void* thisptr, Render::CRenderContext& ctx, const CSurfacePassElement::SRenderData& data, const CBox& windowBox, const Vector2D& projSizeUnscaled) {
        (*g_calculateUV)(thisptr, ctx, data.pWindow, data.surface, data.pMonitor.lock(), data.mainSurface, windowBox.size(), projSizeUnscaled,
                          misalignedFSv1(data, windowBox));
        const SUVMapping uv{ctx.m_data.primarySurfaceUVTopLeft, ctx.m_data.primarySurfaceUVBottomRight};
        ctx.m_data.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
        ctx.m_data.primarySurfaceUVBottomRight = Vector2D(-1, -1);
        return uv;
    }

    void hkDrawSurface(void* thisptr, Render::CRenderContext& ctx, WP<CSurfacePassElement> element, const CRegion& damage) {
        // WP elsewhere points at a UP<>-owned element; never .lock() it (asserts).
        const auto callOriginal = [&]() { (*reinterpret_cast<drawSurface_t>(g_hook->m_original))(thisptr, ctx, element, damage); };

        if (element.expired()) {
            callOriginal();
            return;
        }

        auto* el   = element.get();
        auto& data = el->m_data;

        if (!data.pWindow || data.popup || !data.pMonitor || !data.texture || !data.surface) {
            callOriginal();
            return;
        }

        const bool isSubsurface = !data.mainSurface;
        if (isSubsurface && !forceRoundRiskySurfaces()) {
            callOriginal();
            return;
        }

        if (data.dontRound) {
            callOriginal();
            return;
        }

        if (!data.texture->ok()) {
            callOriginal();
            return;
        }

        const auto corners = SeamState::liveCornersFor(data.pWindow);
        if (!corners) {
            callOriginal();
            return;
        }

        auto& renderData = ctx.m_data;

        CBox windowBox = el->getTexBox();
        windowBox.scale(data.pMonitor->m_scale);
        windowBox.round();

        if (windowBox.width <= 1 || windowBox.height <= 1) {
            callOriginal();
            return;
        }

        const SeamState::SLiveCorners radii = isSubsurface ? subsurfaceCorners(*corners, windowBox, windowFrameBox(data)) : *corners;
        if (isSubsurface && radii.topLeft <= 0 && radii.topRight <= 0 && radii.bottomLeft <= 0 && radii.bottomRight <= 0) {
            callOriginal();
            return;
        }

        static auto  PPOWER       = CConfigValue<Config::FLOAT>("plugin:seam:rounding_power");
        const float  roundingPower = *PPOWER;
        const double scale         = data.pMonitor->m_scale;

        const SCornerPatches patches = computeCornerPatches(windowBox, radii, scale);

        {
            const int r0 = patches.topLeft.round;
            if (r0 > 0 && patches.topRight.round == r0 && patches.bottomLeft.round == r0 && patches.bottomRight.round == r0) {
                const int   savedRounding = data.rounding;
                const float savedPower    = data.roundingPower;

                data.rounding      = r0;
                data.roundingPower = roundingPower;

                callOriginal();

                data.rounding      = savedRounding;
                data.roundingPower = savedPower;
                return;
            }

            if (!renderData.clipBox.empty()) {
                callOriginal();
                return;
            }
        }

        const CRegion savedDamage    = renderData.damage;
        const bool    savedDontRound = data.dontRound;

        {
            const int rounds[4] = {patches.topLeft.round, patches.topRight.round, patches.bottomLeft.round, patches.bottomRight.round};
            int best = rounds[0];
            for (int i = 1; i < 4; ++i)
                best = std::max(best, rounds[i]);
            bool allPositive = patches.topLeft.round > 0 && patches.topRight.round > 0 && patches.bottomLeft.round > 0 && patches.bottomRight.round > 0;
            if (best > 0 && allPositive) {
                SCornerPatches diff = patches;
                auto rescale = [&](SCornerPatch& pc, bool left, bool top) {
                    if (pc.round == best) { pc.round = 0; return; }
                    SCornerPatch nb = makeCornerPatch(windowBox, left, top, best / scale, scale);
                    nb.round = pc.round;
                    pc = nb;
                };
                rescale(diff.topLeft, true, true);
                rescale(diff.topRight, false, true);
                rescale(diff.bottomLeft, true, false);
                rescale(diff.bottomRight, false, false);

                const int   savedRounding = data.rounding;
                const float savedPower    = data.roundingPower;

                renderData.damage = pruneDamageForPatches(savedDamage, diff);
                data.rounding      = best;
                data.roundingPower = roundingPower;

                callOriginal();

                renderData.damage = savedDamage;
                data.rounding      = savedRounding;
                data.roundingPower = savedPower;

                const Vector2D   projSizeUnscaled = el->getTexBox().size();
                const SUVMapping uv               = recomputeUV(thisptr, ctx, data, windowBox, projSizeUnscaled);

                drawCorners(ctx, diff, data, windowBox, savedDamage, uv, roundingPower);
                return;
            }
        }

        renderData.damage = pruneDamageForPatches(savedDamage, patches);
        data.dontRound    = true;

        callOriginal();

        renderData.damage = savedDamage;
        data.dontRound    = savedDontRound;

        const Vector2D   projSizeUnscaled = el->getTexBox().size();
        const SUVMapping uv               = recomputeUV(thisptr, ctx, data, windowBox, projSizeUnscaled);

        drawCorners(ctx, patches, data, windowBox, savedDamage, uv, roundingPower);
    }

    // SurfacePassElement.hpp (0.56.2); CRegion result in rsi, `this` in rsi; mirrors opaqueRegion().
    using opaqueRegion_t = CRegion (*)(void* thisptr, Render::CRenderContext& ctx);

    constexpr const char* OPAQUE_DEMANGLED = "CSurfacePassElement::opaqueRegion(Render::CRenderContext&)";

    CFunctionHook* g_opaqueHook = nullptr;

    CRegion hkOpaqueRegion(void* thisptr, Render::CRenderContext& ctx) {
        CRegion region = (*reinterpret_cast<opaqueRegion_t>(g_opaqueHook->m_original))(thisptr, ctx);
        if (region.empty())
            return region;

        auto*       el   = static_cast<CSurfacePassElement*>(thisptr);
        const auto& data = el->m_data;

        if (!data.pWindow || data.popup || data.dontRound || !data.pMonitor)
            return region;
        if (!data.mainSurface && !forceRoundRiskySurfaces())
            return region;

        const auto corners = SeamState::liveCornersFor(data.pWindow);
        if (!corners)
            return region;

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
            Log::logger->log(Log::DEBUG, "hypr-seam", "[hypr-seam] {} candidate: {} @ {}", shortName, m.demangled, m.address);
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
        Log::logger->log(Log::ERR, msg);
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

    Log::logger->log(Log::DEBUG, "hypr-seam", "[hypr-seam] drawSurface hook installed @ {}", target);

    // opaqueRegion hook is optional; without it corners can show stale content, so warn and keep
    // the render hook.
    const auto  opaqueLookup = findExact("opaqueRegion", OPAQUE_DEMANGLED);
    void* const opaqueTarget = opaqueLookup.address;
    if (opaqueLookup.status != eLookup::Found) {
        const std::string msg = opaqueLookup.status == eLookup::Ambiguous ? "[hypr-seam] Found more than one exact match for CSurfacePassElement::opaqueRegion — not hooking it; corners may show black/stale content." :
                                                                             "[hypr-seam] Could not locate CSurfacePassElement::opaqueRegion — corners may show black/stale content.";
        Log::logger->log(Log::ERR, msg);
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

    Log::logger->log(Log::DEBUG, "hypr-seam", "[hypr-seam] opaqueRegion hook installed @ {}", opaqueTarget);
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
