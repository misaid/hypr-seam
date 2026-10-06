// tests/test_seamconfig.cpp
#include "../SeamConfig.hpp"
#include <cassert>
#include <cstdio>

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
    CHECK(!parseSeamRuleLine("seam 1, workspace:2", rule)); // unknown matcher
    CHECK(parseSeamRuleLine("seam 1 , class:^(foot)$ ", rule)); // surrounding whitespace is fine
    CHECK(rule.classPattern == "^(foot)$");
}

// Regression test for a latent UB hazard: bare `SSeamRule rule;` (no braces,
// as used by onSeamRule() in main.cpp before parseSeamRuleLine fills it) must
// deterministically default-initialize every scalar member via SSeamRule's
// default member initializers, not leave bool/double fields indeterminate.
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

    if (failures == 0) {
        std::printf("All seam-config tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d seam-config test(s) failed.\n", failures);
    return 1;
}
