// Per-corner border and shadow rounding via render-pass elements.

#include "SeamDecorHook.hpp"
#include "SeamState.hpp"
#include "globals.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/render/WindowRenderPresentation.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/ShadowPassElement.hpp>
#include <hyprland/src/render/decorations/CHyprDropShadowDecoration.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <concepts>
#include <format>
#include <string>

namespace {
    struct CRoundingOverride {
        PHLWINDOW window;
        bool had = false;
        std::optional<Config::INTEGER> saved;
        bool valid = false;
        CRoundingOverride(PHLWINDOW w, Config::INTEGER v) {
            if (!w || !w->m_ruleApplicator) return;
            window = w;
            auto& r = window->m_ruleApplicator->rounding();
            had = r.hasValue() && r.getPriority() == Desktop::Types::PRIORITY_SET_PROP;
            saved = had ? std::optional<Config::INTEGER>(r.value()) : std::nullopt;
            r.set(v, Desktop::Types::PRIORITY_SET_PROP);
            valid = true;
        }
        ~CRoundingOverride() {
            if (!valid || !window || !window->m_ruleApplicator) return;
            auto& r = window->m_ruleApplicator->rounding();
            if (had) r.set(*saved, Desktop::Types::PRIORITY_SET_PROP);
            else r.unset(Desktop::Types::PRIORITY_SET_PROP);
        }
        CRoundingOverride& operator=(const CRoundingOverride&) = delete;
    };

    struct SCornerBox {
        CBox box;
        int  round = 0; // 0 = no rounding
    };

    struct SCornerBoxes {
        SCornerBox topLeft, topRight, bottomLeft, bottomRight;
    };

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

    template <std::invocable<const SCornerBox&> F>
    void forEachCorner(Render::CRenderContext& ctx, const SCornerBoxes& boxes, const CRegion& savedDamage, F&& draw) {
        auto& renderData = ctx.m_data;
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

    using borderDraw_t = void (*)(void* thisptr, Render::CRenderContext& ctx, WP<CBorderPassElement> element, const CRegion& damage);

    constexpr const char* BORDER_DEMANGLED =
        "Render::GL::CGLElementRenderer::draw(Render::CRenderContext&, Hyprutils::Memory::CWeakPointer<CBorderPassElement>, Hyprutils::Math::CRegion const&)";

    bool roundBordersEnabled() {
        return vars.roundBorders && vars.roundBorders->value();
    }

    SCornerBox makeBorderCornerBox(const CBox& box, bool left, bool top, double liveRadiusPx, double scale, int scaledBorderSize) {
        const double maxR  = std::floor(std::min(box.width, box.height) / 2.0);
        const double r     = std::clamp(liveRadiusPx * scale, 0.0, maxR);
        const int    round = static_cast<int>(std::lround(r));
        if (round <= 0)
            return SCornerBox{};

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

    void hkBorderDraw(void* thisptr, Render::CRenderContext& ctx, WP<CBorderPassElement> element, const CRegion& damage) {
        const auto callOriginal = [&]() { (*reinterpret_cast<borderDraw_t>(g_borderHook->m_original))(thisptr, ctx, element, damage); };

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

        const PHLMONITOR mon = ctx.m_data.pMonitor.lock();
        if (!mon) {
            callOriginal();
            return;
        }

        const double scale            = mon->m_scale;
        const int    scaledBorderSize = static_cast<int>(std::round(data.borderSize * scale));

        static auto PPOWER       = CConfigValue<Config::FLOAT>("plugin:seam:rounding_power");
        const float roundingPower = *PPOWER;

        const SCornerBoxes boxes = computeBorderCornerBoxes(data.box, *corners, scale, scaledBorderSize);

        {
            const int r0 = boxes.topLeft.round;
            if (r0 > 0 && boxes.topRight.round == r0 && boxes.bottomLeft.round == r0 && boxes.bottomRight.round == r0) {
                const int savedRound = data.round;
                const int savedOuterRound = data.outerRound;
                const auto savedPower = data.roundingPower;
                data.round = r0;
                data.outerRound = -1;
                data.roundingPower = roundingPower;
                callOriginal();
                data.round = savedRound;
                data.outerRound = savedOuterRound;
                data.roundingPower = savedPower;
                return;
            }
        }

        {
            if (!ctx.m_data.clipBox.empty()) {
                callOriginal();
                return;
            }
        }

        auto&         renderData  = ctx.m_data;
        const CRegion savedDamage = renderData.damage;
        const int     savedRound  = data.round;

        {
            const int rounds[4] = {boxes.topLeft.round, boxes.topRight.round, boxes.bottomLeft.round, boxes.bottomRight.round};
            int best = rounds[0];
            for (int i = 1; i < 4; ++i)
                best = std::max(best, rounds[i]);
            bool allPositive = boxes.topLeft.round > 0 && boxes.topRight.round > 0 && boxes.bottomLeft.round > 0 && boxes.bottomRight.round > 0;
            if (best > 0 && allPositive) {
                SCornerBoxes diff = boxes;
                auto rescaleB = [&](SCornerBox& pc, bool left, bool top) {
                    if (pc.round == best) { pc.round = 0; return; }
                    SCornerBox nb = makeBorderCornerBox(data.box, left, top, best / scale, scale, scaledBorderSize);
                    nb.round = pc.round;
                    pc = nb;
                };
                rescaleB(diff.topLeft, true, true);
                rescaleB(diff.topRight, false, true);
                rescaleB(diff.bottomLeft, true, false);
                rescaleB(diff.bottomRight, false, false);

                const int   savedOuterRound = data.outerRound;
                const float savedPower      = data.roundingPower;

                renderData.damage = pruneDamageForBoxes(savedDamage, diff);
                data.round         = best;
                data.outerRound    = -1;
                data.roundingPower = roundingPower;

                callOriginal();

                renderData.damage = savedDamage;
                data.round         = savedRound;
                data.outerRound    = savedOuterRound;
                data.roundingPower = savedPower;

                forEachCorner(ctx, diff, savedDamage, [&](const SCornerBox& patch) {
                    const Render::GL::CHyprOpenGLImpl::SBorderRenderData perCorner{
                        .round         = patch.round,
                        .roundingPower = roundingPower,
                        .borderSize    = data.borderSize,
                        .a             = data.a,
                        .outerRound    = -1,
                    };
                    if (data.hasGrad2)
                        Render::GL::g_pHyprOpenGL->renderBorder(ctx, data.box, data.grad1, data.grad2, data.lerp, perCorner);
                    else
                        Render::GL::g_pHyprOpenGL->renderBorder(ctx, data.box, data.grad1, perCorner);
                });
                return;
            }
        }

        const int   savedOuterRound = data.outerRound;
        const float savedPower      = data.roundingPower;

        renderData.damage = pruneDamageForBoxes(savedDamage, boxes);
        data.round         = 0;
        data.outerRound    = -1;
        data.roundingPower = roundingPower;

        callOriginal();

        renderData.damage = savedDamage;
        data.round         = savedRound;
        data.outerRound    = savedOuterRound;
        data.roundingPower = savedPower;

        forEachCorner(ctx, boxes, savedDamage, [&](const SCornerBox& patch) {
            const Render::GL::CHyprOpenGLImpl::SBorderRenderData perCorner{
                .round         = patch.round,
                .roundingPower = roundingPower,
                .borderSize    = data.borderSize,
                .a             = data.a,
                .outerRound    = -1, // use the same effective (round + borderSize) radius for the outer edge
            };
            if (data.hasGrad2)
                Render::GL::g_pHyprOpenGL->renderBorder(ctx, data.box, data.grad1, data.grad2, data.lerp, perCorner);
            else
                Render::GL::g_pHyprOpenGL->renderBorder(ctx, data.box, data.grad1, perCorner);
        });
    }
} // namespace

namespace {
    CFunctionHook* g_shadowHook           = nullptr;
    CFunctionHook* g_shadowTransitionHook = nullptr;

    using shadowDraw_t = void (*)(void* thisptr, Render::CRenderContext& ctx, const CBox& box, int round, float roundingPower, int range,
                                  const Config::CGradientValueData& grad, float a, const Render::SWindowRenderPresentation& presentation);

    using shadowDrawTransition_t = void (*)(void* thisptr, Render::CRenderContext& ctx, const CBox& box, int round, float roundingPower, int range,
                                            const Config::CGradientValueData& grad1, const Config::CGradientValueData& grad2, float lerp, float a,
                                            const Render::SWindowRenderPresentation& presentation);

    constexpr const char* SHADOW_DEMANGLED =
        "CHyprDropShadowDecoration::drawShadowInternal(Render::CRenderContext&, Hyprutils::Math::CBox const&, int, float, int, "
        "Config::CGradientValueData const&, float, Render::SWindowRenderPresentation const&)";

    constexpr const char* SHADOW_TRANSITION_DEMANGLED =
        "CHyprDropShadowDecoration::drawShadowInternal(Render::CRenderContext&, Hyprutils::Math::CBox const&, int, float, int, "
        "Config::CGradientValueData const&, Config::CGradientValueData const&, float, float, Render::SWindowRenderPresentation const&)";

    bool roundShadowsEnabled() {
        return vars.roundShadows && vars.roundShadows->value();
    }

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

    template <typename CallOriginal, typename CallOriginalWithRound, typename DrawCorner>
    void drawShadowCorners(Render::CRenderContext& ctx, const CBox& fullBox, int range, CallOriginal&& callOriginal,
                           CallOriginalWithRound&& callOriginalWithRound, DrawCorner&& drawCorner) {
        if (!roundShadowsEnabled()) {
            callOriginal();
            return;
        }

        const PHLMONITOR mon = ctx.m_data.pMonitor.lock();
        if (!mon) {
            callOriginal();
            return;
        }

        const PHLWINDOW window = ctx.m_data.currentWindow.lock();
        if (!window) {
            callOriginal();
            return;
        }

        const auto corners = SeamState::liveCornersFor(window);
        if (!corners) {
            callOriginal();
            return;
        }

        static auto PSHADOWSHARP = CConfigValue<Config::INTEGER>("decoration:shadow:sharp");
        if (*PSHADOWSHARP) {
            callOriginal();
            return;
        }

        static auto PPOWER       = CConfigValue<Config::FLOAT>("plugin:seam:rounding_power");
        const float roundingPower = *PPOWER;

        const SCornerBoxes boxes = computeShadowCornerBoxes(fullBox, *corners, mon->m_scale, range);

        {
            const int r0 = boxes.topLeft.round;
            if (r0 > 0 && boxes.topRight.round == r0 && boxes.bottomLeft.round == r0 && boxes.bottomRight.round == r0) {
                const double combinedScale = std::max(0.0001, sc<double>(ctx.m_data.renderModif.combinedScale()));
                const double nativePower   = std::max(0.01, sc<double>(window->m_ruleApplicator->roundingPower().valueOrDefault()));
                const auto   nativeRound   = static_cast<Config::INTEGER>(std::floor(r0 * 2.0 / (nativePower * mon->m_scale * combinedScale)));
                CRoundingOverride override{window, nativeRound};

                callOriginalWithRound(r0, roundingPower);
                return;
            }
        }

        {
            if (!ctx.m_data.clipBox.empty()) {
                callOriginal();
                return;
            }
        }

        auto&         renderData  = ctx.m_data;
        const CRegion savedDamage = renderData.damage;

        {
            const int rounds[4] = {boxes.topLeft.round, boxes.topRight.round, boxes.bottomLeft.round, boxes.bottomRight.round};
            int best = rounds[0];
            for (int i = 1; i < 4; ++i)
                best = std::max(best, rounds[i]);
            bool allPositive = boxes.topLeft.round > 0 && boxes.topRight.round > 0 && boxes.bottomLeft.round > 0 && boxes.bottomRight.round > 0;
            if (best > 0 && allPositive) {
                SCornerBoxes diff = boxes;
                auto rescaleS = [&](SCornerBox& pc, bool left, bool top) {
                    if (pc.round == best) { pc.round = 0; return; }
                    SCornerBox nb = makeShadowCornerBox(fullBox, left, top, best / mon->m_scale, mon->m_scale, range);
                    nb.round = pc.round;
                    pc = nb;
                };
                rescaleS(diff.topLeft, true, true);
                rescaleS(diff.topRight, false, true);
                rescaleS(diff.bottomLeft, true, false);
                rescaleS(diff.bottomRight, false, false);

                {
                    const double combinedScale = std::max(0.0001, sc<double>(ctx.m_data.renderModif.combinedScale()));
                    const double nativePower   = std::max(0.01, sc<double>(window->m_ruleApplicator->roundingPower().valueOrDefault()));
                    const auto   nativeRound   = static_cast<Config::INTEGER>(std::floor(best * 2.0 / (nativePower * mon->m_scale * combinedScale)));
                    CRoundingOverride override{window, nativeRound};

                    renderData.damage = pruneDamageForBoxes(savedDamage, diff);
                    callOriginalWithRound(best, roundingPower);
                    renderData.damage = savedDamage;
                }

                forEachCorner(ctx, diff, savedDamage, [&](const SCornerBox& patch) {
                    const double combinedScale = std::max(0.0001, sc<double>(ctx.m_data.renderModif.combinedScale()));
                    const double nativePower   = std::max(0.01, sc<double>(window->m_ruleApplicator->roundingPower().valueOrDefault()));
                    const int    cutoutRound   = std::max(0, patch.round - 2);
                    const auto   nativeRound   = static_cast<Config::INTEGER>(std::floor(cutoutRound * 2.0 / (nativePower * mon->m_scale * combinedScale)));
                    CRoundingOverride override{window, nativeRound};

                    drawCorner(patch, roundingPower);
                });
                return;
            }
        }

        renderData.damage = pruneDamageForBoxes(savedDamage, boxes);
        callOriginal();
        renderData.damage = savedDamage;

        forEachCorner(ctx, boxes, savedDamage, [&](const SCornerBox& patch) {
            const double combinedScale = std::max(0.0001, sc<double>(ctx.m_data.renderModif.combinedScale()));
            const double nativePower   = std::max(0.01, sc<double>(window->m_ruleApplicator->roundingPower().valueOrDefault()));
            const int    cutoutRound   = std::max(0, patch.round - 2);
            const auto   nativeRound   = static_cast<Config::INTEGER>(std::floor(cutoutRound * 2.0 / (nativePower * mon->m_scale * combinedScale)));
            CRoundingOverride override{window, nativeRound};

            drawCorner(patch, roundingPower);
        });
    }

    void hkShadowDraw(void* thisptr, Render::CRenderContext& ctx, const CBox& box, int round, float roundingPower, int range, const Config::CGradientValueData& grad,
                      float a, const Render::SWindowRenderPresentation& presentation) {
        drawShadowCorners(
            ctx, box, range,
            [&] { (*reinterpret_cast<shadowDraw_t>(g_shadowHook->m_original))(thisptr, ctx, box, round, roundingPower, range, grad, a, presentation); },
            [&](int ro, float power) {
                (*reinterpret_cast<shadowDraw_t>(g_shadowHook->m_original))(thisptr, ctx, box, ro, power, range, grad, a, presentation);
            },
            [&](const SCornerBox& patch, float power) {
                Render::GL::g_pHyprOpenGL->renderRoundedShadow(ctx, box, patch.round, power, range, grad, a, presentation);
            });
    }

    void hkShadowDrawTransition(void* thisptr, Render::CRenderContext& ctx, const CBox& box, int round, float roundingPower, int range, const Config::CGradientValueData& grad1,
                                const Config::CGradientValueData& grad2, float lerp, float a, const Render::SWindowRenderPresentation& presentation) {
        drawShadowCorners(
            ctx, box, range,
            [&] {
                (*reinterpret_cast<shadowDrawTransition_t>(g_shadowTransitionHook->m_original))(thisptr, ctx, box, round, roundingPower, range, grad1, grad2, lerp, a,
                                                                                               presentation);
            },
            [&](int ro, float power) {
                (*reinterpret_cast<shadowDrawTransition_t>(g_shadowTransitionHook->m_original))(thisptr, ctx, box, ro, power, range, grad1, grad2, lerp, a,
                                                                                               presentation);
            },
            [&](const SCornerBox& patch, float power) {
                Render::GL::g_pHyprOpenGL->renderRoundedShadow(ctx, box, patch.round, power, range, grad1, grad2, lerp, a, presentation);
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
            std::format("[hypr-seam] Found more than one exact match for {} — refusing to guess which to use.", what) :
            std::format("[hypr-seam] Could not locate {} to hook.", what);
        Log::logger->log(Log::ERR, msg);
        HyprlandAPI::addNotification(PHANDLE, msg, kErrorColor, kNotifyTimeoutMs);
        return true;
    }

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

    const auto installShadow = [&](const char* demangled, const char* what, void* replacement, CFunctionHook*& hook) {
        const auto lookup = findExact("drawShadowInternal", demangled);
        if (reportLookupFailure(lookup.status, what))
            return;
        hook = HyprlandAPI::createFunctionHook(PHANDLE, lookup.address, replacement);
        if (!hook || !hook->hook()) {
            Log::logger->log(Log::ERR, "[hypr-seam] failed to install shadow hook");
            if (hook)
                HyprlandAPI::removeFunctionHook(PHANDLE, hook);
            hook = nullptr;
        }
    };

    installShadow(SHADOW_DEMANGLED, "CHyprDropShadowDecoration::drawShadowInternal (solid)", reinterpret_cast<void*>(&hkShadowDraw), g_shadowHook);
    installShadow(SHADOW_TRANSITION_DEMANGLED, "CHyprDropShadowDecoration::drawShadowInternal (transition)", reinterpret_cast<void*>(&hkShadowDrawTransition),
                  g_shadowTransitionHook);
    return true;
}

void SeamDecorHook::remove() {
    if (g_shadowTransitionHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_shadowTransitionHook);
    g_shadowTransitionHook = nullptr;
    if (g_shadowHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_shadowHook);
    g_shadowHook = nullptr;
    if (g_borderHook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_borderHook);
    g_borderHook = nullptr;
}
