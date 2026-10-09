// Opt-in hooks that round border and shadow corners, using the same render-pass-element
// pattern as hkDrawSurface in SeamHook.cpp. Each hook reads SeamState::liveCornersFor(window),
// which already accounts for seams, animation and per-app rules, and redraws the border or
// shadow in up to 4 corner passes through g_pHyprOpenGL->renderBorder()/renderRoundedShadow().
// That's how hkDrawSurface gets per-corner rounding out of renderTexture()'s single `round`.
//
// Checked against the installed Hyprland 0.56.2 headers, the upstream 0.56.2 .cpp sources
// (CHyprBorderDecoration.cpp, CHyprDropShadowDecoration.cpp, GLElementRenderer.cpp and
// OpenGL.cpp, which the dev headers don't include), and the dynamic symbol table:
//   - The exact demangled draw() overloads for CBorderPassElement/CShadowPassElement are below.
//   - g_pHyprRenderer->m_renderData.damage limits renderBorder()/renderRoundedShadow() output
//     the same way it limits drawSurface (tested with a pixel diff).
//   - decoration:shadow:color resolves through CConfigValue<Config::IComplexConfigValue> to a
//     Config::CGradientValueData.
//   - CBorderPassElement::m_data.box and the fullBox from
//     CHyprDropShadowDecoration::getRenderData() are both in monitor-scaled px rounded to int
//     (both end with `.scale(pMonitor->m_scale).round()`), the same space as hkDrawSurface's
//     windowBox. SeamState::liveCornersFor returns logical px, so both hooks multiply by the
//     monitor scale before comparing.
//   - renderBorder()'s effective inner radius is `data.round + (data.round == 0 ? 0 :
//     scaledBorderSize)`. `data.outerRound == -1` reuses that value for the outer edge instead
//     of native's whole-window outerRound, which gives one matching inner and outer curve for
//     a single corner's radius.
//   - renderBorder() draws into box.expand(scaledBorderSize), a ring that reaches
//     scaledBorderSize past the content box on every side. A corner's redraw box has to reach
//     that far out too, or the outer part of the ring stays square from the straight-edge pass.
//   - getRenderData() sets g_pHyprRenderer->m_renderData.currentWindow as a side effect (the
//     shadow shader uses it to cut the window's box out of the fill), and reposition() clears
//     it at the end of the native render(). That makes it the shadow's owner: call
//     getRenderData() once and read currentWindow, with no separate window matching. The
//     corner redraws leave it unset on purpose; see hkShadowDraw.

#include "SeamDecorHook.hpp"
#include "SeamState.hpp"
#include "globals.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/ShadowPassElement.hpp>
#include <hyprland/src/render/decorations/CHyprDropShadowDecoration.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <format>
#include <string>

namespace {
    // One corner's redraw box, in the same monitor-scaled px space as the box it came from,
    // plus the radius for that corner's shader call.
    struct SCornerBox {
        CBox box;
        int  round = 0; // 0 means "no rounding needed here, leave it to the native straight-edge paint"
    };

    struct SCornerBoxes {
        SCornerBox topLeft, topRight, bottomLeft, bottomRight;
    };

    // Subtracts each non-empty corner box from `savedDamage`, giving the region the
    // straight-edge pass paints into.
    CRegion pruneDamageForBoxes(const CRegion& savedDamage, const SCornerBoxes& boxes) {
        CRegion pruned = savedDamage.copy();
        if (boxes.topLeft.round > 0)
            pruned.subtract(CRegion(boxes.topLeft.box));
        if (boxes.topRight.round > 0)
            pruned.subtract(CRegion(boxes.topRight.box));
        if (boxes.bottomLeft.round > 0)
            pruned.subtract(CRegion(boxes.bottomLeft.box));
        if (boxes.bottomRight.round > 0)
            pruned.subtract(CRegion(boxes.bottomRight.box));
        return pruned;
    }

    // Redraws each non-empty corner box, limited to that box intersected with this frame's
    // damage. `draw` makes the render call for one corner. Like hkDrawSurface, it reads
    // g_pHyprRenderer->m_renderData through `auto&` instead of naming its type.
    template <std::invocable<const SCornerBox&> F>
    void forEachCorner(const SCornerBoxes& boxes, const CRegion& savedDamage, F&& draw) {
        auto& renderData = g_pHyprRenderer->m_renderData;
        auto  redraw      = [&](const SCornerBox& patch) {
            if (patch.round <= 0)
                return;
            CRegion cornerRegion = savedDamage.copy().intersect(CRegion(patch.box));
            if (cornerRegion.empty())
                return;
            renderData.damage = cornerRegion;
            draw(patch);
            renderData.damage = savedDamage;
        };
        redraw(boxes.topLeft);
        redraw(boxes.topRight);
        redraw(boxes.bottomLeft);
        redraw(boxes.bottomRight);
    }
} // namespace

namespace {
    CFunctionHook* g_borderHook = nullptr;

    using borderDraw_t = void (*)(void* thisptr, WP<CBorderPassElement> element, const CRegion& damage);

    // From the installed 0.56.2 headers and the dynamic symbol table.
    constexpr const char* BORDER_DEMANGLED =
        "Render::GL::CGLElementRenderer::draw(Hyprutils::Memory::CWeakPointer<CBorderPassElement>, Hyprutils::Math::CRegion const&)";

    bool roundBordersEnabled() {
        return vars.roundBorders && vars.roundBorders->value();
    }

    // Anchored on `box`, the border's content box before expansion, and extended by
    // `scaledBorderSize` past the corner's two outer edges, so the redraw box covers the whole
    // ring renderBorder() paints there (box.expand(scaledBorderSize)).
    SCornerBox makeBorderCornerBox(const CBox& box, bool left, bool top, double liveRadiusPx, double scale, int scaledBorderSize) {
        const double maxR  = std::floor(std::min(box.width, box.height) / 2.0);
        const double r     = std::clamp(liveRadiusPx * scale, 0.0, maxR);
        const int    round = static_cast<int>(std::lround(r));
        if (round <= 0)
            return SCornerBox{}; // renderBorder's effective round is 0 here too, so the native paint is already right.

        const double innerSize = std::ceil(r);
        const double size      = innerSize + scaledBorderSize;
        const double x         = left ? box.x - scaledBorderSize : box.x + box.width - innerSize;
        const double y         = top ? box.y - scaledBorderSize : box.y + box.height - innerSize;
        return SCornerBox{CBox{x, y, size, size}, round};
    }

    SCornerBoxes computeBorderCornerBoxes(const CBox& box, const SeamState::SLiveCorners& radii, double scale, int scaledBorderSize) {
        return SCornerBoxes{
            makeBorderCornerBox(box, true, true, radii.topLeft, scale, scaledBorderSize),
            makeBorderCornerBox(box, false, true, radii.topRight, scale, scaledBorderSize),
            makeBorderCornerBox(box, true, false, radii.bottomLeft, scale, scaledBorderSize),
            makeBorderCornerBox(box, false, false, radii.bottomRight, scale, scaledBorderSize),
        };
    }

    void hkBorderDraw(void* thisptr, WP<CBorderPassElement> element, const CRegion& damage) {
        const auto callOriginal = [&]() { (*reinterpret_cast<borderDraw_t>(g_borderHook->m_original))(thisptr, element, damage); };

        if (!roundBordersEnabled() || element.expired()) {
            callOriginal();
            return;
        }

        auto& data   = element.get()->m_data;
        auto  window = data.window.lock();
        if (!window) {
            callOriginal();
            return;
        }

        const auto corners = SeamState::liveCornersFor(window);
        if (!corners) {
            callOriginal();
            return;
        }

        const PHLMONITOR mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!mon) {
            callOriginal();
            return;
        }

        const double scale            = mon->m_scale;
        const int    scaledBorderSize = static_cast<int>(std::round(data.borderSize * scale));

        // Same setting hkDrawSurface uses for content corners, not the native per-window
        // roundingPower, so the border and shadow curve the same way as the content.
        static auto PPOWER       = CConfigValue<Config::FLOAT>("plugin:seam:rounding_power");
        const float roundingPower = *PPOWER;

        const SCornerBoxes boxes = computeBorderCornerBoxes(data.box, *corners, scale, scaledBorderSize);

        auto&         renderData  = g_pHyprRenderer->m_renderData;
        const CRegion savedDamage = renderData.damage;
        const int     savedRound  = data.round;

        renderData.damage = pruneDamageForBoxes(savedDamage, boxes);
        data.round         = 0;

        callOriginal();

        renderData.damage = savedDamage;
        data.round         = savedRound;

        forEachCorner(boxes, savedDamage, [&](const SCornerBox& patch) {
            const Render::GL::CHyprOpenGLImpl::SBorderRenderData perCorner{
                .round         = patch.round,
                .roundingPower = roundingPower,
                .borderSize    = data.borderSize,
                .a             = data.a,
                .outerRound    = -1, // use the same effective (round + borderSize) radius for the outer edge
            };
            if (data.hasGrad2)
                Render::GL::g_pHyprOpenGL->renderBorder(data.box, data.grad1, data.grad2, data.lerp, perCorner);
            else
                Render::GL::g_pHyprOpenGL->renderBorder(data.box, data.grad1, perCorner);
        });
    }
} // namespace

namespace {
    CFunctionHook* g_shadowHook = nullptr;

    using shadowDraw_t = void (*)(void* thisptr, WP<CShadowPassElement> element, const CRegion& damage);

    bool roundShadowsEnabled() {
        return vars.roundShadows && vars.roundShadows->value();
    }

    // Anchored on `fullBox`'s corner. fullBox is already the outer extent: the window box plus
    // the shadow range on every side. The box extends inward by the scaled shadow range, so it
    // covers the corner's rounded falloff and the blur margin out to fullBox's edge.
    SCornerBox makeShadowCornerBox(const CBox& fullBox, bool left, bool top, double liveRadiusPx, double scale, int scaledRange) {
        const double maxR  = std::floor(std::min(fullBox.width, fullBox.height) / 2.0);
        const double r     = std::clamp(liveRadiusPx * scale, 0.0, maxR);
        const int    round = static_cast<int>(std::lround(r));
        if (round <= 0)
            return SCornerBox{};

        const double size = std::ceil(r) + scaledRange;
        const double x    = left ? fullBox.x : fullBox.x + fullBox.width - size;
        const double y    = top ? fullBox.y : fullBox.y + fullBox.height - size;
        return SCornerBox{CBox{x, y, size, size}, round};
    }

    SCornerBoxes computeShadowCornerBoxes(const CBox& fullBox, const SeamState::SLiveCorners& radii, double scale, int scaledRange) {
        return SCornerBoxes{
            makeShadowCornerBox(fullBox, true, true, radii.topLeft, scale, scaledRange),
            makeShadowCornerBox(fullBox, false, true, radii.topRight, scale, scaledRange),
            makeShadowCornerBox(fullBox, true, false, radii.bottomLeft, scale, scaledRange),
            makeShadowCornerBox(fullBox, false, false, radii.bottomRight, scale, scaledRange),
        };
    }

    void hkShadowDraw(void* thisptr, WP<CShadowPassElement> element, const CRegion& damage) {
        const auto callOriginal = [&]() { (*reinterpret_cast<shadowDraw_t>(g_shadowHook->m_original))(thisptr, element, damage); };

        if (!roundShadowsEnabled() || element.expired()) {
            callOriginal();
            return;
        }

        auto& data = element.get()->m_data;
        if (!data.deco) {
            callOriginal();
            return;
        }

        const PHLMONITOR mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!mon) {
            callOriginal();
            return;
        }

        // getRenderData() sets g_pHyprRenderer->m_renderData.currentWindow to the window that
        // owns this shadow, so no separate window matching is needed.
        const SShadowRenderData rd = data.deco->getRenderData(mon, data.a);
        if (!rd.valid) {
            callOriginal();
            return;
        }

        const PHLWINDOW window = g_pHyprRenderer->m_renderData.currentWindow.lock();
        if (!window) {
            callOriginal();
            return;
        }

        const auto corners = SeamState::liveCornersFor(window);
        if (!corners) {
            callOriginal();
            return;
        }

        // decoration:shadow:sharp draws a flat rect without the rounded falloff this hook
        // expects, so leave it to the native draw.
        static auto PSHADOWSHARP = CConfigValue<Config::INTEGER>("decoration:shadow:sharp");
        if (*PSHADOWSHARP) {
            callOriginal();
            return;
        }

        // Use the window's live shadow color and fade state instead of the global config
        // value. That covers color_inactive, per-window overrides and the focus-change fade.
        const bool  animated = window->m_shadowFadeAnimationProgress->isBeingAnimated();
        const auto& grad1    = animated ? window->m_realShadowColorPrevious : window->m_realShadowColor;
        const auto& grad2    = window->m_realShadowColor;
        const float lerp     = animated ? window->m_shadowFadeAnimationProgress->value() : 1.0F;

        const int scaledRange = static_cast<int>(std::round(rd.size * mon->m_scale));

        static auto PPOWER       = CConfigValue<Config::FLOAT>("plugin:seam:rounding_power");
        const float roundingPower = *PPOWER;

        const SCornerBoxes boxes = computeShadowCornerBoxes(rd.fullBox, *corners, mon->m_scale, scaledRange);

        auto&         renderData  = g_pHyprRenderer->m_renderData;
        const CRegion savedDamage = renderData.damage;

        renderData.damage = pruneDamageForBoxes(savedDamage, boxes);
        callOriginal();
        renderData.damage = savedDamage;

        // currentWindow is left unset for the corner redraws. The straight pass gets it from
        // its own getRenderData() call inside callOriginal(). The shadow shader's window cutout
        // (renderRoundedShadow in OpenGL.cpp, ~2412-2438, `cutoutRadius` from
        // `PWINDOW->rounding() * scale`) is sized from the window's native rounding, which the
        // plugin requires to be 0, so the cutout is always a hard square whatever `round` this
        // call passes. On a rounded corner, that square cutout would leave a small triangular
        // notch of background at the corner tip. Without the cutout, a translucent window's
        // corner shadow shows through a small rounded sliver of its content as a soft tint,
        // which is much less visible than the notch.
        forEachCorner(boxes, savedDamage, [&](const SCornerBox& patch) {
            if (animated)
                Render::GL::g_pHyprOpenGL->renderRoundedShadow(rd.fullBox, patch.round, roundingPower, scaledRange, grad1, grad2, lerp, data.a);
            else
                Render::GL::g_pHyprOpenGL->renderRoundedShadow(rd.fullBox, patch.round, roundingPower, scaledRange, grad2, data.a);
        });
    }
} // namespace

namespace {
    enum class eLookup {
        Found,
        NotFound,
        Ambiguous,
    };

    const CHyprColor kErrorColor{1.0, 0.2, 0.2, 1.0};
    constexpr int     kNotifyTimeoutMs = 8000;

    struct SLookupResult {
        eLookup status;
        void*   address = nullptr;
    };

    // Finds the one function whose demangled name equals `demangled`. Returns Ambiguous if
    // there is more than one exact match. Same as SeamHook.cpp's findExact.
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
            std::format("[hypr-seam] Found more than one exact match for {} — refusing to guess which to use.", what) :
            std::format("[hypr-seam] Could not locate {} to hook.", what);
        Log::logger->log(Log::ERR, "{}", msg);
        HyprlandAPI::addNotification(PHANDLE, msg, kErrorColor, kNotifyTimeoutMs);
        return true;
    }

    constexpr const char* SHADOW_DEMANGLED =
        "Render::GL::CGLElementRenderer::draw(Hyprutils::Memory::CWeakPointer<CShadowPassElement>, Hyprutils::Math::CRegion const&)";
} // namespace

bool SeamDecorHook::install() {
    const auto borderLookup = findExact("draw", BORDER_DEMANGLED);
    if (reportLookupFailure(borderLookup.status, "CGLElementRenderer::draw(CBorderPassElement)"))
        return false;

    g_borderHook = HyprlandAPI::createFunctionHook(PHANDLE, borderLookup.address, reinterpret_cast<void*>(&hkBorderDraw));
    if (!g_borderHook || !g_borderHook->hook()) {
        Log::logger->log(Log::ERR, "[hypr-seam] failed to install border hook");
        if (g_borderHook)
            HyprlandAPI::removeFunctionHook(PHANDLE, g_borderHook);
        g_borderHook = nullptr;
        return false;
    }

    const auto shadowLookup = findExact("draw", SHADOW_DEMANGLED);
    if (reportLookupFailure(shadowLookup.status, "CGLElementRenderer::draw(CShadowPassElement)"))
        return true; // border still installed; shadow rounding just won't work

    g_shadowHook = HyprlandAPI::createFunctionHook(PHANDLE, shadowLookup.address, reinterpret_cast<void*>(&hkShadowDraw));
    if (!g_shadowHook || !g_shadowHook->hook()) {
        Log::logger->log(Log::ERR, "[hypr-seam] failed to install shadow hook");
        if (g_shadowHook)
            HyprlandAPI::removeFunctionHook(PHANDLE, g_shadowHook);
        g_shadowHook = nullptr;
    }
    return true;
}

void SeamDecorHook::remove() {
    if (g_shadowHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_shadowHook);
    g_shadowHook = nullptr;
    if (g_borderHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_borderHook);
    g_borderHook = nullptr;
}
