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
    // Shared notification styling. kErrorColor is used for every hard failure (version
    // mismatch, failed rule registration, malformed Lua rule). Init status toasts use
    // kShortNotifyTimeoutMs; errors and diagnostics, which take longer to read, use
    // kNotifyTimeoutMs.
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
    // Every event-bus listener handle the plugin owns. They live at namespace scope so
    // PLUGIN_EXIT can drop them. A listener is unregistered when its handle is destroyed,
    // and a function-local static in PLUGIN_INIT would only be destroyed at DSO teardown,
    // which may happen late or never, leaving callbacks into plugin state after unload.
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
    // Needs class and/or title, and seam and/or rounding. Each directive becomes its own
    // rule, so a table with both seam and rounding produces two rules.
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

    // Per-app rules for Lua configs, registered as hl.plugin.seam.rule. Takes the same
    // string as a hyprlang `seamrule = ...` line, or a table:
    //   hl.plugin.seam.rule("seam 0, class:^(foot)$")
    //   hl.plugin.seam.rule({ class = "^(kitty)$", rounding = { 4, 4, 22, 22 } })
    //   hl.plugin.seam.rule({ title = "^Picture-in-Picture$", seam = false })
    //
    // Errors go to the config error list and show in the error bar, the same way
    // Hyprland's own Lua bindings report them. Raising a Lua error would abort the rest of
    // the user's config, and lua_error longjmps, which must never cross live C++ objects
    // with destructors.
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

    // Lua version of the `seam:debugstate` dispatcher, registered as
    // hl.plugin.seam.debugstate. Under a Lua config, `hyprctl dispatch` becomes
    // `hl.dispatch(<lua expr>)` and can't reach dispatchers added with addDispatcherV2,
    // so use this instead:
    //   hyprctl eval 'return hl.plugin.seam.debugstate()'
    // Returns the dump as a string and logs it, like the dispatcher.
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

    // Per-app rules have one entry point per config format. Both are registered, but
    // each only succeeds under its own config manager: per the 0.56.2 PluginAPI.cpp,
    // addConfigKeyword fails unless Hyprland runs a hyprlang .conf, and addLuaFunction
    // fails unless it runs a Lua config. Notify only if the one for the active config
    // manager failed.
    const bool keywordOk = HyprlandAPI::addConfigKeyword(PHANDLE, "seamrule", onSeamRule, Hyprlang::SHandlerOptions{});
    const bool luaOk     = HyprlandAPI::addLuaFunction(PHANDLE, "seam", "rule", luaSeamRule);
    // Diagnostic only, so no notification on failure. The dispatcher still works.
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

// With plugin:seam:tolerance at its default (-1), the tolerance comes from general:gaps_in,
// so tiled windows at the layout's normal spacing count as touching without any tuning.
// The real gap between two dwindle/master siblings is wider than gaps_in (border_size adds
// to it, and there's no documented formula), so the estimate is generous. Measured gaps of
// 12-26px, with gaps_in of 4-10px and border_size of 2-3px, all fall inside 3x+6. Too wide a
// tolerance only matters with a tiny seam_radius and a large intentional gap, and anyone who
// wants that can set plugin:seam:tolerance explicitly, which skips this.
static double resolveTolerance() {
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
// Declared in globals.hpp.
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

// Warns if decoration:rounding isn't 0. The plugin replaces native corner rounding,
// and the two conflict on screen.
static void checkNativeRoundingIsZero() {
    static auto PROUNDING = CConfigValue<Config::INTEGER>("decoration:rounding");
    if (*PROUNDING != 0) {
        HyprlandAPI::addNotification(
            PHANDLE, "[hypr-seam] decoration:rounding is not 0 — this plugin fully replaces native rounding and requires it. Set decoration:rounding = 0.",
            CHyprColor{1.0, 0.6, 0.0, 1.0}, kNotifyTimeoutMs);
    }
}

// On every config reload, hyprlang calls the `seamrule` handler again for each rule (a Lua
// config re-runs its hl.plugin.seam.rule calls), but never clears SeamRuleStore. Without
// the preReload clear, reloaded rules would pile up on top of the old ones. Both config
// managers emit preReload before re-running the config. The rounding check runs on
// reloaded, after the new values are in.
//
// Also registers the window-tracking and adjacency-recompute triggers. Hyprland has no
// window move/resize event, so geometry changes are caught on `tick`, behind a cheap
// dirty check (SeamState::onTick()) that keeps idle ticks nearly free.
static void registerEventListeners() {
    g_listeners.emplace_back(Event::bus()->m_events.config.preReload.listen([]() { SeamRuleStore::clear(); }));
    // Recompute on a full config reload, such as `hyprctl reload` after editing rules,
    // radii or plugin:seam:enabled. Otherwise corners stay stale until some unrelated
    // geometry, workspace or monitor event fires.
    g_listeners.emplace_back(Event::bus()->m_events.config.reloaded.listen([]() {
        checkNativeRoundingIsZero();
        SeamState::recomputeAll();
    }));
    // EventBus.hpp doesn't document when config.props_refreshed fires. This listener is
    // a cheap extra in case some config change fires it without a full reload. Tested:
    // `hyprctl keyword plugin:seam:... <value>` does not fire it, so a keyword change
    // still waits for a window, workspace or monitor event, or a `hyprctl reload`.
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

// Tracks every window already open at load time, since the plugin can be loaded into
// a running session. Hidden windows (such as inactive members of a group) are included:
// they don't fire window.open when shown again, so skipping them would leave them
// untracked and square until reopened.
static void attachExistingWindows() {
    for (auto& w : Desktop::windowState()->windows()) {
        if (!validMapped(w))
            continue;
        SeamState::onWindowOpened(w);
    }
}

// `hyprctl dispatch seam:debugstate` shows a notification, and writes the same text to
// hyprland.log, listing every tracked window's live corner radii. '*' marks corners
// flagged as touching a neighbor. The render path doesn't use it; it's for bug reports.
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
    // If install() fails it has already shown a notification, and the plugin stays
    // loaded without rendering anything.
    SeamHook::install();
    SeamDecorHook::install();

    HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, kShortNotifyTimeoutMs);

    return {"hypr-seam", "Per-corner window rounding with a book-seam adjacency flag.", "misaid", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Order matters: remove the render hooks, then drop the event listeners so nothing
    // calls back into SeamState, then release tracked windows and their animated
    // variables, then the parsed rules. Hyprland unregisters hl.plugin.seam.rule, the
    // seamrule keyword, the dispatcher and the plugin:seam:* values on unload
    // (PluginSystem / onPluginUnload).
    SeamDecorHook::remove();
    SeamHook::remove();
    g_listeners.clear();
    SeamState::clear();
    SeamRuleStore::clear();
}
