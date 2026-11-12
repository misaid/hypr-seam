#include "../src/SeamConfig.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static SGlobalSeamDefaults defaultDefaults() {
    return SGlobalSeamDefaults{
        .baseRadii = {22, 22, 22, 22},
        .roundingPower = 2.0,
        .seamEnabled = false,
        .seamRadius = 2,
        .tolerance = 6,
    };
}

static void test_no_rules_uses_global_defaults() {
    auto result = resolveWindowConfig("kitty", "", false, defaultDefaults(), {});
    CHECK(result.radii.topLeft == 22);
    CHECK(result.seamEnabled == false);
}

static void test_rounding_rule_matches_by_class() {
    SSeamRule rule{.isSeamDirective = false, .radii = {4, 4, 22, 22}, .classPattern = "^(kitty)$"};
    auto result = resolveWindowConfig("kitty", "", false, defaultDefaults(), {rule});
    CHECK(result.radii.topLeft == 4 && result.radii.bottomRight == 22);

    auto nonMatch = resolveWindowConfig("foot", "", false, defaultDefaults(), {rule});
    CHECK(nonMatch.radii.topLeft == 22); // falls back to global default
}

static void test_seam_rule_force_on() {
    SSeamRule rule{.isSeamDirective = true, .seamOn = true, .classPattern = "^(mpv)$"};
    auto result = resolveWindowConfig("mpv", "", false, defaultDefaults(), {rule});
    CHECK(result.seamEnabled == true);
}

static void test_seam_rule_force_off_overrides_global_enabled() {
    auto defaults = defaultDefaults();
    defaults.seamEnabled = true; // global ON
    SSeamRule rule{.isSeamDirective = true, .seamOn = false, .classPattern = "^(foot)$"};
    auto result = resolveWindowConfig("foot", "", false, defaults, {rule});
    CHECK(result.seamEnabled == false); // per-app opt-out wins
}

static void test_floating_window_never_gets_seam_regardless_of_rules() {
    auto defaults = defaultDefaults();
    defaults.seamEnabled = true;
    SSeamRule rule{.isSeamDirective = true, .seamOn = true, .classPattern = "^(anything)$"};
    auto result = resolveWindowConfig("anything", "", /*isFloating=*/true, defaults, {rule});
    CHECK(result.seamEnabled == false);
}

static void test_parse_rounding_rule_line() {
    SSeamRule rule;
    CHECK(parseSeamRuleLine("rounding 4 4 22 22, class:^(kitty)$", rule));
    CHECK(!rule.isSeamDirective);
    CHECK(rule.radii.topLeft == 4 && rule.radii.topRight == 4);
    CHECK(rule.radii.bottomLeft == 22 && rule.radii.bottomRight == 22);
    CHECK(rule.classPattern == "^(kitty)$");
}

static void test_parse_seam_rule_line() {
    SSeamRule rule;
    CHECK(parseSeamRuleLine("seam 0, class:^(foot)$", rule));
    CHECK(rule.isSeamDirective);
    CHECK(rule.seamOn == false);
    CHECK(rule.classPattern == "^(foot)$");
}

static void test_parse_malformed_line_fails() {
    SSeamRule rule;
    CHECK(!parseSeamRuleLine("not a valid rule", rule));
    CHECK(!parseSeamRuleLine("rounding 4 4 22, class:^(kitty)$", rule)); // only 3 numbers
}

static void test_title_rule_matches_by_title_only() {
    SSeamRule rule{.isSeamDirective = true, .seamOn = true, .titlePattern = "^Picture-in-Picture$"};
    auto      hit = resolveWindowConfig("firefox", "Picture-in-Picture", false, defaultDefaults(), {rule});
    CHECK(hit.seamEnabled == true);
    auto miss = resolveWindowConfig("firefox", "Mozilla Firefox", false, defaultDefaults(), {rule});
    CHECK(miss.seamEnabled == false);
}

static void test_class_and_title_both_required_when_both_set() {
    SSeamRule rule{.isSeamDirective = false, .radii = {1, 2, 3, 4}, .classPattern = "^kitty$", .titlePattern = "vim"};
    CHECK(resolveWindowConfig("kitty", "nvim foo", false, defaultDefaults(), {rule}).radii.topLeft == 1);
    CHECK(resolveWindowConfig("kitty", "zsh", false, defaultDefaults(), {rule}).radii.topLeft == 22);
    CHECK(resolveWindowConfig("foot", "nvim foo", false, defaultDefaults(), {rule}).radii.topLeft == 22);
}

static void test_rule_without_any_pattern_never_matches() {
    SSeamRule rule{.isSeamDirective = true, .seamOn = true};
    CHECK(resolveWindowConfig("kitty", "x", false, defaultDefaults(), {rule}).seamEnabled == false);
}

static void test_parse_title_rule_line() {
    SSeamRule rule;
    CHECK(parseSeamRuleLine("seam 1, title:^(Picture-in-Picture)$", rule));
    CHECK(rule.isSeamDirective && rule.seamOn);
    CHECK(rule.titlePattern == "^(Picture-in-Picture)$");
    CHECK(rule.classPattern.empty());
}

static void test_parse_trailing_garbage_fails() {
    SSeamRule rule;
    CHECK(!parseSeamRuleLine("seam 1 junk, class:^(foot)$", rule));
    CHECK(!parseSeamRuleLine("seam 1junk, class:^(foot)$", rule));
    CHECK(!parseSeamRuleLine("rounding 1 2 3 4 5, class:^(foot)$", rule));
    CHECK(!parseSeamRuleLine("seam 1, class:", rule)); // empty pattern
    CHECK(!parseSeamRuleLine("seam 1, monitor:2", rule)); // unknown matcher
    CHECK(parseSeamRuleLine("seam 1 , class:^(foot)$ ", rule)); // surrounding whitespace is fine
    CHECK(rule.classPattern == "^(foot)$");
}

// Regression: a bare `SSeamRule rule;` (as in onSeamRule()) must not leave scalar
// members indeterminate.
static void test_default_constructed_rule_has_safe_defaults() {
    SSeamRule rule;
    CHECK(rule.isSeamDirective == false);
    CHECK(rule.seamOn == false);
    CHECK(rule.radii.topLeft == 0.0);
    CHECK(rule.radii.topRight == 0.0);
    CHECK(rule.radii.bottomLeft == 0.0);
    CHECK(rule.radii.bottomRight == 0.0);
    CHECK(rule.classPattern.empty());
    CHECK(rule.titlePattern.empty());
}

// Regression: rules follow the window's current title, as re-resolved by
// SeamState::recomputeWindow() on every title change.
static void test_rule_follows_title_changes() {
    SSeamRule rule{.isSeamDirective = false, .radii = {0, 0, 0, 0}, .titlePattern = "nvim"};
    const std::vector<SSeamRule> rules{rule};

    CHECK(resolveWindowConfig("kitty", "~", false, defaultDefaults(), rules).radii.topLeft == 22);
    CHECK(resolveWindowConfig("kitty", "nvim src/main.cpp", false, defaultDefaults(), rules).radii.topLeft == 0);
    CHECK(resolveWindowConfig("kitty", "~", false, defaultDefaults(), rules).radii.topLeft == 22);
}

// Patterns are compiled once and cached. A cached pattern must keep matching and
// rejecting exactly as a fresh one would.
static void test_cached_pattern_keeps_matching() {
    SSeamRule rule{.isSeamDirective = true, .seamOn = true, .classPattern = "^(mpv)$"};
    for (int i = 0; i < 3; ++i) {
        CHECK(resolveWindowConfig("mpv", "", false, defaultDefaults(), {rule}).seamEnabled == true);
        CHECK(resolveWindowConfig("mpvx", "", false, defaultDefaults(), {rule}).seamEnabled == false);
    }
}

// A malformed pattern is cached as malformed, so the rule is skipped on every resolve,
// not only the first, and later valid rules still apply.
static void test_malformed_pattern_skipped_every_time() {
    SSeamRule broken{.isSeamDirective = false, .radii = {1, 1, 1, 1}, .classPattern = "(kitty"};
    SSeamRule valid{.isSeamDirective = true, .seamOn = true, .classPattern = "^(kitty)$"};
    for (int i = 0; i < 3; ++i) {
        const auto result = resolveWindowConfig("kitty", "", false, defaultDefaults(), {broken, valid});
        CHECK(result.radii.topLeft == 22);
        CHECK(result.seamEnabled == true);
    }
}

// --- Workspace matching ---------------------------------------------------------

static WorkspaceSelector idSel(std::int64_t id) {
    return SWorkspaceId{id};
}

static WorkspaceSelector nameSel(const char* name) {
    return SWorkspaceName{name};
}

// A rule with only a workspace matcher applies to every window on that workspace and to
// none elsewhere.
static void test_workspace_only_rule_matches_that_workspace() {
    SSeamRule rule{.isSeamDirective = true, .seamOn = true, .workspaces = {idSel(2)}};
    CHECK(resolveWindowConfig("kitty", "", false, defaultDefaults(), {rule}, SWorkspaceRef{2, "2"}).seamEnabled == true);
    CHECK(resolveWindowConfig("kitty", "", false, defaultDefaults(), {rule}, SWorkspaceRef{1, "1"}).seamEnabled == false);
}

// A list of workspaces is an OR: any entry matching is enough.
static void test_workspace_list_matches_any_entry() {
    SSeamRule rule{.isSeamDirective = true, .seamOn = true, .workspaces = {idSel(1), idSel(2), idSel(3)}};
    for (std::int64_t id = 1; id <= 3; ++id)
        CHECK(resolveWindowConfig("x", "", false, defaultDefaults(), {rule}, SWorkspaceRef{id, std::to_string(id)}).seamEnabled == true);
    CHECK(resolveWindowConfig("x", "", false, defaultDefaults(), {rule}, SWorkspaceRef{4, "4"}).seamEnabled == false);
}

// Matchers within one rule are an AND: workspace and class must both match.
static void test_workspace_and_class_both_required() {
    SSeamRule rule{.isSeamDirective = false, .radii = {0, 0, 0, 0}, .classPattern = "^kitty$", .workspaces = {idSel(2)}};
    CHECK(resolveWindowConfig("kitty", "", false, defaultDefaults(), {rule}, SWorkspaceRef{2, "2"}).radii.topLeft == 0);
    CHECK(resolveWindowConfig("kitty", "", false, defaultDefaults(), {rule}, SWorkspaceRef{1, "1"}).radii.topLeft == 22);
    CHECK(resolveWindowConfig("foot", "", false, defaultDefaults(), {rule}, SWorkspaceRef{2, "2"}).radii.topLeft == 22);
}

// Order decides between a workspace rule and an app rule, in both directions.
static void test_workspace_and_class_rules_resolve_by_order() {
    SSeamRule wsRule{.isSeamDirective = false, .radii = {0, 0, 0, 0}, .workspaces = {idSel(2)}};
    SSeamRule appRule{.isSeamDirective = false, .radii = {12, 12, 12, 12}, .classPattern = "^firefox$"};
    const SWorkspaceRef ws2{2, "2"};
    CHECK(resolveWindowConfig("firefox", "", false, defaultDefaults(), {wsRule, appRule}, ws2).radii.topLeft == 12);
    CHECK(resolveWindowConfig("firefox", "", false, defaultDefaults(), {appRule, wsRule}, ws2).radii.topLeft == 0);
    CHECK(resolveWindowConfig("kitty", "", false, defaultDefaults(), {wsRule, appRule}, ws2).radii.topLeft == 0);
}

// A window with no workspace (std::nullopt) can never satisfy a workspace matcher.
static void test_workspace_rule_never_matches_window_without_workspace() {
    SSeamRule rule{.isSeamDirective = true, .seamOn = true, .workspaces = {idSel(2)}};
    CHECK(resolveWindowConfig("kitty", "", false, defaultDefaults(), {rule}).seamEnabled == false);
}

// Name selectors match by exact name, which is how named and special workspaces are
// reached, since they have no numeric ID.
static void test_workspace_name_selector() {
    SSeamRule rule{.isSeamDirective = true, .seamOn = true, .workspaces = {nameSel("web"), nameSel("special:scratch")}};
    CHECK(resolveWindowConfig("x", "", false, defaultDefaults(), {rule}, SWorkspaceRef{std::nullopt, "web"}).seamEnabled == true);
    CHECK(resolveWindowConfig("x", "", false, defaultDefaults(), {rule}, SWorkspaceRef{std::nullopt, "special:scratch"}).seamEnabled == true);
    CHECK(resolveWindowConfig("x", "", false, defaultDefaults(), {rule}, SWorkspaceRef{std::nullopt, "web2"}).seamEnabled == false);
    // Named and special workspaces have no ID, so an ID selector cannot match them.
    SSeamRule byId{.isSeamDirective = true, .seamOn = true, .workspaces = {idSel(2)}};
    CHECK(resolveWindowConfig("x", "", false, defaultDefaults(), {byId}, SWorkspaceRef{std::nullopt, "special:scratch"}).seamEnabled == false);
}

// Numbers (bare or as text) become IDs, everything else a name; `name:` is an optional
// prefix. Empty selectors, numeric names and out-of-range IDs are rejected.
static void test_parse_workspace_selector() {
    CHECK(parseWorkspaceSelector("2") == idSel(2));
    CHECK(parseWorkspaceSelector("0") == idSel(0));
    CHECK(parseWorkspaceSelector("4294967295") == idSel(4294967295LL));
    CHECK(parseWorkspaceSelector("web") == nameSel("web"));
    CHECK(parseWorkspaceSelector("name:web") == nameSel("web"));
    CHECK(!parseWorkspaceSelector("name:2")); // Hyprland forbids numeric workspace names
    CHECK(parseWorkspaceSelector("special:scratch") == nameSel("special:scratch"));
    CHECK(parseWorkspaceSelector("2b") == nameSel("2b"));
    CHECK(!parseWorkspaceSelector(""));
    CHECK(!parseWorkspaceSelector("name:"));
    CHECK(!parseWorkspaceSelector("99999999999999999999999"));
    CHECK(!parseWorkspaceSelector("-1"));         // IDs are unsigned; no workspace has a negative one
    CHECK(!parseWorkspaceSelector("4294967296")); // one past the 32-bit limit
}

// Surrounding whitespace is ignored, including after `name:`, so " 2" is ID 2 rather
// than a name no workspace can have.
static void test_parse_workspace_selector_trims() {
    CHECK(parseWorkspaceSelector(" 2") == idSel(2));
    CHECK(parseWorkspaceSelector("2\t ") == idSel(2));
    CHECK(parseWorkspaceSelector("  web ") == nameSel("web"));
    CHECK(parseWorkspaceSelector("name: web") == nameSel("web"));
    CHECK(parseWorkspaceSelector("my web") == nameSel("my web")); // inner spaces are kept
    CHECK(!parseWorkspaceSelector("   "));
    CHECK(!parseWorkspaceSelector("name:  "));
}

// A name selector matches either Hyprland's address or the label the user sees, so a
// workspace renamed to "web" is reachable as "web". Numbered workspaces' addresses are
// their ID as text, which no name selector can equal, so the two kinds never cross-match.
static void test_workspace_name_matches_display_name() {
    SSeamRule rule{.isSeamDirective = true, .seamOn = true, .workspaces = {nameSel("web")}};
    const SWorkspaceRef renamed{.id = 2, .address = "2", .displayName = "web"};
    CHECK(resolveWindowConfig("x", "", false, defaultDefaults(), {rule}, renamed).seamEnabled == true);

    SSeamRule byId{.isSeamDirective = true, .seamOn = true, .workspaces = {idSel(2)}};
    CHECK(resolveWindowConfig("x", "", false, defaultDefaults(), {byId}, renamed).seamEnabled == true);

    const SWorkspaceRef other{.id = 3, .address = "3", .displayName = "mail"};
    CHECK(resolveWindowConfig("x", "", false, defaultDefaults(), {rule}, other).seamEnabled == false);
}

// The string form accepts a single `workspace:<selector>` matcher and resets the
// matchers of a reused rule object.
static void test_parse_workspace_rule_line() {
    SSeamRule rule;
    CHECK(parseSeamRuleLine("rounding 0 0 0 0, workspace:2", rule));
    CHECK(!rule.isSeamDirective);
    CHECK(rule.workspaces == std::vector{idSel(2)});
    CHECK(rule.classPattern.empty() && rule.titlePattern.empty());

    CHECK(parseSeamRuleLine("seam 1, workspace:name:web", rule));
    CHECK(rule.workspaces == std::vector{nameSel("web")});

    // A reused rule object must not carry the previous matcher over.
    CHECK(parseSeamRuleLine("seam 0, class:^foot$", rule));
    CHECK(rule.workspaces.empty() && rule.classPattern == "^foot$");

    CHECK(!parseSeamRuleLine("seam 1, workspace:", rule));
    CHECK(!parseSeamRuleLine("seam 1, workspace:name:", rule));

    // The string form takes exactly one matcher, so anything after a comma is an error
    // instead of becoming part of a name that can never match.
    CHECK(!parseSeamRuleLine("seam 1, workspace:2, class:^kitty$", rule));
    CHECK(!parseSeamRuleLine("seam 1, workspace:1,2", rule));
    CHECK(parseSeamRuleLine("seam 1, workspace: 2", rule));
    CHECK(rule.workspaces == std::vector{idSel(2)});
}

// A workspace-only rule counts as having a matcher, so it must neither be rejected as
// matcher-less nor ignored by the resolver.
static void test_workspace_rule_counts_as_matcher() {
    SSeamRule rule;
    CHECK(parseSeamRuleLine("seam 1, workspace:3", rule));
    CHECK(resolveWindowConfig("x", "", false, defaultDefaults(), {rule}, SWorkspaceRef{3, "3"}).seamEnabled == true);
}

int main() {
    test_default_constructed_rule_has_safe_defaults();
    test_title_rule_matches_by_title_only();
    test_class_and_title_both_required_when_both_set();
    test_rule_without_any_pattern_never_matches();
    test_parse_title_rule_line();
    test_parse_trailing_garbage_fails();
    test_no_rules_uses_global_defaults();
    test_rounding_rule_matches_by_class();
    test_seam_rule_force_on();
    test_seam_rule_force_off_overrides_global_enabled();
    test_floating_window_never_gets_seam_regardless_of_rules();
    test_parse_rounding_rule_line();
    test_parse_seam_rule_line();
    test_parse_malformed_line_fails();
    test_rule_follows_title_changes();
    test_cached_pattern_keeps_matching();
    test_malformed_pattern_skipped_every_time();
    test_workspace_only_rule_matches_that_workspace();
    test_workspace_list_matches_any_entry();
    test_workspace_and_class_both_required();
    test_workspace_and_class_rules_resolve_by_order();
    test_workspace_rule_never_matches_window_without_workspace();
    test_workspace_name_selector();
    test_parse_workspace_selector();
    test_parse_workspace_selector_trims();
    test_workspace_name_matches_display_name();
    test_parse_workspace_rule_line();
    test_workspace_rule_counts_as_matcher();

    if (failures == 0) {
        std::printf("All seam-config tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d seam-config test(s) failed.\n", failures);
    return 1;
}
