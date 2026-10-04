#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>

#include "globals.hpp"
#include "SeamConfig.hpp"
#include "SeamRuleStore.hpp"
#include "SeamState.hpp"
#include "SeamHook.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// Hyprlang keyword handler for `seamrule = ...` lines. Handlers are plain function
// pointers with no captured state, so parsed rules are stashed in SeamRuleStore.
static Hyprlang::CParseResult onSeamRule(const char* /*COMMAND*/, const char* VALUE) {
    Hyprlang::CParseResult result;
    SSeamRule              rule;
    if (!parseSeamRuleLine(VALUE, rule))
        result.setError("hypr-seam: malformed seamrule line");
    else
        SeamRuleStore::add(rule);
    return result;
}

static void registerSeamConfig() {
    vars.rounding            = makeShared<Config::Values::CIntValue>("plugin:seam:rounding", "Base corner radius", 22);
    vars.roundingTopLeft     = makeShared<Config::Values::CIntValue>("plugin:seam:rounding_topleft", "Base top-left radius", -1);
    vars.roundingTopRight    = makeShared<Config::Values::CIntValue>("plugin:seam:rounding_topright", "Base top-right radius", -1);
    vars.roundingBottomLeft  = makeShared<Config::Values::CIntValue>("plugin:seam:rounding_bottomleft", "Base bottom-left radius", -1);
    vars.roundingBottomRight = makeShared<Config::Values::CIntValue>("plugin:seam:rounding_bottomright", "Base bottom-right radius", -1);
    vars.roundingPower       = makeShared<Config::Values::CFloatValue>("plugin:seam:rounding_power", "Squircle exponent", 2.0F);
    vars.enabled             = makeShared<Config::Values::CBoolValue>("plugin:seam:enabled", "Global seam master switch", false);
    vars.seamRadius          = makeShared<Config::Values::CIntValue>("plugin:seam:seam_radius", "Radius a flattened corner collapses to", 2);
    vars.tolerance           = makeShared<Config::Values::CIntValue>("plugin:seam:tolerance", "Max px gap still considered touching", 6);
    vars.animate             = makeShared<Config::Values::CBoolValue>("plugin:seam:animate", "Ease corner radius changes", true);
    vars.animationSpeed      = makeShared<Config::Values::CFloatValue>("plugin:seam:animation_speed", "Transition duration in ms", 300.0F);
    vars.animationCurve      = makeShared<Config::Values::CStringValue>("plugin:seam:animation_curve", "Bezier curve name", "default");

    HyprlandAPI::addConfigValueV2(PHANDLE, vars.rounding);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.roundingTopLeft);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.roundingTopRight);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.roundingBottomLeft);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.roundingBottomRight);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.roundingPower);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.enabled);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.seamRadius);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.tolerance);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.animate);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.animationSpeed);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.animationCurve);

    HyprlandAPI::addConfigKeyword(PHANDLE, "seamrule", onSeamRule, Hyprlang::SHandlerOptions{});

    HyprlandAPI::reloadConfig();
}

// Builds the resolved global seam defaults from the live plugin:seam:* config values.
// Non-static (declared in globals.hpp) so other translation units can call it directly.
SGlobalSeamDefaults currentGlobalDefaults() {
    const double base = static_cast<double>(vars.rounding->value());

    auto pick = [&](const SP<Config::Values::CIntValue>& v) -> double {
        const Config::INTEGER val = v->value();
        return val < 0 ? base : static_cast<double>(val);
    };

    return SGlobalSeamDefaults{
        .baseRadii =
            {
                .topLeft     = pick(vars.roundingTopLeft),
                .topRight    = pick(vars.roundingTopRight),
                .bottomLeft  = pick(vars.roundingBottomLeft),
                .bottomRight = pick(vars.roundingBottomRight),
            },
        .roundingPower = static_cast<double>(vars.roundingPower->value()),
        .seamEnabled   = vars.enabled->value(),
        .seamRadius    = static_cast<double>(vars.seamRadius->value()),
        .tolerance     = static_cast<double>(vars.tolerance->value()),
    };
}

// Warns if the user hasn't zeroed decoration:rounding, since this plugin fully
// replaces native corner rounding and will visually conflict with it otherwise.
static void checkNativeRoundingIsZero() {
    static auto PROUNDING = CConfigValue<Config::INTEGER>("decoration:rounding");
    if (*PROUNDING != 0) {
        HyprlandAPI::addNotification(
            PHANDLE, "[hypr-seam] decoration:rounding is not 0 — this plugin fully replaces native rounding and requires it. Set decoration:rounding = 0.",
            CHyprColor{1.0, 0.6, 0.0, 1.0}, 8000);
    }
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hypr-seam] Version mismatch");
    }

    registerSeamConfig();
    checkNativeRoundingIsZero();

    // Hyprlang re-invokes the `seamrule` keyword handler for every matching line on
    // each config reload, but never clears state our plugin owns (SeamRuleStore) —
    // so without this, rules re-parsed on reload would simply append onto the old
    // ones forever. Clear on preReload (before Hyprlang re-runs the keyword lines),
    // and re-check the rounding guard on reloaded (after the new values land).
    static auto PRERELOAD = Event::bus()->m_events.config.preReload.listen([]() { SeamRuleStore::clear(); });
    // Also recompute on full config reload (e.g. `hyprctl reload` after editing
    // seamrule lines, radii, or plugin:seam:enabled in the config file) — without
    // this, a reload leaves every tracked window's corners stale until some
    // unrelated geometry/workspace/monitor event happens to fire.
    static auto PRELOADED = Event::bus()->m_events.config.reloaded.listen([]() {
        checkNativeRoundingIsZero();
        SeamState::recomputeAll();
    });
    // config.props_refreshed's exact trigger conditions aren't documented in the
    // installed EventBus.hpp beyond its name and `Event<const bool>` signature.
    // Wired as a defensive extra alongside the guaranteed `reloaded` path above,
    // in case some config-value change path fires it without a full reload —
    // but verified empirically (fix-round-1 manual testing) that it does NOT
    // fire for a plain `hyprctl keyword plugin:seam:... <value>` set, so that
    // path still relies on an explicit window/workspace/monitor event or a full
    // `hyprctl reload` to pick up the new value; this listener is cheap and
    // harmless to keep regardless, in case other config-change paths do fire it.
    static auto PPROPSREFRESHED = Event::bus()->m_events.config.props_refreshed.listen([](const bool) { SeamState::recomputeAll(); });

    // Window tracking + adjacency recompute triggers (Task 5).
    //
    // Event names below were confirmed against the installed
    // <hyprland/src/event/EventBus.hpp> rather than trusted from the plan verbatim:
    //   - window.open / window.close: exist exactly as named.
    //   - window.move / window.changeFloatingMode / window.fullscreen /
    //     workspace.active: "window.move" does NOT exist (there is no standalone
    //     window-move/resize event at all); "changeFloatingMode" doesn't exist
    //     either — the real member is "window.floating". "window.fullscreen" and
    //     "workspace.active" exist exactly as named.
    //   - monitor add/remove/move: real members are monitor.added, monitor.removed,
    //     monitor.layoutChanged (used here for monitor rearrangement/"move").
    // For the missing window-move/resize event, we follow the plan's documented
    // fallback: recompute from a general per-tick callback, gated by a cheap dirty
    // check (SeamState::onTick()) so idle ticks stay nearly free.
    static auto PWINOPEN     = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { SeamState::onWindowOpened(w); });
    static auto PWINCLOSE    = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) { SeamState::onWindowClosed(w); });
    static auto PWINFLOAT    = Event::bus()->m_events.window.floating.listen([](PHLWINDOW) { SeamState::recomputeAll(); });
    static auto PWINFULL     = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW) { SeamState::recomputeAll(); });
    static auto PWINMOVEWS   = Event::bus()->m_events.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE) { SeamState::recomputeAll(); });
    static auto PWORKSPACE   = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE) { SeamState::recomputeAll(); });
    static auto PMONADDED    = Event::bus()->m_events.monitor.added.listen([](PHLMONITOR) { SeamState::recomputeAll(); });
    static auto PMONREMOVED  = Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR) { SeamState::recomputeAll(); });
    static auto PMONLAYOUT   = Event::bus()->m_events.monitor.layoutChanged.listen([]() { SeamState::recomputeAll(); });
    static auto PTICK        = Event::bus()->m_events.tick.listen([]() { SeamState::onTick(); });

    // Attach to every window already open at load time (the plugin can be
    // hot-loaded into a running session with windows already present).
    for (auto& w : Desktop::windowState()->windows()) {
        if (!validMapped(w) || w->isHidden())
            continue;
        SeamState::onWindowOpened(w);
    }

    // Debug-only: lets us confirm the tracking/adjacency/animation state machine
    // works before Task 7 adds the real render hook. Kept (rather than removed)
    // per the task brief's option to leave it documented as debug-only; safe to
    // delete once Task 7 lands and there's a visual way to check this instead.
    HyprlandAPI::addDispatcherV2(PHANDLE, "seam:debugstate", [](std::string) -> SDispatchResult {
        HyprlandAPI::addNotification(PHANDLE, "[hypr-seam debug]\n" + SeamState::debugDump(), CHyprColor{0.4, 0.7, 1.0, 1.0}, 8000);
        return {};
    });

    // Task 6 spike: log-only hook on Render::IElementRenderer::drawSurface.
    // On failure install() has already shown a notification; the plugin stays
    // loaded but inert for rendering.
    SeamHook::install();

    HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);

    return {"hypr-seam", "Per-corner window rounding with a book-seam adjacency flag.", "you", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    SeamHook::remove();
}
