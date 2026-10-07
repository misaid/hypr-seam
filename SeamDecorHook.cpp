// Opt-in border/shadow corner-rounding hooks on the same render-pass-element pattern
// SeamHook.cpp already uses for window content (hkDrawSurface). Each hook reuses
// SeamState::liveCornersFor(window) -- already seam-aware, already animated, already
// per-app-resolved -- and redraws the border/shadow in up to 4 corner-quadrant passes
// through Hyprland's own g_pHyprOpenGL->renderBorder()/renderRoundedShadow(), mirroring
// how hkDrawSurface gets per-corner rounding out of renderTexture()'s single `round` field.
//
// Confirmed against the installed Hyprland 0.56.2 headers, the upstream 0.56.2 .cpp sources
// (CHyprBorderDecoration.cpp, CHyprDropShadowDecoration.cpp, GLElementRenderer.cpp, OpenGL.cpp
// -- not shipped with the dev headers, fetched separately to settle the final review's Critical
// findings), and the dynamic symbol table (plan
// docs/superpowers/plans/2026-10-06-border-shadow-rounding.md, Task 1):
//   - exact demangled draw() overloads for CBorderPassElement/CShadowPassElement below
//   - g_pHyprRenderer->m_renderData.damage restricts renderBorder()/renderRoundedShadow()
//     output the same way it restricts drawSurface (empirically confirmed via pixel diff)
//   - decoration:shadow:color resolves through CConfigValue<Config::IComplexConfigValue>
//     to a real Config::CGradientValueData
//   - CBorderPassElement::m_data.box and CHyprDropShadowDecoration::getRenderData()'s fullBox
//     are both monitor-scaled, rounded-to-int px (CHyprBorderDecoration::draw() and
//     getRenderData() both end with `.scale(pMonitor->m_scale).round()`) -- the SAME space
//     hkDrawSurface's windowBox uses. SeamState::liveCornersFor returns LOGICAL px, so both
//     hooks must multiply by the monitor's scale before comparing against these boxes.
//   - renderBorder()'s effective inner radius is `data.round + (data.round == 0 ? 0 :
//     scaledBorderSize)`, and `data.outerRound == -1` makes it reuse that same effective value
//     for the outer edge instead of native's separate (whole-window) outerRound -- the correct,
//     simplest way to get a single matching inner+outer curve for one corner's own radius.
//   - renderBorder() draws into box.expand(scaledBorderSize), i.e. a ring extending
//     scaledBorderSize beyond the content box on every edge, so a corner's redraw-restriction
//     box must extend that far outward too, or the ring outside the content box never gets
//     repainted and stays square from the straight-edge pass.
//   - getRenderData() sets g_pHyprRenderer->m_renderData.currentWindow as a side effect (used
//     by the shadow shader to cut the window's own box out of the fill) and reposition()
//     clears it at the end of the native render() call -- so it must be reset around the corner
//     redraws too, and it is also the exact, unambiguous shadow owner: calling getRenderData()
//     once and reading currentWindow afterwards needs no separate window-matching logic at all.

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
#include <format>
#include <string>

namespace {
    // One corner's redraw box, already in the same monitor-scaled px space as the box it was
    // computed against, plus the radius to feed Hyprland's own rounded-rect shader for just
    // that corner.
    struct SCornerBox {
        CBox box;
        int  round = 0; // 0 means "no rounding needed here, leave it to the native straight-edge paint"
    };

    struct SCornerBoxes {
        SCornerBox topLeft, topRight, bottomLeft, bottomRight;
    };

    // Subtracts each non-empty corner box from `savedDamage`, producing the region the
    // straight-edge pass should actually paint into.
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

    // Redraws each non-empty corner box, restricted to just that box via a damage region
    // intersected with what was actually damaged this frame. `draw` performs one actual
    // render call for one corner's box/round. Reaches for g_pHyprRenderer->m_renderData
    // directly (same as hkDrawSurface does) rather than naming its type as a parameter --
    // SeamHook.cpp never names that type either, it only ever accesses it through `auto&`.
    template <typename F>
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

    // Confirmed in Task 1, Step 2 against the installed 0.56.2 headers + dynamic symbol table.
    constexpr const char* BORDER_DEMANGLED =
        "Render::GL::CGLElementRenderer::draw(Hyprutils::Memory::CWeakPointer<CBorderPassElement>, Hyprutils::Math::CRegion const&)";

    bool roundBordersEnabled() {
        return vars.roundBorders && vars.roundBorders->value();
    }

    // Anchored on `box` (the border's content box, pre-expansion), extended outward by
    // `scaledBorderSize` on the corner's own two outward edges so the redraw box covers the
    // whole ring renderBorder() paints there (box.expand(scaledBorderSize)), not just the part
    // inside the content box.
    SCornerBox makeBorderCornerBox(const CBox& box, bool left, bool top, double liveRadiusPx, double scale, int scaledBorderSize) {
        const double maxR  = std::floor(std::min(box.width, box.height) / 2.0);
        const double r     = std::clamp(liveRadiusPx * scale, 0.0, maxR);
        const int    round = static_cast<int>(std::lround(r));
        if (round <= 0)
            return SCornerBox{}; // renderBorder's own effective round is 0 too in this case -- native paint is already correct here.

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

        // Same source hkDrawSurface uses for content corners (SeamHook.cpp), not the native
        // per-window roundingPower, so border/shadow curve exactly like content at each corner.
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
                .outerRound    = -1, // reuse the same effective (round + borderSize) radius for the outer edge too
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

    // Anchored exactly on `fullBox`'s own corner (fullBox already IS the outermost extent --
    // window box plus the shadow's range margin on every side), extended inward by the scaled
    // shadow range so the redraw box covers both the corner's rounded falloff and the blur
    // margin out to fullBox's edge.
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

        // getRenderData() sets g_pHyprRenderer->m_renderData.currentWindow as a side effect --
        // the exact, unambiguous owner of this shadow decoration. No separate window-matching
        // needed (the plan's Task 1 constructor-hook design was unusable -- ambiguous symbols
        // -- and its box-matching fallback compared mismatched coordinate spaces; this reads
        // Hyprland's own answer directly instead).
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

        // decoration:shadow:sharp draws a flat rect, not the rounded-shadow falloff this hook
        // assumes -- leave it to native rather than drawing mismatched corners.
        static auto PSHADOWSHARP = CConfigValue<Config::INTEGER>("decoration:shadow:sharp");
        if (*PSHADOWSHARP) {
            callOriginal();
            return;
        }

        // The window's own live shadow color/animation state, not the raw global config value
        // -- covers color_inactive, per-window overrides, and the focus-change fade.
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

        // getRenderData() set currentWindow for native's own draw; callOriginal()'s reposition()
        // clears it afterwards (CHyprDropShadowDecoration.cpp), so it must be set again for the
        // corner redraws to get the same window-cutout the native fill gets.
        g_pHyprRenderer->m_renderData.currentWindow = window;

        forEachCorner(boxes, savedDamage, [&](const SCornerBox& patch) {
            if (animated)
                Render::GL::g_pHyprOpenGL->renderRoundedShadow(rd.fullBox, patch.round, roundingPower, scaledRange, grad1, grad2, lerp, data.a);
            else
                Render::GL::g_pHyprOpenGL->renderRoundedShadow(rd.fullBox, patch.round, roundingPower, scaledRange, grad2, data.a);
        });

        g_pHyprRenderer->m_renderData.currentWindow.reset();
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

    // Finds exactly one function whose demangled name equals `demangled`. Refuses (Ambiguous)
    // rather than guessing if more than one exact match exists. Mirrors SeamHook.cpp's
    // findExact (deliberately not shared across files -- see File Structure in the plan).
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
