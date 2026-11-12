#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>

#include <lua.h>
#include <lauxlib.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <array>

#include "globals.hpp"
#include "SeamConfig.hpp"
#include "SeamRuleStore.hpp"
#include "SeamState.hpp"
#include "SeamHook.hpp"
#include "SeamDecorHook.hpp"

namespace {
    // Notification styling: errors use kNotifyTimeoutMs, init status toasts the shorter one.
    const CHyprColor kErrorColor{1.0, 0.2, 0.2, 1.0};
    constexpr int     kNotifyTimeoutMs      = 8000;
    constexpr int     kShortNotifyTimeoutMs = 5000;
}

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// Hyprlang `seamrule = ...` handler. Handlers carry no state, so rules go to SeamRuleStore.
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
    // Listener handles, kept at namespace scope so PLUGIN_EXIT can drop them. A function-local
    // static would only die at DSO teardown, leaving callbacks into an unloaded plugin.
    std::vector<Hyprutils::Signal::CHyprSignalListener> g_listeners;

    // Reads an optional regex field. Returns false (with `err` set) if present but not a string.
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

    // Appends the selector on top of the Lua stack. Numbers are read as numbers, not via
    // lua_tostring, which would turn 2.0 into the name "2.0". Returns false with `err` set.
    bool luaWorkspaceSelector(lua_State* L, std::vector<WorkspaceSelector>& out, std::string& err) {
        if (lua_type(L, -1) == LUA_TNUMBER) {
            // Range-checked before the cast so the conversion is defined.
            const lua_Number n = lua_tonumber(L, -1);
            if (n != std::floor(n) || n < 0 || n > static_cast<lua_Number>(kMaxWorkspaceId)) {
                err = "'workspace' numbers must be whole workspace IDs (0 or higher)";
                return false;
            }
            out.emplace_back(SWorkspaceId{static_cast<std::int64_t>(n)});
            return true;
        }

        if (lua_type(L, -1) == LUA_TSTRING) {
            auto selector = parseWorkspaceSelector(lua_tostring(L, -1));
            if (!selector) {
                err = "'workspace' is empty, a numeric name, or an out-of-range ID";
                return false;
            }
            out.push_back(std::move(*selector));
            return true;
        }

        err = "'workspace' must be a workspace ID, a name, or a list of those";
        return false;
    }

    // Reads the optional `workspace` field: one selector or a non-empty list. Absent = any workspace.
    bool luaOptionalWorkspaces(lua_State* L, int tableIdx, std::vector<WorkspaceSelector>& out, std::string& err) {
        lua_getfield(L, tableIdx, "workspace");
        bool ok = true;

        if (lua_type(L, -1) == LUA_TTABLE) {
            const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, -1));
            if (count == 0) {
                err = "'workspace' list must not be empty";
                ok  = false;
            }
            for (lua_Integer i = 1; ok && i <= count; ++i) {
                lua_rawgeti(L, -1, i);
                ok = luaWorkspaceSelector(L, out, err);
                lua_pop(L, 1);
            }
        } else if (!lua_isnil(L, -1))
            ok = luaWorkspaceSelector(L, out, err);

        lua_pop(L, 1);
        return ok;
    }

    // Parses the table form of hl.plugin.seam.rule:
    //   { class = <regex>, title = <regex>, workspace = <id | name | {...}>,
    //     seam = <bool|0|1>, rounding = <n | {tl, tr, bl, br}> }
    // Needs at least one matcher and at least one directive. Each directive becomes its own rule.
    bool parseLuaRuleTable(lua_State* L, int idx, std::vector<SSeamRule>& out, std::string& err) {
        SSeamRule base{};
        if (!luaOptionalString(L, idx, "class", base.classPattern, err) || !luaOptionalString(L, idx, "title", base.titlePattern, err) ||
            !luaOptionalWorkspaces(L, idx, base.workspaces, err))
            return false;
        if (base.classPattern.empty() && base.titlePattern.empty() && base.workspaces.empty()) {
            err = "needs a non-empty 'class' and/or 'title' regex, and/or a 'workspace'";
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

    // hl.plugin.seam.rule: takes a rule string or a table.
    //   hl.plugin.seam.rule("seam 0, class:^(foot)$")
    //   hl.plugin.seam.rule({ workspace = { 1, 2, 3 }, seam = true })
    //   hl.plugin.seam.rule({ workspace = 2, class = "^(kitty)$", rounding = 0 })
    // Last matching rule wins, whatever it matched on.
    //
    // Errors go to the config error list instead of lua_error: raising would abort the rest of
    // the config, and lua_error longjmps across C++ destructors.
    int luaSeamRule(lua_State* L) {
        std::string            err;
        std::vector<SSeamRule> parsed;

        if (lua_type(L, 1) == LUA_TSTRING) {
            SSeamRule rule;
            if (parseSeamRuleLine(lua_tostring(L, 1), rule))
                parsed.push_back(rule);
            else
                err = "malformed rule string (expected e.g. \"seam 0, class:^(foot)$\", \"rounding 4 4 22 22, title:^(x)$\" or \"seam 1, workspace:2\")";
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

    // hl.plugin.seam.debugstate: Lua replacement for the `seam:debugstate` dispatcher, which
    // Lua configs can't reach. Returns and logs the dump:
    //   hyprctl eval 'return hl.plugin.seam.debugstate()'
    int luaSeamDebugState(lua_State* L) {
        {
            const auto dump = SeamState::debugDump();
            Log::logger->log(Log::INFO, "hypr-seam", "[hypr-seam debugstate]\n{}", dump);
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

    // Both rule entry points are registered, but each only succeeds under its own config
    // manager (keyword: hyprlang, function: Lua). Only the active one's failure is reported.
    const bool keywordOk = HyprlandAPI::addConfigKeyword(PHANDLE, "seamrule", onSeamRule, Hyprlang::SHandlerOptions{});
    const bool luaOk     = HyprlandAPI::addLuaFunction(PHANDLE, "seam", "rule", luaSeamRule);
    // Diagnostic only; failure is not reported.
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

// Default tolerance (plugin:seam:tolerance = -1), derived from general:gaps_in. Real sibling
// gaps are wider than gaps_in because borders add to them; measured 12-26px gaps all fall
// inside 3x+6. Set plugin:seam:tolerance explicitly to override.
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

// Resolves the global seam defaults from live plugin:seam:* values (declared in globals.hpp).
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

// Warns if decoration:rounding isn't 0; native rounding conflicts with the plugin's.
static void checkNativeRoundingIsZero() {
    static auto PROUNDING = CConfigValue<Config::INTEGER>("decoration:rounding");
    if (*PROUNDING != 0) {
        HyprlandAPI::addNotification(
            PHANDLE, "[hypr-seam] decoration:rounding is not 0 — this plugin fully replaces native rounding and requires it. Set decoration:rounding = 0.",
            CHyprColor{1.0, 0.6, 0.0, 1.0}, kNotifyTimeoutMs);
    }
}

// Registers config and window listeners.
//
// Rules are cleared on preReload because a reload re-runs every rule registration without
// clearing the store. Hyprland has no move/resize or `hyprctl keyword` event, so those are
// caught on `tick` behind SeamState::onTick()'s dirty check; `tick` only fires while
// something animates.
static void registerEventListeners() {
    g_listeners.emplace_back(Event::bus()->m_events.config.preReload.listen([]() { SeamRuleStore::clear(); }));
    // A full reload can change rules, radii or enabled; recompute instead of waiting for an event.
    g_listeners.emplace_back(Event::bus()->m_events.config.reloaded.listen([]() {
        checkNativeRoundingIsZero();
        SeamState::recomputeAll();
    }));
    // Cheap extra in case a config change fires props_refreshed without a full reload.
    // `hyprctl keyword` does not fire it; onTick() covers that.
    g_listeners.emplace_back(Event::bus()->m_events.config.props_refreshed.listen([](const bool) { SeamState::recomputeAll(); }));

    g_listeners.emplace_back(Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { SeamState::onWindowOpened(w); }));
    g_listeners.emplace_back(Event::bus()->m_events.window.close.listen([](PHLWINDOW w) { SeamState::onWindowClosed(w); }));
    // A new title or class can change which rules match.
    g_listeners.emplace_back(Event::bus()->m_events.window.title.listen([](PHLWINDOW w) { SeamState::recomputeWindow(w); }));
    g_listeners.emplace_back(Event::bus()->m_events.window.class_.listen([](PHLWINDOW w) { SeamState::recomputeWindow(w); }));
    g_listeners.emplace_back(Event::bus()->m_events.window.floating.listen([](PHLWINDOW) { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW) { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE) { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE) { SeamState::recomputeAll(); }));
    // Name selectors also match the display name, which a rename changes without any
    // geometry change for onTick() to notice.
    g_listeners.emplace_back(Event::bus()->m_events.workspace.renamed.listen([](PHLWORKSPACEREF) { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.monitor.added.listen([](PHLMONITOR) { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR) { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.monitor.layoutChanged.listen([]() { SeamState::recomputeAll(); }));
    g_listeners.emplace_back(Event::bus()->m_events.tick.listen([]() { SeamState::onTick(); }));
}

// Tracks windows already open at load time. Hidden windows (inactive group members) are
// included, since they fire no window.open when shown again.
static void attachExistingWindows() {
    for (auto& w : Desktop::windowState()->windows()) {
        if (!validMapped(w))
            continue;
        SeamState::onWindowOpened(w);
    }
}

// `hyprctl dispatch seam:debugstate`: shows and logs every tracked window's live radii,
// with '*' on corners flagged as touching. For bug reports.
static void registerDebugDispatcher() {
    HyprlandAPI::addDispatcherV2(PHANDLE, "seam:debugstate", [](std::string) -> SDispatchResult {
        const auto dump = SeamState::debugDump();
        Log::logger->log(Log::INFO, "hypr-seam", "[hypr-seam debugstate]\n{}", dump);
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

    // Per-corner surface rounding (SeamHook.cpp). On failure install() has already notified,
    // and the plugin stays loaded without rendering.
    SeamHook::install();
    SeamDecorHook::install();

    HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, kShortNotifyTimeoutMs);

    return {"hypr-seam", "Per-corner window rounding with a book-seam adjacency flag.", "misaid", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Order matters: hooks first, then listeners so nothing calls into SeamState, then tracked
    // windows, then rules. Hyprland unregisters the plugin's functions, keyword, dispatcher
    // and config values itself.
    SeamDecorHook::remove();
    SeamHook::remove();
    g_listeners.clear();
    SeamState::clear();
    SeamRuleStore::clear();
}
