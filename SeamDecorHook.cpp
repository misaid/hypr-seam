// Opt-in border/shadow corner-rounding hooks on the same render-pass-element pattern
// SeamHook.cpp already uses for window content (hkDrawSurface). Each hook reuses
// SeamState::liveCornersFor(window) -- already seam-aware, already animated, already
// per-app-resolved -- and redraws the border/shadow in up to 4 corner-quadrant passes
// through Hyprland's own g_pHyprOpenGL->renderBorder()/renderRoundedShadow(), mirroring
// how hkDrawSurface gets per-corner rounding out of renderTexture()'s single `round` field.
//
// Confirmed against the installed Hyprland 0.56.2 headers and dynamic symbol table
// (plan docs/superpowers/plans/2026-10-06-border-shadow-rounding.md, Task 1):
//   - exact demangled draw() overloads for CBorderPassElement/CShadowPassElement below
//   - g_pHyprRenderer->m_renderData.damage restricts renderBorder()/renderRoundedShadow()
//     output the same way it restricts drawSurface (empirically confirmed via pixel diff)
//   - decoration:shadow:color resolves through CConfigValue<Config::IComplexConfigValue>
//     to a real Config::CGradientValueData

#include "SeamDecorHook.hpp"
#include "SeamState.hpp"
#include "globals.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/ShadowPassElement.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace {
    CFunctionHook* g_borderHook = nullptr;

    using borderDraw_t = void (*)(void* thisptr, WP<CBorderPassElement> element, const CRegion& damage);

    // Confirmed in Task 1, Step 2 against the installed 0.56.2 headers + dynamic symbol table.
    constexpr const char* BORDER_DEMANGLED =
        "Render::GL::CGLElementRenderer::draw(Hyprutils::Memory::CWeakPointer<CBorderPassElement>, Hyprutils::Math::CRegion const&)";

    bool roundBordersEnabled() {
        return vars.roundBorders && vars.roundBorders->value();
    }

    // One corner's redraw box: a size x size square anchored on the given corner of `box`.
    // Deliberately separate from SeamHook.cpp's SCornerPatch/computeCornerPatches (those
    // additionally scale for texture sampling, which border/shadow don't need) -- see File
    // Structure in the plan.
    struct SCornerBox {
        CBox box;
        int  round = 0; // 0 means "no rounding needed here, leave it to the native straight-edge paint"
    };

    SCornerBox makeCornerBox(const CBox& box, bool left, bool top, double liveRadiusPx) {
        const double maxR  = std::floor(std::min(box.width, box.height) / 2.0);
        const double r     = std::clamp(liveRadiusPx, 0.0, maxR);
        const int    round = static_cast<int>(std::lround(r));
        if (round <= 0)
            return SCornerBox{};

        const double size = std::ceil(r);
        const double x    = left ? box.x : box.x + box.width - size;
        const double y    = top ? box.y : box.y + box.height - size;
        return SCornerBox{CBox{x, y, size, size}, round};
    }

    struct SCornerBoxes {
        SCornerBox topLeft, topRight, bottomLeft, bottomRight;
    };

    SCornerBoxes computeCornerBoxes(const CBox& box, const SeamState::SLiveCorners& radii) {
        return SCornerBoxes{
            makeCornerBox(box, true, true, radii.topLeft),
            makeCornerBox(box, false, true, radii.topRight),
            makeCornerBox(box, true, false, radii.bottomLeft),
            makeCornerBox(box, false, false, radii.bottomRight),
        };
    }

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

        const SCornerBoxes boxes = computeCornerBoxes(data.box, *corners);

        auto&         renderData  = g_pHyprRenderer->m_renderData;
        const CRegion savedDamage = renderData.damage;
        const int     savedRound  = data.round;

        renderData.damage = pruneDamageForBoxes(savedDamage, boxes);
        data.round         = 0;

        callOriginal();

        renderData.damage = savedDamage;
        data.round         = savedRound;

        const Render::GL::CHyprOpenGLImpl::SBorderRenderData borderData{
            .round         = 0, // overwritten per corner below
            .roundingPower = data.roundingPower,
            .borderSize    = data.borderSize,
            .a             = data.a,
            .outerRound    = data.outerRound,
        };

        forEachCorner(boxes, savedDamage, [&](const SCornerBox& patch) {
            auto perCorner  = borderData;
            perCorner.round = patch.round;
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

    void hkShadowDraw(void* thisptr, WP<CShadowPassElement> element, const CRegion& damage) {
        // Filled in by Task 4.
        (*reinterpret_cast<shadowDraw_t>(g_shadowHook->m_original))(thisptr, element, damage);
    }
}

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
}

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
