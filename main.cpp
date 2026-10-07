#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>

#include <lua.h>
#include <lauxlib.h>

#include <algorithm>
#include <string>
#include <vector>

#include <array>

#include "globals.hpp"
#include "SeamConfig.hpp"
#include "SeamRuleStore.hpp"
#include "SeamState.hpp"
#include "SeamHook.hpp"
#include "SeamDecorHook.hpp"

namespace {
    // Shared notification styling. kErrorColor is reused across every hard-failure
    // notification (version mismatch, failed rule registration, malformed Lua rule);
    // the two timeouts are genuinely distinct durations in the UI, not duplicates of
    // each other: short-lived init status toasts use kShortNotifyTimeoutMs, while
    // diagnostic/error messages the user may need longer to read use kNotifyTimeoutMs.
    const CHyprColor kErrorColor{1.0, 0.2, 0.2, 1.0};
    constexpr int     kNotifyTimeoutMs      = 8000;
    constexpr int     kShortNotifyTimeoutMs = 5000;
}

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

namespace {
    // Every event-bus listener handle the plugin owns. Kept at namespace scope (not as
    // function-local statics inside PLUGIN_INIT) so PLUGIN_EXIT can drop them: a listener
    // is unregistered when its handle is destroyed, and a function-local static is only
    // destroyed at DSO teardown — which may never happen promptly (or at all) on plugin
    // unload, leaving callbacks into our state live after the plugin is gone.
    std::vector<Hyprutils::Signal::CHyprSignalListener> g_listeners;

    // Reads one rule-table field as a regex pattern string. Returns false (with `err` set)
    // if present but not a string.
    bool luaOptionalString(lua_State* L, int tableIdx, const char* field, std::string& out, std::string& err) {
        lua_getfield(L, tableIdx, field);
        bool ok = true;
        if (lua_type(L, -1) == LUA_TSTRING)
            out = lua_tostring(L, -1);
        else if (!lua_isnil(L, -1)) {
            err = std::string{"'"} + field + "' must be a string (a regex)";
            ok  = false;
        }
        lua_pop(L, 1);
        return ok;
    }

    // Parses the table form of hl.plugin.seam.rule:
    //   { class = "<regex>", title = "<regex>", seam = <bool|0|1>, rounding = <n | {tl, tr, bl, br}> }
    // At least one of class/title, and at least one of seam/rounding, is required. Produces
    // one rule per directive given (so seam + rounding in one call yields two rules).
    bool parseLuaRuleTable(lua_State* L, int idx, std::vector<SSeamRule>& out, std::string& err) {
        SSeamRule base{};
        if (!luaOptionalString(L, idx, "class", base.classPattern, err) || !luaOptionalString(L, idx, "title", base.titlePattern, err))
            return false;
        if (base.classPattern.empty() && base.titlePattern.empty()) {
            err = "needs a non-empty 'class' and/or 'title' regex";
            return false;
        }

        bool ok = true;

        lua_getfield(L, idx, "rounding");
        if (lua_type(L, -1) == LUA_TNUMBER) {
            const double r = lua_tonumber(L, -1);
            SSeamRule    rule = base;
            rule.isSeamDirective = false;
            rule.radii           = {r, r, r, r};
            out.push_back(rule);
        } else if (lua_type(L, -1) == LUA_TTABLE) {
            std::array<double, 4> v{};
            for (int i = 0; i < 4 && ok; ++i) {
                lua_rawgeti(L, -1, i + 1);
                if (lua_type(L, -1) != LUA_TNUMBER)
                    ok = false;
                else
                    v[i] = lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
            if (ok && lua_rawlen(L, -1) != 4)
                ok = false;
            if (!ok)
                err = "'rounding' table must be exactly 4 numbers: { tl, tr, bl, br }";
            else {
                SSeamRule rule       = base;
                rule.isSeamDirective = false;
                rule.radii           = {v[0], v[1], v[2], v[3]};
                out.push_back(rule);
            }
        } else if (!lua_isnil(L, -1)) {
            err = "'rounding' must be a number or a table of 4 numbers";
            ok  = false;
        }
        lua_pop(L, 1);
        if (!ok)
            return false;

        lua_getfield(L, idx, "seam");
        if (lua_type(L, -1) == LUA_TBOOLEAN || lua_type(L, -1) == LUA_TNUMBER) {
            SSeamRule rule       = base;
            rule.isSeamDirective = true;
            rule.seamOn          = lua_type(L, -1) == LUA_TBOOLEAN ? lua_toboolean(L, -1) : lua_tonumber(L, -1) != 0;
            out.push_back(rule);
        } else if (!lua_isnil(L, -1)) {
            err = "'seam' must be a boolean (or 0/1)";
            ok  = false;
        }
        lua_pop(L, 1);
        if (!ok)
            return false;

        if (out.empty()) {
            err = "needs 'seam' and/or 'rounding'";
            return false;
        }
        return true;
    }

    // Lua entry point for per-app rules under the Lua config manager, registered as
    // hl.plugin.seam.rule. Accepts either the same string a hyprlang `seamrule = ...` line
    // takes, or a table:
    //   hl.plugin.seam.rule("seam 0, class:^(foot)$")
    //   hl.plugin.seam.rule({ class = "^(kitty)$", rounding = { 4, 4, 22, 22 } })
    //   hl.plugin.seam.rule({ title = "^Picture-in-Picture$", seam = false })
    //
    // Errors are reported the way Hyprland's own Lua bindings report config errors (added
    // to the config error list, shown in the error bar) rather than by raising a Lua error:
    // raising would abort the rest of the user's config file, and lua_error longjmps, which
    // must never cross live C++ objects with destructors.
    int luaSeamRule(lua_State* L) {
        std::string            err;
        std::vector<SSeamRule> parsed;

        if (lua_type(L, 1) == LUA_TSTRING) {
            SSeamRule rule;
            if (parseSeamRuleLine(lua_tostring(L, 1), rule))
                parsed.push_back(rule);
            else
                err = "malformed rule string (expected e.g. \"seam 0, class:^(foot)$\" or \"rounding 4 4 22 22, title:^(x)$\")";
        } else if (lua_type(L, 1) == LUA_TTABLE)
            parseLuaRuleTable(L, 1, parsed, err);
        else
            err = "expected a rule string or a table";

        if (!err.empty()) {
            luaL_where(L, 1);
            std::string where = lua_tostring(L, -1);
            lua_pop(L, 1);
            const std::string msg = where + "hl.plugin.seam.rule: " + err;
            if (auto* mgr = Config::Lua::CConfigManager::fromLuaState(L))
                mgr->addError(std::string{msg});
            else
                HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] " + msg, kErrorColor, kNotifyTimeoutMs);
            return 0;
        }

        for (const auto& r : parsed)
            SeamRuleStore::add(r);
        return 0;
    }

    // Lua-side twin of the `seam:debugstate` dispatcher, registered as
    // hl.plugin.seam.debugstate. Under a Lua config, plugin dispatchers added with
    // addDispatcherV2 aren't reachable from `hyprctl dispatch` (which becomes
    // `hl.dispatch(<lua expr>)`), so this is the way to get the same diagnostic there:
    //   hyprctl eval 'return hl.plugin.seam.debugstate()'
    // Returns the dump as a string (and logs it, like the dispatcher).
    int luaSeamDebugState(lua_State* L) {
        {
            const auto dump = SeamState::debugDump();
            Log::logger->log(Log::INFO, "[hypr-seam debugstate]\n{}", dump);
            lua_pushstring(L, dump.c_str());
        }
        return 1;
    }
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
    vars.tolerance           = makeShared<Config::Values::CIntValue>("plugin:seam:tolerance", "Max px gap still considered touching, or -1 to derive it from general:gaps_in", -1);
    vars.animate             = makeShared<Config::Values::CBoolValue>("plugin:seam:animate", "Ease corner radius changes", true);
    vars.animationSpeed      = makeShared<Config::Values::CFloatValue>("plugin:seam:animation_speed", "Transition duration in ms", 300.0F);
    vars.animationCurve      = makeShared<Config::Values::CStringValue>("plugin:seam:animation_curve", "Bezier curve name", "default");
    vars.forceRoundRiskySurfaces =
        makeShared<Config::Values::CBoolValue>("plugin:seam:force_round_risky_surfaces", "Also round subsurfaces that reach a window corner (Firefox/Zen page content)", false);
    vars.roundBorders = makeShared<Config::Values::CBoolValue>("plugin:seam:round_borders", "Round the native border to match this window's live corner radii", false);
    vars.roundShadows = makeShared<Config::Values::CBoolValue>("plugin:seam:round_shadows", "Round the native drop shadow to match this window's live corner radii", false);

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
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.forceRoundRiskySurfaces);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.roundBorders);
    HyprlandAPI::addConfigValueV2(PHANDLE, vars.roundShadows);

    // Per-app rules have two entry points, one per config flavour; both are always
    // attempted (additive), but each API only succeeds under its own config manager:
    // addConfigKeyword returns false unless Hyprland is running a legacy hyprlang .conf,
    // and addLuaFunction returns false unless it's running a Lua config (both confirmed
    // against the 0.56.2 PluginAPI.cpp). So only the one matching the active config
    // manager is expected to succeed — complain if THAT one didn't.
    const bool keywordOk = HyprlandAPI::addConfigKeyword(PHANDLE, "seamrule", onSeamRule, Hyprlang::SHandlerOptions{});
    const bool luaOk     = HyprlandAPI::addLuaFunction(PHANDLE, "seam", "rule", luaSeamRule);
    // Diagnostic only; not worth a notification if it fails (the dispatcher remains).
    HyprlandAPI::addLuaFunction(PHANDLE, "seam", "debugstate", luaSeamDebugState);

    const bool isLua = Config::mgr() && Config::mgr()->type() == Config::CONFIG_LUA;
    if (isLua ? !luaOk : !keywordOk) {
        HyprlandAPI::addNotification(PHANDLE,
                                     isLua ? "[hypr-seam] Failed to register hl.plugin.seam.rule — per-app seam rules will not work (see hyprland.log)." :
                                             "[hypr-seam] Failed to register the seamrule keyword — per-app seam rules will not work.",
                                     kErrorColor, kNotifyTimeoutMs);
    }

    HyprlandAPI::reloadConfig();
}

// plugin:seam:tolerance left at its default (-1) derives a touch-tolerance from
// general:gaps_in instead of a fixed px value, so two tiled windows at the layout's own
// default spacing are recognized as touching without the user needing to discover and tune
// tolerance themselves. The actual on-screen gap between two dwindle/master siblings runs
// wider than gaps_in alone (border_size adds to it too, and the exact relationship isn't a
// clean documented multiple), so this errs generous -- measured gaps of 12-26px against
// gaps_in values of 4-10px (with border_size 2-3px) all land comfortably inside 3x+6 -- rather
// than risk the same silent non-match this is meant to fix. A real false-touching cost would
// only show up for a deliberately tiny seam_radius with an unusually large intentional gap;
// anyone who wants that sets plugin:seam:tolerance explicitly and this is bypassed entirely.
double resolveTolerance() {
    const Config::INTEGER val = vars.tolerance->value();
    if (val >= 0)
        return static_cast<double>(val);

    static auto PGAPSIN = CConfigValue<Config::IComplexConfigValue>("general:gaps_in");
    const auto* gaps    = dynamic_cast<const Config::CCssGapData*>(PGAPSIN.ptr());
    if (!gaps)
        return 8.0;

    const int64_t maxGap = std::max({gaps->m_top, gaps->m_right, gaps->m_bottom, gaps->m_left});
    return std::max<double>(8.0, static_cast<double>(maxGap) * 3.0 + 6.0);
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
        .tolerance     = resolveTolerance(),
    };
}

// Warns if the user hasn't zeroed decoration:rounding, since this plugin fully
// replaces native corner rounding and will visually conflict with it otherwise.
static void checkNativeRoundingIsZero() {
    static auto PROUNDING = CConfigValue<Config::INTEGER>("decoration:rounding");
    if (*PROUNDING != 0) {
        HyprlandAPI::addNotification(
            PHANDLE, "[hypr-seam] decoration:rounding is not 0 — this plugin fully replaces native rounding and requires it. Set decoration:rounding = 0.",
            CHyprColor{1.0, 0.6, 0.0, 1.0}, kNotifyTimeoutMs);
    }
}

// Hyprlang re-invokes the `seamrule` keyword handler (and a Lua config re-runs its
// hl.plugin.seam.rule calls) for every rule on each config reload, but never
// clears state our plugin owns (SeamRuleStore) — so without this, rules
// re-parsed on reload would simply append onto the old ones forever. Clear on
// preReload (emitted by both config managers before the config is re-run),
// and re-check the rounding guard on reloaded (after the new values land).
//
// Also covers window tracking + adjacency recompute triggers (Task 5). Event names
// below were confirmed against the installed <hyprland/src/event/EventBus.hpp>
// rather than trusted from the plan verbatim:
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
static void registerEventListeners() {
    g_listeners.emplace_back(Event::bus()->m_events.config.preReload.listen([]() { SeamRuleStore::clear(); }));
    // Also recompute on full config reload (e.g. `hyprctl reload` after editing
    // seamrule lines, radii, or plugin:seam:enabled in the config file) — without
    // this, a reload leaves every tracked window's corners stale until some
    // unrelated geometry/workspace/monitor event happens to fire.
    g_listeners.emplace_back(Event::bus()->m_events.config.reloaded.listen([]() {
        checkNativeRoundingIsZero();
        SeamState::recomputeAll();
    }));
    // config.props_refreshed's exact trigger conditions aren't documented in the
    // installed EventBus.hpp beyond its name and `Event<const bool>` signature.
    // Wired as a defensive extra alongside the guaranteed `reloaded` path above,
    // in case some config-value change path fires it without a full reload —
    // but verified empirically (fix-round-1 manual testing) that it does NOT
    // fire for a plain `hyprctl keyword plugin:seam:... <value>` set, so that
    // path still relies on an explicit window/workspace/monitor event or a full
    // `hyprctl reload` to pick up the new value; this listener is cheap and
    // harmless to keep regardless, in case other config-change paths do fire it.
    g_listeners.emplace_back(Event::bus()->m_events.config.props_refreshed.listen([](const bool) { SeamState::recomputeAll(); }));

    g_listeners.emplace_back(Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { SeamState::onWindowOpened(w); }));
    g_listeners.emplace_back(Event::bus()->m_events.window.close.listen([](PHLWINDOW w) { SeamState::onWindowClosed(w); }));
    g_listeners.emplace_back(Event::bus()->m_events.window.floating.listen([](PHLWINDOW) { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW) { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE) { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE) { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.monitor.added.listen([](PHLMONITOR) { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR) { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.monitor.layoutChanged.listen([]() { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.tick.listen([]() { SeamState::onTick(); }));
}

// Attach to every window already open at load time (the plugin can be
// hot-loaded into a running session with windows already present).
// Includes windows hidden at load time (e.g. inactive members of a window group):
// they never fire window.open again when un-hidden, so skipping them here would leave
// them untracked (and square-cornered) until reopened.
static void attachExistingWindows() {
    for (auto& w : Desktop::windowState()->windows()) {
        if (!validMapped(w))
            continue;
        SeamState::onWindowOpened(w);
    }
}

// Diagnostic dispatcher (`hyprctl dispatch seam:debugstate`): shows a notification
// (and writes the same text to hyprland.log) listing every tracked window's live
// corner radii, '*' marking corners currently flagged as touching a neighbor. Not
// used by the render path; kept on purpose as a troubleshooting aid for bug reports.
static void registerDebugDispatcher() {
    HyprlandAPI::addDispatcherV2(PHANDLE, "seam:debugstate", [](std::string) -> SDispatchResult {
        const auto dump = SeamState::debugDump();
        Log::logger->log(Log::INFO, "[hypr-seam debugstate]\n{}", dump);
        HyprlandAPI::addNotification(PHANDLE, "[hypr-seam debug]\n" + dump, CHyprColor{0.4, 0.7, 1.0, 1.0}, kNotifyTimeoutMs);
        return {};
    });
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     kErrorColor, kShortNotifyTimeoutMs);
        throw std::runtime_error("[hypr-seam] Version mismatch");
    }

    registerSeamConfig();
    checkNativeRoundingIsZero();
    registerEventListeners();
    attachExistingWindows();
    registerDebugDispatcher();

    // Per-corner rendering hook on Render::IElementRenderer::drawSurface (SeamHook.cpp).
    // On failure install() has already shown a notification; the plugin stays
    // loaded but inert for rendering.
    SeamHook::install();
    SeamDecorHook::install();

    HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, kShortNotifyTimeoutMs);

    return {"hypr-seam", "Per-corner window rounding with a book-seam adjacency flag.", "misaid", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Order matters: stop rendering through our hook first, then drop every event
    // listener (so nothing can call back into SeamState), then release the tracked
    // window state and its animated variables, then the parsed rules. hl.plugin.seam.rule,
    // the seamrule keyword, the dispatcher, and plugin:seam:* values are all
    // unregistered by Hyprland itself on unload (PluginSystem / onPluginUnload).
    SeamDecorHook::remove();
    SeamHook::remove();
    g_listeners.clear();
    SeamState::clear();
    SeamRuleStore::clear();
}
