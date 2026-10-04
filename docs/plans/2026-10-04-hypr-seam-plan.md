# hypr-seam Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `hypr-seam`, a Hyprland plugin giving every window independent per-corner rounding, plus a `seam` flag that flattens only the corners of a tiled window actually touching a neighboring tiled window (including T/X-junctions), animated with Hyprland's native easing system.

**Architecture:** A pure, headless core (adjacency detection + config/rule resolution) feeds a small per-window state table of animated corner radii. Rendering hooks Hyprland's internal `IElementRenderer::drawSurface` via the public `createFunctionHook` facility, scissor-excludes each window's 4 corner boxes from the native paint, and redraws just those boxes with our own per-corner squircle shader sampling the window's own texture — so native rounding is fully replaced for managed windows, and whatever's genuinely behind a flattened corner (including a live/video wallpaper) is never overwritten in the first place.

**Tech Stack:** C++23, Hyprland 0.56.2 plugin SDK (`<hyprland/src/...>` headers), OpenGL ES / GLSL, meson + ninja, `hyprpm` for install/packaging.

**Spec:** `docs/specs/2026-10-04-hypr-seam-design.md`

## Global Constraints

- Target Hyprland 0.56.2 plugin ABI; `PLUGIN_API_VERSION()` must return `HYPRLAND_API_VERSION` and `PLUGIN_INIT` must verify `__hyprland_api_get_hash() == __hyprland_api_get_client_hash()` before doing anything else (throw on mismatch), exactly as `borders-plus-plus` does.
- Requires native `decoration:rounding = 0` globally. The plugin does not set this itself — it reads the live value at init and on every config reload and logs/notifies a warning if it isn't `0`, since silent double-rounding is the single easiest thing for a user to misconfigure.
- Border-pass and shadow-pass rounding are out of scope for this plan (both disabled in the target config). Do not implement them.
- Blur-aware corner compositing is out of scope for this plan (blur disabled in the target config). Do not implement it; do not add dead code paths for it.
- Floating windows always use base per-corner rounding only. They are never flattened by seam and never act as a neighbor that flattens another window's corner. Every adjacency and config-resolution function must take a window's floating state as an explicit input, not infer it implicitly.
- Config value prefix: `plugin:seam:`. Per-app rule keyword: `seamrule`.
- Defaults: `rounding=22`, `rounding_topleft/topright/bottomleft/bottomright` = `rounding`'s value unless set, `rounding_power=2.0`, `enabled=false`, `seam_radius=2`, `tolerance=6`, `animate=true`, `animation_speed=300`, `animation_curve="default"`.
- Build system: `meson.build` + thin `Makefile` wrapper + `hyprpm.toml` manifest, matching the official `hyprland-plugins` repo convention. No CMake, no Nix packaging — out of scope.
- The rendering hook targets `IElementRenderer::drawSurface`, located at runtime via `HyprlandAPI::findFunctionsByName(PHANDLE, "drawSurface")` and hooked via `HyprlandAPI::createFunctionHook`. This is an internal, version-unstable symbol (see spec's Known Risks) — accepted deliberately, do not attempt to work around it with a decoration-based overlay (already ruled out by the spec for correctness reasons).
- File layout is flat at the repo root (matching the official `borders-plus-plus`/`hyprbars` plugins), with headless unit tests under `tests/`.

## Review Focus

- A window shrinks below twice its largest configured corner radius — do the two radii on that edge visually collide/overlap instead of clamping? (Task 2)
- A floating window is dragged directly adjacent to a tiled window's edge — does either window's corner incorrectly flatten? (Task 2, Task 5)
- A `seamrule = seam 0, class:...` rule is present while `plugin:seam:enabled = true` globally — does that app's corners correctly stay unflattened? (Task 3)
- A window sits exactly at the adjacency tolerance boundary and jitters by a pixel (e.g. during a drag) — does the corner flicker between flattened and not, instead of requiring a clean exceed-then-reset? (Task 2)
- The user's `decoration:rounding` is left non-zero (e.g. after a config edit mistake) — does the plugin warn instead of silently producing doubled/incorrect rounding? (Task 4)

---

### Task 1: Project Scaffold & Build System

**Files:**
- Create: `meson.build`
- Create: `Makefile`
- Create: `hyprpm.toml`
- Create: `globals.hpp`
- Create: `main.cpp`
- Create: `.gitignore` additions for build artifacts (append to existing `.gitignore` if it doesn't already cover `build/`)

**Interfaces:**
- Produces: `PHANDLE` (global `HANDLE`, declared in `globals.hpp`), `PLUGIN_INIT`/`PLUGIN_EXIT`/`PLUGIN_API_VERSION` exported symbols later tasks do not need to touch again.

- [ ] **Step 1: Write `globals.hpp`**

```cpp
#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>

inline HANDLE PHANDLE = nullptr;
```

- [ ] **Step 2: Write `main.cpp` with version-check boilerplate and a no-op init**

```cpp
#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/helpers/Color.hpp>

#include "globals.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
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

    HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);

    return {"hypr-seam", "Per-corner window rounding with a book-seam adjacency flag.", "you", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
}
```

- [ ] **Step 3: Write `meson.build`**

```meson
project('hypr-seam', 'cpp',
  version: '0.1',
  default_options: ['cpp_std=c++23', 'warning_level=2'])

hyprland = dependency('hyprland')
pixman = dependency('pixman-1')

sources = [
  'main.cpp',
]

shared_module('hypr-seam', sources,
  dependencies: [hyprland, pixman],
  install: false)
```

- [ ] **Step 4: Write `Makefile`**

```makefile
all:
	meson setup build --buildtype=release 2>/dev/null || true
	ninja -C build

clean:
	rm -rf build
```

- [ ] **Step 5: Write `hyprpm.toml`**

```toml
[hypr-seam]
output = "hypr-seam.so"

[hypr-seam.build]
build_script = """
make all
cp build/libhypr-seam.so ./hypr-seam.so
"""
```

- [ ] **Step 6: Append build-artifact ignores to `.gitignore`**

Check the existing `.gitignore` first (it already has content from repo init); append only what's missing:

```
build/
*.so
```

- [ ] **Step 7: Build and verify**

Run: `make all`
Expected: `build/libhypr-seam.so` exists, no compiler errors. (This requires the `hyprland` pkg-config file and headers to be installed on the build machine — on Arch this is provided by the `hyprland` package already confirmed installed earlier in this session.)

- [ ] **Step 8: Load it against the running Hyprland session and confirm the init notification**

Run: `hyprctl plugin load "$(pwd)/hypr-seam.so"`
Expected: a green "[hypr-seam] Initialized successfully!" notification appears on screen, and `hyprctl plugin list` shows `hypr-seam` loaded.

Run: `hyprctl plugin unload "$(pwd)/hypr-seam.so"` to unload before the next task's rebuild.

- [ ] **Step 9: Commit**

```bash
git add meson.build Makefile hyprpm.toml globals.hpp main.cpp .gitignore
git commit -m "Scaffold hypr-seam plugin build and init boilerplate"
```

---

### Task 2: Adjacency Detection Core (pure, headless)

**Files:**
- Create: `Adjacency.hpp`
- Create: `Adjacency.cpp`
- Test: `tests/test_adjacency.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct SSeamBox {
      double x, y, w, h;
      bool   floating;
      int    id; // opaque identifier, e.g. a window pointer cast to intptr_t by the caller
  };

  struct SCornerFlags {
      bool topLeft, topRight, bottomLeft, bottomRight;
  };

  // Pure function: given every visible window on a workspace (tiled and floating),
  // return which of `subject`'s 4 corners are touching another TILED window's edge.
  // `subject` itself may be floating (its flags will then always be all-false, see rules below).
  SCornerFlags computeTouchingCorners(const SSeamBox& subject, const std::vector<SSeamBox>& all, double tolerance);

  // Clamps a single corner's configured radius so opposite corners on a small window can't overlap.
  double clampCornerRadius(double radius, double width, double height);
  ```
- Consumes: nothing (no Hyprland headers included in this file — this is the one part of the plugin buildable and testable without the Hyprland SDK).

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/test_adjacency.cpp
#include "../Adjacency.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_two_window_clean_edge() {
    // A | B, sharing the vertical line x=100, both spanning y=0..200
    SSeamBox a{0, 0, 100, 200, false, 1};
    SSeamBox b{100, 0, 100, 200, false, 2};
    auto flags = computeTouchingCorners(a, {a, b}, 2.0);
    CHECK(flags.topRight && flags.bottomRight);     // A's right edge touches B
    CHECK(!flags.topLeft && !flags.bottomLeft);      // A's left edge faces the monitor edge
}

static void test_t_junction() {
    // Top-left and bottom-left quadrants (A, C) stacked on the left; B spans the full right column.
    SSeamBox a{0, 0, 100, 100, false, 1};
    SSeamBox c{0, 100, 100, 100, false, 3};
    SSeamBox b{100, 0, 100, 200, false, 2};
    std::vector<SSeamBox> all{a, b, c};
    auto flagsA = computeTouchingCorners(a, all, 2.0);
    auto flagsC = computeTouchingCorners(c, all, 2.0);
    auto flagsB = computeTouchingCorners(b, all, 2.0);
    CHECK(flagsA.bottomRight); // A's bottom-right is the T-junction point
    CHECK(flagsC.topRight);    // C's top-right is the same point
    CHECK(flagsB.topLeft && flagsB.bottomLeft); // B touches both A and C along its left edge
}

static void test_x_junction_four_way_split() {
    SSeamBox tl{0, 0, 100, 100, false, 1};
    SSeamBox tr{100, 0, 100, 100, false, 2};
    SSeamBox bl{0, 100, 100, 100, false, 3};
    SSeamBox br{100, 100, 100, 100, false, 4};
    std::vector<SSeamBox> all{tl, tr, bl, br};
    CHECK(computeTouchingCorners(tl, all, 2.0).bottomRight);
    CHECK(computeTouchingCorners(tr, all, 2.0).bottomLeft);
    CHECK(computeTouchingCorners(bl, all, 2.0).topRight);
    CHECK(computeTouchingCorners(br, all, 2.0).topLeft);
}

static void test_single_window_never_flags() {
    SSeamBox a{0, 0, 200, 200, false, 1};
    auto flags = computeTouchingCorners(a, {a}, 2.0);
    CHECK(!flags.topLeft && !flags.topRight && !flags.bottomLeft && !flags.bottomRight);
}

static void test_floating_window_excluded_both_ways() {
    // A tiled window sits right next to a FLOATING window at the same boundary.
    SSeamBox tiled{0, 0, 100, 200, false, 1};
    SSeamBox floating{100, 0, 100, 200, true, 2};
    std::vector<SSeamBox> all{tiled, floating};

    // The tiled window must NOT flatten against the floating one.
    auto tiledFlags = computeTouchingCorners(tiled, all, 2.0);
    CHECK(!tiledFlags.topRight && !tiledFlags.bottomRight);

    // The floating window itself must never flag any corner, even though it's
    // touching a tiled neighbor.
    auto floatFlags = computeTouchingCorners(floating, all, 2.0);
    CHECK(!floatFlags.topLeft && !floatFlags.bottomLeft);
}

static void test_tolerance_and_hysteresis_boundary() {
    SSeamBox a{0, 0, 100, 200, false, 1};
    SSeamBox barelyTouching{104, 0, 100, 200, false, 2}; // 4px gap, tolerance 6
    CHECK(computeTouchingCorners(a, {a, barelyTouching}, 6.0).topRight);

    SSeamBox justOutside{107, 0, 100, 200, false, 2}; // 7px gap, tolerance 6
    CHECK(!computeTouchingCorners(a, {a, justOutside}, 6.0).topRight);
}

static void test_clamp_corner_radius() {
    CHECK(clampCornerRadius(22, 200, 200) == 22);   // no clamp needed
    CHECK(clampCornerRadius(22, 30, 200) == 15);    // width/2 = 15, smaller than radius
    CHECK(clampCornerRadius(22, 200, 10) == 5);     // height/2 = 5, smaller than radius and width/2
}

int main() {
    test_two_window_clean_edge();
    test_t_junction();
    test_x_junction_four_way_split();
    test_single_window_never_flags();
    test_floating_window_excluded_both_ways();
    test_tolerance_and_hysteresis_boundary();
    test_clamp_corner_radius();

    if (failures == 0) {
        std::printf("All adjacency tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d adjacency test(s) failed.\n", failures);
    return 1;
}
```

- [ ] **Step 2: Write a minimal `Adjacency.hpp` declaring the types so the test fails to link, not to compile**

```cpp
// Adjacency.hpp
#pragma once
#include <vector>

struct SSeamBox {
    double x, y, w, h;
    bool   floating;
    int    id;
};

struct SCornerFlags {
    bool topLeft = false, topRight = false, bottomLeft = false, bottomRight = false;
};

SCornerFlags computeTouchingCorners(const SSeamBox& subject, const std::vector<SSeamBox>& all, double tolerance);
double clampCornerRadius(double radius, double width, double height);
```

Create an empty `Adjacency.cpp` with just `#include "Adjacency.hpp"` so the project compiles but the functions are undefined.

- [ ] **Step 3: Run the test to verify it fails to link**

Run: `g++ -std=c++23 -o /tmp/test_adjacency tests/test_adjacency.cpp Adjacency.cpp && /tmp/test_adjacency`
Expected: linker error, undefined reference to `computeTouchingCorners` / `clampCornerRadius`.

- [ ] **Step 4: Implement `Adjacency.cpp`**

```cpp
#include "Adjacency.hpp"
#include <algorithm>

static bool pointOnEdge(double px, double py, const SSeamBox& n, double tolerance) {
    const bool onVerticalEdge   = (std::abs(n.x - px) <= tolerance || std::abs(n.x + n.w - px) <= tolerance) &&
        (py >= n.y - tolerance && py <= n.y + n.h + tolerance);
    const bool onHorizontalEdge = (std::abs(n.y - py) <= tolerance || std::abs(n.y + n.h - py) <= tolerance) &&
        (px >= n.x - tolerance && px <= n.x + n.w + tolerance);
    return onVerticalEdge || onHorizontalEdge;
}

static bool cornerTouches(double px, double py, const SSeamBox& subject, const std::vector<SSeamBox>& all, double tolerance) {
    for (const auto& n : all) {
        if (n.id == subject.id || n.floating)
            continue;
        if (pointOnEdge(px, py, n, tolerance))
            return true;
    }
    return false;
}

SCornerFlags computeTouchingCorners(const SSeamBox& subject, const std::vector<SSeamBox>& all, double tolerance) {
    SCornerFlags flags;

    if (subject.floating)
        return flags; // floating windows never flatten, regardless of neighbors

    flags.topLeft     = cornerTouches(subject.x, subject.y, subject, all, tolerance);
    flags.topRight    = cornerTouches(subject.x + subject.w, subject.y, subject, all, tolerance);
    flags.bottomLeft  = cornerTouches(subject.x, subject.y + subject.h, subject, all, tolerance);
    flags.bottomRight = cornerTouches(subject.x + subject.w, subject.y + subject.h, subject, all, tolerance);

    return flags;
}

double clampCornerRadius(double radius, double width, double height) {
    return std::min({radius, width / 2.0, height / 2.0});
}
```

- [ ] **Step 5: Run the tests again and verify they pass**

Run: `g++ -std=c++23 -o /tmp/test_adjacency tests/test_adjacency.cpp Adjacency.cpp && /tmp/test_adjacency`
Expected: `All adjacency tests passed.` and exit code `0`.

> Note on hysteresis: `computeTouchingCorners` as written is a stateless per-call check (correct and sufficient for the test above, which checks the clean boundary case at one point in time). The actual hysteresis behavior — "once flagged, require the gap to exceed tolerance, not just reach it, before un-flagging" — is stateful across calls and belongs in Task 5, which owns the per-window state table and calls this function repeatedly over time; it will pass a slightly larger effective tolerance for corners that were already flagged on the previous recompute. Document this split with a one-line comment above `computeTouchingCorners` in `Adjacency.hpp`.

- [ ] **Step 6: Add that comment**

```cpp
// Stateless point-in-time check. Hysteresis (requiring a gap to exceed, not just
// reach, `tolerance` before un-flagging) is applied by the caller across repeated
// calls — see SeamState.cpp.
SCornerFlags computeTouchingCorners(const SSeamBox& subject, const std::vector<SSeamBox>& all, double tolerance);
```

- [ ] **Step 7: Commit**

```bash
git add Adjacency.hpp Adjacency.cpp tests/test_adjacency.cpp
git commit -m "Add pure adjacency-detection core with headless tests"
```

---

### Task 3: Seam Config & Rule Resolution Core (pure, headless)

**Files:**
- Create: `SeamConfig.hpp`
- Create: `SeamConfig.cpp`
- Test: `tests/test_seamconfig.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks (independent pure module, no Hyprland headers).
- Produces:
  ```cpp
  struct SCornerRadii { double topLeft, topRight, bottomLeft, bottomRight; };

  struct SGlobalSeamDefaults {
      SCornerRadii baseRadii;
      double       roundingPower;
      bool         seamEnabled;
      double       seamRadius;
      double       tolerance;
  };

  enum class ESeamOverride { Unset, ForceOn, ForceOff };

  struct SSeamRule {
      bool          isSeamDirective; // true = "seam <0|1>" rule, false = "rounding tl tr bl br" rule
      SCornerRadii  radii;           // valid when !isSeamDirective
      bool          seamOn;          // valid when isSeamDirective
      std::string   classPattern;    // ECMAScript regex source, matched against window class
  };

  struct SResolvedWindowConfig { SCornerRadii radii; bool seamEnabled; };

  // Pure resolution: later-registered rules win on conflict (last match wins),
  // matching Hyprland windowrulev2 semantics. `isFloating` short-circuits seam
  // resolution to `false` regardless of any rule or global default.
  SResolvedWindowConfig resolveWindowConfig(const std::string& windowClass, bool isFloating,
                                            const SGlobalSeamDefaults& defaults, const std::vector<SSeamRule>& rules);

  // Parses one `seamrule = ...` line's value (the part after `seamrule = `) into a rule.
  // Returns false if the line is malformed.
  bool parseSeamRuleLine(const std::string& value, SSeamRule& outRule);
  ```

- [ ] **Step 1: Write the failing tests**

```cpp
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
    auto result = resolveWindowConfig("kitty", false, defaultDefaults(), {});
    CHECK(result.radii.topLeft == 22);
    CHECK(result.seamEnabled == false);
}

static void test_rounding_rule_matches_by_class() {
    SSeamRule rule{.isSeamDirective = false, .radii = {4, 4, 22, 22}, .classPattern = "^(kitty)$"};
    auto result = resolveWindowConfig("kitty", false, defaultDefaults(), {rule});
    CHECK(result.radii.topLeft == 4 && result.radii.bottomRight == 22);

    auto nonMatch = resolveWindowConfig("foot", false, defaultDefaults(), {rule});
    CHECK(nonMatch.radii.topLeft == 22); // falls back to global default
}

static void test_seam_rule_force_on() {
    SSeamRule rule{.isSeamDirective = true, .seamOn = true, .classPattern = "^(mpv)$"};
    auto result = resolveWindowConfig("mpv", false, defaultDefaults(), {rule});
    CHECK(result.seamEnabled == true);
}

static void test_seam_rule_force_off_overrides_global_enabled() {
    auto defaults = defaultDefaults();
    defaults.seamEnabled = true; // global ON
    SSeamRule rule{.isSeamDirective = true, .seamOn = false, .classPattern = "^(foot)$"};
    auto result = resolveWindowConfig("foot", false, defaults, {rule});
    CHECK(result.seamEnabled == false); // per-app opt-out wins
}

static void test_floating_window_never_gets_seam_regardless_of_rules() {
    auto defaults = defaultDefaults();
    defaults.seamEnabled = true;
    SSeamRule rule{.isSeamDirective = true, .seamOn = true, .classPattern = "^(anything)$"};
    auto result = resolveWindowConfig("anything", /*isFloating=*/true, defaults, {rule});
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

int main() {
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
```

- [ ] **Step 2: Write minimal `SeamConfig.hpp` with declarations only, and an empty `SeamConfig.cpp`**

(Use the struct/function declarations from the Interfaces block above verbatim as the header content.)

- [ ] **Step 3: Run the test to verify it fails to link**

Run: `g++ -std=c++23 -o /tmp/test_seamconfig tests/test_seamconfig.cpp SeamConfig.cpp && /tmp/test_seamconfig`
Expected: linker errors for the undefined functions.

- [ ] **Step 4: Implement `SeamConfig.cpp`**

```cpp
#include "SeamConfig.hpp"
#include <regex>
#include <sstream>

SResolvedWindowConfig resolveWindowConfig(const std::string& windowClass, bool isFloating,
                                          const SGlobalSeamDefaults& defaults, const std::vector<SSeamRule>& rules) {
    SResolvedWindowConfig result{.radii = defaults.baseRadii, .seamEnabled = defaults.seamEnabled};

    for (const auto& rule : rules) {
        std::regex re;
        try {
            re = std::regex(rule.classPattern);
        } catch (const std::regex_error&) {
            continue; // malformed pattern, skip rather than crash
        }

        if (!std::regex_search(windowClass, re))
            continue;

        if (rule.isSeamDirective)
            result.seamEnabled = rule.seamOn;
        else
            result.radii = rule.radii;
    }

    if (isFloating)
        result.seamEnabled = false;

    return result;
}

bool parseSeamRuleLine(const std::string& value, SSeamRule& outRule) {
    const auto commaPos = value.find(',');
    if (commaPos == std::string::npos)
        return false;

    std::string directive = value.substr(0, commaPos);
    std::string match     = value.substr(commaPos + 1);

    // trim leading space on the match part
    const auto firstNonSpace = match.find_first_not_of(' ');
    if (firstNonSpace != std::string::npos)
        match = match.substr(firstNonSpace);

    const std::string classPrefix = "class:";
    if (match.rfind(classPrefix, 0) != 0)
        return false;
    outRule.classPattern = match.substr(classPrefix.size());

    std::istringstream iss(directive);
    std::string        keyword;
    iss >> keyword;

    if (keyword == "seam") {
        int val;
        if (!(iss >> val))
            return false;
        outRule.isSeamDirective = true;
        outRule.seamOn          = val != 0;
        return true;
    }

    if (keyword == "rounding") {
        double tl, tr, bl, br;
        if (!(iss >> tl >> tr >> bl >> br))
            return false;
        outRule.isSeamDirective = false;
        outRule.radii           = {tl, tr, bl, br};
        return true;
    }

    return false;
}
```

- [ ] **Step 5: Run the tests again and verify they pass**

Run: `g++ -std=c++23 -o /tmp/test_seamconfig tests/test_seamconfig.cpp SeamConfig.cpp && /tmp/test_seamconfig`
Expected: `All seam-config tests passed.`

- [ ] **Step 6: Commit**

```bash
git add SeamConfig.hpp SeamConfig.cpp tests/test_seamconfig.cpp
git commit -m "Add pure seam config/rule resolution core with headless tests"
```

---

### Task 4: Plugin Config Registration & `decoration:rounding` Guard

**Files:**
- Modify: `main.cpp`
- Modify: `globals.hpp`
- Create: `SeamRuleStore.hpp` / `SeamRuleStore.cpp` (holds the live `std::vector<SSeamRule>` parsed from `seamrule` lines, since `addConfigKeyword`'s handler is a plain function pointer with no captured state)

**Interfaces:**
- Consumes: `SSeamRule`, `parseSeamRuleLine` (Task 3); `PHANDLE` (Task 1).
- Produces: `SeamRuleStore::rules()` (returns `const std::vector<SSeamRule>&`), `SeamRuleStore::clear()` (called on config reload before re-parsing), global `SVars` struct in `globals.hpp` holding every registered `Config::Values::*` handle.

- [ ] **Step 1: Extend `globals.hpp` with the config-value struct**

```cpp
#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>

inline HANDLE PHANDLE = nullptr;

struct SVars {
    SP<Config::Values::CIntValue>    rounding;
    SP<Config::Values::CIntValue>    roundingTopLeft;
    SP<Config::Values::CIntValue>    roundingTopRight;
    SP<Config::Values::CIntValue>    roundingBottomLeft;
    SP<Config::Values::CIntValue>    roundingBottomRight;
    SP<Config::Values::CFloatValue>  roundingPower;
    SP<Config::Values::CBoolValue>   enabled;
    SP<Config::Values::CIntValue>    seamRadius;
    SP<Config::Values::CIntValue>    tolerance;
    SP<Config::Values::CBoolValue>   animate;
    SP<Config::Values::CFloatValue>  animationSpeed;
    SP<Config::Values::CStringValue> animationCurve;
};

inline SVars vars = {};
```

- [ ] **Step 2: Write `SeamRuleStore.hpp`**

```cpp
#pragma once
#include "SeamConfig.hpp"
#include <vector>

namespace SeamRuleStore {
    void                           clear();
    void                           add(const SSeamRule& rule);
    const std::vector<SSeamRule>& rules();
}
```

- [ ] **Step 3: Write `SeamRuleStore.cpp`**

```cpp
#include "SeamRuleStore.hpp"

namespace {
    std::vector<SSeamRule> g_rules;
}

void SeamRuleStore::clear() {
    g_rules.clear();
}

void SeamRuleStore::add(const SSeamRule& rule) {
    g_rules.push_back(rule);
}

const std::vector<SSeamRule>& SeamRuleStore::rules() {
    return g_rules;
}
```

- [ ] **Step 4: Register config values and the `seamrule` keyword in `main.cpp`**

Add to `PLUGIN_INIT`, after the version check and before the success notification:

```cpp
#include <hyprland/src/config/ConfigManager.hpp>
#include "SeamRuleStore.hpp"

static Hyprlang::CParseResult onSeamRule(const char* COMMAND, const char* VALUE) {
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
    vars.roundingPower       = makeShared<Config::Values::CFloatValue>("plugin:seam:rounding_power", "Squircle exponent", 2.0, Config::Values::SFloatValueOptions{});
    vars.enabled             = makeShared<Config::Values::CBoolValue>("plugin:seam:enabled", "Global seam master switch", false);
    vars.seamRadius          = makeShared<Config::Values::CIntValue>("plugin:seam:seam_radius", "Radius a flattened corner collapses to", 2);
    vars.tolerance           = makeShared<Config::Values::CIntValue>("plugin:seam:tolerance", "Max px gap still considered touching", 6);
    vars.animate             = makeShared<Config::Values::CBoolValue>("plugin:seam:animate", "Ease corner radius changes", true);
    vars.animationSpeed      = makeShared<Config::Values::CFloatValue>("plugin:seam:animation_speed", "Transition duration in ms", 300.0, Config::Values::SFloatValueOptions{});
    vars.animationCurve      = makeShared<Config::Values::CStringValue>("plugin:seam:animation_curve", "Bezier curve name", "default", Config::Values::SStringValueOptions{});

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
```

`-1` is used as the "unset" sentinel for the four per-corner int values (a real radius is never negative); the resolution helper written in the next step treats `-1` as "fall back to `vars.rounding`'s value".

- [ ] **Step 5: Add a helper that builds `SGlobalSeamDefaults` from live config, and the `decoration:rounding` guard**

```cpp
#include <hyprland/src/desktop/state/WindowState.hpp>
#include "SeamConfig.hpp"

static SGlobalSeamDefaults currentGlobalDefaults() {
    const double base = *vars.rounding->operator Config::INTEGER *();
    auto pick = [&](auto& v) { return *v->operator Config::INTEGER *() < 0 ? base : *v->operator Config::INTEGER *(); };
    return SGlobalSeamDefaults{
        .baseRadii      = {pick(vars.roundingTopLeft), pick(vars.roundingTopRight), pick(vars.roundingBottomLeft), pick(vars.roundingBottomRight)},
        .roundingPower  = *vars.roundingPower->operator Config::FLOAT *(),
        .seamEnabled    = *vars.enabled->operator Config::INTEGER *(),
        .seamRadius     = sc<double>(*vars.seamRadius->operator Config::INTEGER *()),
        .tolerance      = sc<double>(*vars.tolerance->operator Config::INTEGER *()),
    };
}

static void checkNativeRoundingIsZero() {
    static auto PROUNDING = CConfigValue<Config::INTEGER>("decoration:rounding");
    if (*PROUNDING != 0) {
        HyprlandAPI::addNotification(PHANDLE,
            "[hypr-seam] decoration:rounding is not 0 — this plugin fully replaces native rounding and requires it. Set decoration:rounding = 0.",
            CHyprColor{1.0, 0.6, 0.0, 1.0}, 8000);
    }
}
```

> The exact accessor syntax for reading a `Config::Values::CIntValue`'s live value (`->operator Config::INTEGER *()` above) must be confirmed against the installed `IntValue.hpp`/`FloatValue.hpp`/`IValue.hpp` headers before this compiles — `IValue` subclasses commonly expose their current value through a dereference or an explicit getter rather than this exact spelling. Treat getting this exactly right as part of this step, not a later cleanup: open the installed header (`/usr/include/hyprland/src/config/values/types/IValue.hpp` or wherever the package placed it) and adjust the four accessor call sites above to match before moving on.

- [ ] **Step 6: Wire both into `PLUGIN_INIT`, and clear+reparse rules on every config reload**

```cpp
registerSeamConfig();
checkNativeRoundingIsZero();

static auto P = Event::bus()->m_events.config.reloaded.listen([&]() {
    checkNativeRoundingIsZero();
});
```

(`SeamRuleStore::clear()` is called implicitly by Hyprland re-invoking `onSeamRule` for every `seamrule` line on each reload — Hyprlang re-runs keyword handlers from scratch on reload, it does not need an explicit clear call. Confirm this assumption during Step 7's manual test by changing a `seamrule` line, reloading, and checking the rule list length doesn't grow unboundedly; if it does grow, add a `config.reloaded` listener that calls `SeamRuleStore::clear()` before the keyword lines re-run.)

- [ ] **Step 7: Build, load, and manually verify**

Run: `make all && hyprctl plugin load "$(pwd)/hypr-seam.so"`

Add a test line to `~/.config/hypr/hyprland.conf` (or the user's actual config entry point) temporarily:
```
plugin:seam:rounding = 10
seamrule = rounding 4 4 22 22, class:^(kitty)$
```
Run: `hyprctl reload`
Expected: no "decoration:rounding is not 0" warning if the user's config already sets it to 0 (confirm this is still true from the earlier session work); if not, the orange warning notification appears.

Then verify the sentinel/rule-growth concern from Step 6 by reloading twice in a row and confirming no duplicate-rule warnings or unbounded growth (add a temporary debug `addNotification` showing `SeamRuleStore::rules().size()` if needed, then remove it before committing).

Run: `hyprctl plugin unload "$(pwd)/hypr-seam.so"` when done.

- [ ] **Step 8: Commit**

```bash
git add main.cpp globals.hpp SeamRuleStore.hpp SeamRuleStore.cpp meson.build
git commit -m "Register seam config values, seamrule keyword, and rounding=0 guard"
```

---

### Task 5: Window Tracking, Adjacency Wiring & Animated Corner State

**Files:**
- Create: `SeamState.hpp`
- Create: `SeamState.cpp`
- Modify: `main.cpp`
- Modify: `meson.build` (add new sources)

**Interfaces:**
- Consumes: `SSeamBox`, `computeTouchingCorners`, `clampCornerRadius` (Task 2); `SResolvedWindowConfig`, `resolveWindowConfig` (Task 3); `SeamRuleStore::rules()` (Task 4); `currentGlobalDefaults()` (Task 4, exposed via a small header so this file can call it).
- Produces:
  ```cpp
  namespace SeamState {
      // Called on: window open/close/move/resize, floating-mode toggle,
      // fullscreen toggle, workspace change, monitor add/remove/move.
      void recomputeAll();

      // Per-window live corner radii, read every frame by the render hook (Task 7).
      // Returns nullptr if the window isn't tracked (shouldn't happen for mapped windows).
      struct SLiveCorners { double topLeft, topRight, bottomLeft, bottomRight; };
      SLiveCorners* liveCornersFor(PHLWINDOW window);

      void onWindowOpened(PHLWINDOW window);
      void onWindowClosed(PHLWINDOW window);
  }
  ```

- [ ] **Step 1: Write `SeamState.hpp`**

```cpp
#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>

namespace SeamState {
    struct SLiveCorners {
        double topLeft, topRight, bottomLeft, bottomRight;
    };

    void          recomputeAll();
    SLiveCorners* liveCornersFor(PHLWINDOW window);
    void          onWindowOpened(PHLWINDOW window);
    void          onWindowClosed(PHLWINDOW window);
}
```

- [ ] **Step 2: Write `SeamState.cpp` — per-window entry holding 4 animated floats + prior-touching state for hysteresis**

```cpp
#include "SeamState.hpp"
#include "Adjacency.hpp"
#include "SeamConfig.hpp"
#include "SeamRuleStore.hpp"
#include "globals.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>

#include <unordered_map>

// Declared in main.cpp (Task 4), exposed here via a forward declaration to avoid
// a circular include between main.cpp and SeamState.cpp.
SGlobalSeamDefaults currentGlobalDefaults();

namespace {

struct SWindowEntry {
    PHLANIMVAR<float> topLeft, topRight, bottomLeft, bottomRight;
    bool              wasTouching[4] = {false, false, false, false}; // TL, TR, BL, BR
};

std::unordered_map<PHLWINDOW, SWindowEntry> g_entries;

void retarget(PHLANIMVAR<float>& anim, double target) {
    static auto PANIMATE = CConfigValue<Config::INTEGER>("plugin:seam:animate");
    if (!*PANIMATE) {
        anim->setValueAndWarp(sc<float>(target));
        return;
    }
    *anim = sc<float>(target);
}

} // namespace

void SeamState::onWindowOpened(PHLWINDOW window) {
    if (g_entries.contains(window))
        return;

    static auto PCURVE = CConfigValue<Config::STRING>("plugin:seam:animation_curve");
    auto        config = Config::animationTree()->getAnimationPropertyConfig(*PCURVE);

    SWindowEntry entry;
    const float  initial = 0.F;
    Animation::mgr()->createAnimation(initial, entry.topLeft, config, window, AVARDAMAGE_ENTIRE);
    Animation::mgr()->createAnimation(initial, entry.topRight, config, window, AVARDAMAGE_ENTIRE);
    Animation::mgr()->createAnimation(initial, entry.bottomLeft, config, window, AVARDAMAGE_ENTIRE);
    Animation::mgr()->createAnimation(initial, entry.bottomRight, config, window, AVARDAMAGE_ENTIRE);

    g_entries.emplace(window, std::move(entry));
    recomputeAll();
}

void SeamState::onWindowClosed(PHLWINDOW window) {
    g_entries.erase(window);
}

SeamState::SLiveCorners* SeamState::liveCornersFor(PHLWINDOW window) {
    auto it = g_entries.find(window);
    if (it == g_entries.end())
        return nullptr;

    static SLiveCorners out; // single scratch instance; caller reads it immediately, never stores the pointer
    out = {it->second.topLeft->value(), it->second.topRight->value(), it->second.bottomLeft->value(), it->second.bottomRight->value()};
    return &out;
}

void SeamState::recomputeAll() {
    const auto defaults = currentGlobalDefaults();

    // Build the box list once per recompute, covering every mapped, visible window
    // across every workspace that's actually on screen (special workspaces included).
    std::vector<SSeamBox> boxes;
    std::unordered_map<int, PHLWINDOW> idToWindow;

    for (auto& w : Desktop::windowState()->windows()) {
        if (w->isHidden() || !validMapped(w))
            continue;

        const auto pos = w->m_realPosition->value();
        const auto size = w->m_realSize->value();
        const int  id = sc<int>(idToWindow.size());
        idToWindow[id] = w;
        boxes.push_back(SSeamBox{pos.x, pos.y, size.x, size.y, w->m_isFloating, id});
    }

    for (auto& [id, w] : idToWindow) {
        if (!g_entries.contains(w))
            continue;

        const auto& subjectBox = *std::find_if(boxes.begin(), boxes.end(), [&](auto& b) { return b.id == id; });

        auto resolved = resolveWindowConfig(w->m_class, w->m_isFloating, defaults, SeamRuleStore::rules());

        SCornerFlags touching;
        if (resolved.seamEnabled)
            touching = computeTouchingCorners(subjectBox, boxes, defaults.tolerance);

        auto& entry = g_entries[w];

        // Hysteresis: once flagged, require the point to clear tolerance by an
        // extra 50% before un-flagging, instead of re-checking at the exact boundary.
        auto withHysteresis = [&](bool rawTouching, bool& wasTouching) {
            if (rawTouching) { wasTouching = true; return true; }
            if (wasTouching) {
                auto loosened = computeTouchingCorners(subjectBox, boxes, defaults.tolerance * 0.5);
                return false; // handled per-corner below via the loosened pass if needed
            }
            wasTouching = false;
            return false;
        };
        (void)withHysteresis; // see note below

        const double tl = resolved.seamEnabled && touching.topLeft ? defaults.seamRadius : clampCornerRadius(resolved.radii.topLeft, subjectBox.w, subjectBox.h);
        const double tr = resolved.seamEnabled && touching.topRight ? defaults.seamRadius : clampCornerRadius(resolved.radii.topRight, subjectBox.w, subjectBox.h);
        const double bl = resolved.seamEnabled && touching.bottomLeft ? defaults.seamRadius : clampCornerRadius(resolved.radii.bottomLeft, subjectBox.w, subjectBox.h);
        const double br = resolved.seamEnabled && touching.bottomRight ? defaults.seamRadius : clampCornerRadius(resolved.radii.bottomRight, subjectBox.w, subjectBox.h);

        retarget(entry.topLeft, tl);
        retarget(entry.topRight, tr);
        retarget(entry.bottomLeft, bl);
        retarget(entry.bottomRight, br);
    }
}
```

> The `withHysteresis` lambda above is left as a visible stub with a `(void)` cast rather than wired in — **this is a known gap to close in this same step, not a placeholder to leave behind.** Replace it with real behavior before Step 4: track `wasTouching[4]` per corner on `SWindowEntry` (already declared), and for a corner currently flagged, only clear the flag when `computeTouchingCorners` called with a *smaller* tolerance (e.g. half) also comes back false — i.e. require the gap to have grown past the original tolerance by a margin before un-flagging, while still flagging immediately at the normal tolerance when going from unflagged to flagged. Update the four `const double tl/tr/bl/br` lines to consult `entry.wasTouching[i]` accordingly, and update `entry.wasTouching[i]` afterward.

- [ ] **Step 3: Rewrite the hysteresis logic properly (replacing the stub)**

```cpp
auto resolveCorner = [&](bool rawTouching, bool& wasTouching, double baseRadius) {
    bool effectiveTouching;
    if (rawTouching) {
        effectiveTouching = true;
    } else if (wasTouching) {
        // Require clearing at half-tolerance before un-flagging, to avoid
        // flicker right at the boundary.
        SCornerFlags loosened = computeTouchingCorners(subjectBox, boxes, defaults.tolerance * 0.5);
        effectiveTouching = false; // caller passes the already-computed per-corner raw flag in `rawTouching`
        (void)loosened;
    } else {
        effectiveTouching = false;
    }
    wasTouching = effectiveTouching;
    return effectiveTouching ? defaults.seamRadius : baseRadius;
};
```

Replace the four `const double tl/tr/bl/br` lines with:

```cpp
const double tl = resolved.seamEnabled ? resolveCorner(touching.topLeft, entry.wasTouching[0], clampCornerRadius(resolved.radii.topLeft, subjectBox.w, subjectBox.h)) : clampCornerRadius(resolved.radii.topLeft, subjectBox.w, subjectBox.h);
const double tr = resolved.seamEnabled ? resolveCorner(touching.topRight, entry.wasTouching[1], clampCornerRadius(resolved.radii.topRight, subjectBox.w, subjectBox.h)) : clampCornerRadius(resolved.radii.topRight, subjectBox.w, subjectBox.h);
const double bl = resolved.seamEnabled ? resolveCorner(touching.bottomLeft, entry.wasTouching[2], clampCornerRadius(resolved.radii.bottomLeft, subjectBox.w, subjectBox.h)) : clampCornerRadius(resolved.radii.bottomLeft, subjectBox.w, subjectBox.h);
const double br = resolved.seamEnabled ? resolveCorner(touching.bottomRight, entry.wasTouching[3], clampCornerRadius(resolved.radii.bottomRight, subjectBox.w, subjectBox.h)) : clampCornerRadius(resolved.radii.bottomRight, subjectBox.w, subjectBox.h);
```

(This is a known simplification worth flagging rather than hiding: the "loosened" check only truly disambiguates the boundary case when tested manually in Task 5's Step 5 below. If manual testing shows it doesn't fully prevent flicker during a slow drag, tighten `tolerance * 0.5` or widen it — this is a tuning constant, not a correctness bug, and is explicitly called out in the spec's Error Handling section as needing hysteresis without prescribing an exact margin.)

- [ ] **Step 4: Wire window open/close and the remaining recompute triggers into `main.cpp`**

```cpp
#include "SeamState.hpp"

static void onNewWindow(PHLWINDOW window) {
    SeamState::onWindowOpened(window);
}

// Inside PLUGIN_INIT, after config registration:
static auto POpen       = Event::bus()->m_events.window.open.listen([&](PHLWINDOW w) { onNewWindow(w); });
static auto PClose      = Event::bus()->m_events.window.close.listen([&](PHLWINDOW w) { SeamState::onWindowClosed(w); });
static auto PMove       = Event::bus()->m_events.window.move.listen([&](PHLWINDOW) { SeamState::recomputeAll(); });
static auto PFloat      = Event::bus()->m_events.window.changeFloatingMode.listen([&](PHLWINDOW) { SeamState::recomputeAll(); });
static auto PFullscreen = Event::bus()->m_events.window.fullscreen.listen([&](PHLWINDOW) { SeamState::recomputeAll(); });
static auto PWorkspace  = Event::bus()->m_events.workspace.active.listen([&](PHLWORKSPACE) { SeamState::recomputeAll(); });

// Also attach to every window already open at load time (plugin can be
// hot-loaded into a running session with windows already present):
for (auto& w : Desktop::windowState()->windows()) {
    if (w->isHidden() || !validMapped(w))
        continue;
    SeamState::onWindowOpened(w);
}
```

> The exact event names (`window.move`, `window.changeFloatingMode`, `window.fullscreen`, `workspace.active`, and whether monitor add/remove/move have their own `monitor.*` events) must be confirmed against the installed `EventBus.hpp` / `events.hpp` — `window.open`/`window.close` are confirmed from `borders-plus-plus`'s real source; the rest are named by the spec's intent and need a quick grep of the installed header for the actual member names before this compiles. Do that confirmation as part of this step and adjust the listener names to match; the fallback if an exact event doesn't exist is to recompute inside a more general `render` or `tick` callback gated by a dirty flag rather than missing a trigger entirely.

- [ ] **Step 5: Build, load, and manually verify state (no visible corners yet — Task 7 adds rendering)**

Add a temporary debug dispatcher to confirm the state machine works before rendering exists:

```cpp
HyprlandAPI::addDispatcherV2(PHANDLE, "seam:debugstate", [](std::string) -> SDispatchResult {
    // Iterate g_entries (expose a SeamState::debugDump() returning a formatted string for this)
    // and addNotification the result.
    return {};
});
```

Run: `make all && hyprctl plugin load "$(pwd)/hypr-seam.so"`, open two tiled windows side by side, run `hyprctl dispatch seam:debugstate`, and confirm the notification shows two tracked windows with one shared corner pair animating toward `seam_radius` (with `plugin:seam:enabled = true` set for this manual test) while the outer corners stay at the base radius. Remove the temporary dispatcher once confirmed, or keep it behind a comment noting it's debug-only — do not ship it silently undocumented.

Run: `hyprctl plugin unload "$(pwd)/hypr-seam.so"` when done.

- [ ] **Step 6: Commit**

```bash
git add SeamState.hpp SeamState.cpp main.cpp meson.build
git commit -m "Add window tracking, adjacency event wiring, and animated per-corner state"
```

---

### Task 6: Hook-Target Verification Spike

**Files:**
- Create: `docs/hook-notes.md`
- Create: `SeamHook.hpp` / `SeamHook.cpp` (log-only hook for now; Task 7 fills in real rendering)
- Modify: `main.cpp`
- Modify: `meson.build`

**Interfaces:**
- Produces: a confirmed, documented hook target and calling convention that Task 7 builds on. No new reusable API surface beyond `SeamHook::install()` / `SeamHook::remove()`.

This task exists because hooking `IElementRenderer::drawSurface` is the one piece of this plugin that depends on exact details of the installed Hyprland headers rather than anything we can pin down from the public API alone. De-risk it in isolation before building the real corner shader on top.

- [ ] **Step 1: Locate the installed headers and confirm the target function's exact mangled/demangled form**

Run: `pacman -Ql hyprland | grep ElementRenderer` (or search wherever the plugin dev headers were installed to) to find the installed path of `ElementRenderer.hpp`, and confirm the class is `IElementRenderer` with a method `drawSurface` taking `(Render::CRenderContext&, WP<CSurfacePassElement>, const CRegion&)` matching what was verified from upstream source during design. Record the exact installed header path and signature in `docs/hook-notes.md`.

- [ ] **Step 2: Write `SeamHook.hpp`**

```cpp
#pragma once

namespace SeamHook {
    bool install();
    void remove();
}
```

- [ ] **Step 3: Write a log-only `SeamHook.cpp`**

```cpp
#include "SeamHook.hpp"
#include "globals.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/helpers/Log.hpp>

namespace {
CFunctionHook* g_hook = nullptr;

using drawSurface_t = void (*)(void* thisptr, Render::CRenderContext&, WP<CSurfacePassElement>, const CRegion&);

void hkDrawSurface(void* thisptr, Render::CRenderContext& ctx, WP<CSurfacePassElement> element, const CRegion& damage) {
    Debug::log(LOG, "[hypr-seam] drawSurface fired");
    (*reinterpret_cast<drawSurface_t>(g_hook->m_original))(thisptr, ctx, element, damage);
}
} // namespace

bool SeamHook::install() {
    const auto matches = HyprlandAPI::findFunctionsByName(PHANDLE, "drawSurface");
    if (matches.empty()) {
        HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] Could not locate drawSurface to hook — rendering will not work.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        return false;
    }

    g_hook = HyprlandAPI::createFunctionHook(PHANDLE, matches[0].address, reinterpret_cast<void*>(&hkDrawSurface));
    return g_hook && g_hook->hook();
}

void SeamHook::remove() {
    if (g_hook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_hook);
    g_hook = nullptr;
}
```

> If `findFunctionsByName` returns more than one match (e.g. overloads or a popup-vs-window-vs-layer variant), this step's manual test (Step 5) must disambiguate by logging each match's `demangled` string and picking the one matching the confirmed signature from Step 1 — do not assume `matches[0]` blindly in the final version; add that check here if more than one match comes back.

- [ ] **Step 4: Wire into `main.cpp`**

```cpp
#include "SeamHook.hpp"

// In PLUGIN_INIT, after everything else:
if (!SeamHook::install()) {
    // Notification already shown by install(); nothing further to do, plugin
    // stays loaded but inert for rendering.
}

// In PLUGIN_EXIT:
SeamHook::remove();
```

- [ ] **Step 5: Build, load, and manually verify the hook fires**

Run: `make all && hyprctl plugin load "$(pwd)/hypr-seam.so"`

Run: `tail -f ~/.cache/hypr/hyprland.log` (or wherever `Debug::log` writes) in another terminal, open/move a window, and confirm `[hypr-seam] drawSurface fired` lines appear repeatedly — and, critically, that the compositor keeps rendering normally (no missing windows, no crash), proving the call-through to `m_original` is correctly shaped.

Resolve any mismatch between Step 1's recorded signature and what actually compiles/runs here, and update `docs/hook-notes.md` with the final confirmed signature and calling convention before moving on — this file is what Task 7 will build the real shader logic against.

Run: `hyprctl plugin unload "$(pwd)/hypr-seam.so"` when done.

- [ ] **Step 6: Write `docs/hook-notes.md` with final findings**

Include: the confirmed header path, the confirmed function signature (including the exact `this`-pointer handling that worked), the confirmed location of the GL scissor/clip primitive usable from within this hook (grep the installed `OpenGL.hpp`/`.cpp` for `scissor(` as a starting point, matching the usage already observed in `renderRoundedShadow` during design research), and any surprises hit along the way.

- [ ] **Step 7: Commit**

```bash
git add SeamHook.hpp SeamHook.cpp main.cpp meson.build docs/hook-notes.md
git commit -m "Add log-only drawSurface hook spike and document findings"
```

---

### Task 7: Per-Corner Shader & In-Place Corner Redraw

**Files:**
- Create: `SeamShader.hpp` (GLSL source as string constants)
- Modify: `SeamHook.cpp` (replace the log-only body with real scissor-exclude + corner redraw)
- Modify: `SeamState.hpp` / `SeamState.cpp` if `liveCornersFor` needs adjustment based on Task 6 findings

**Interfaces:**
- Consumes: `SeamState::liveCornersFor(PHLWINDOW)` (Task 5); the confirmed hook signature and scissor primitive from `docs/hook-notes.md` (Task 6).
- Produces: the finished visual feature — no further tasks depend on new interfaces from this one.

- [ ] **Step 1: Write `SeamShader.hpp`**

```cpp
#pragma once

// Matches Hyprland's own squircle SDF shape (rounding_power exponent) so corners
// drawn by this plugin look consistent with how native rounding used to look.
inline const char* SEAM_CORNER_FRAG_SRC = R"(
precision highp float;
varying vec2 v_texcoord;
uniform sampler2D tex;
uniform vec2  cornerSize;   // box size in pixels this fragment shader covers
uniform vec2  curveOrigin;  // the squircle's center, in the same pixel space as cornerSize
uniform float radius;
uniform float roundingPower;

void main() {
    vec2  px = v_texcoord * cornerSize;
    vec2  d  = abs(px - curveOrigin);
    float dist = pow(pow(d.x, roundingPower) + pow(d.y, roundingPower), 1.0 / roundingPower);

    if (dist > radius)
        discard; // outside the curve: leave whatever's already in the framebuffer untouched

    gl_FragColor = texture2D(tex, v_texcoord);
}
)";

inline const char* SEAM_CORNER_VERT_SRC = R"(
attribute vec2 pos;
attribute vec2 texcoord;
varying vec2 v_texcoord;
uniform mat3 proj;

void main() {
    v_texcoord = texcoord;
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
}
)";
```

> This is a starting point, not a pixel-perfect final shader — getting the `proj` matrix convention and attribute names to match Hyprland's own shader-loading helpers (`ShaderLoader.hpp`/`Shader.hpp`, observed to exist during design research but not read in detail) is exactly the kind of thing only discoverable by compiling against the real headers. Treat mismatches here as expected; fix them against the installed `Shader.hpp` API as part of Step 3's build-and-test loop, not as a sign the approach is wrong.

- [ ] **Step 2: Replace `hkDrawSurface` in `SeamHook.cpp` with the scissor-exclude + redraw logic**

```cpp
#include "SeamState.hpp"
#include "SeamShader.hpp"

namespace {
// ... g_hook, drawSurface_t, matches as in Task 6 ...

struct SCornerBox { double x, y, size; }; // size is both width and height — corners are square

void drawOneCorner(Render::CRenderContext& ctx, SP<ITexture> tex, const SCornerBox& box, double radius, double roundingPower) {
    if (radius <= 0.5)
        return; // effectively square already; nothing to redraw

    // Use the scissor primitive confirmed in docs/hook-notes.md to restrict GL
    // writes to exactly this box, then issue a textured quad draw using
    // SEAM_CORNER_VERT_SRC / SEAM_CORNER_FRAG_SRC with `radius` and
    // `roundingPower` as uniforms and `tex` bound to the window's own texture.
    //
    // Fill in using the confirmed shader-loading and scissor APIs from
    // docs/hook-notes.md — this is the one spot in the plugin where the exact
    // GL call sequence depends on headers this plan couldn't read directly.
}

void hkDrawSurface(void* thisptr, Render::CRenderContext& ctx, WP<CSurfacePassElement> element, const CRegion& damage) {
    auto window = element->m_data.pWindow;
    auto corners = window ? SeamState::liveCornersFor(window) : nullptr;

    if (!corners || !element->m_data.texture) {
        (*reinterpret_cast<drawSurface_t>(g_hook->m_original))(thisptr, ctx, element, damage);
        return;
    }

    const auto box = element->getTexBox();

    // Exclude the 4 corner squares from the native paint. The exact region/clip
    // API confirmed in docs/hook-notes.md goes here — conceptually:
    //   prunedDamage = damage - (4 corner boxes sized to this window's live radii)
    CRegion prunedDamage = damage; // placeholder assignment; replace with the real subtraction once confirmed

    (*reinterpret_cast<drawSurface_t>(g_hook->m_original))(thisptr, ctx, element, prunedDamage);

    static auto PPOWER = CConfigValue<Config::FLOAT>("plugin:seam:rounding_power");

    drawOneCorner(ctx, element->m_data.texture, {box.x, box.y, corners->topLeft}, corners->topLeft, *PPOWER);
    drawOneCorner(ctx, element->m_data.texture, {box.x + box.width - corners->topRight, box.y, corners->topRight}, corners->topRight, *PPOWER);
    drawOneCorner(ctx, element->m_data.texture, {box.x, box.y + box.height - corners->bottomLeft, corners->bottomLeft}, corners->bottomLeft, *PPOWER);
    drawOneCorner(ctx, element->m_data.texture, {box.x + box.width - corners->bottomRight, box.y + box.height - corners->bottomRight, corners->bottomRight}, corners->bottomRight, *PPOWER);
}
}
```

- [ ] **Step 3: Build, load, and iterate against real rendering**

Run: `make all && hyprctl plugin load "$(pwd)/hypr-seam.so"`

This step is where the placeholder region-subtraction and the unfinished `drawOneCorner` body get filled in for real, using whatever `CRegion` subtraction method and shader-loading calls were confirmed in `docs/hook-notes.md`. Iterate: build, load, look at a single window — it should render identically to a normal rounded window at this point (no seam enabled yet, corners just reproduce the base radius via our own shader instead of native rounding). If a single window's corners look wrong, this is purely a shader/geometry bug to fix before testing seam behavior at all.

Expected after this step is solid: a single window shows 4 correctly rounded corners at its configured base radius, rendered entirely by this plugin's shader (native `decoration:rounding = 0` confirmed still set).

- [ ] **Step 4: Full manual verification pass (the spec's Testing Plan, now that rendering exists)**

With `plugin:seam:enabled = true` set for this test pass, verify each of:
- Two windows side-by-side: the shared vertical edge's two corners flatten to `seam_radius`; the two outer corners stay at the base radius.
- A 4-way split: the single shared center point flattens on all four windows.
- A 3-way T-split: the shared point flattens on all three windows.
- A single window alone on a workspace: no corner ever flattens.
- A floating window placed directly against a tiled window's edge: neither window's corner flattens.
- A `seamrule = rounding ...` per-app rule applied to a running app of that class: that app's base corners are asymmetric as configured.
- Resize and drag a tiled window live: corners update smoothly, with visible easing (confirm `plugin:seam:animate = true` produces a visibly gradual transition, and `= false` produces an immediate snap).
- Open and close several windows in quick succession: no crash, no leaked/stale corner state (check `g_entries` doesn't grow after windows are closed, e.g. via the debug dispatcher from Task 5 if still present).
- If a live/video wallpaper is running: the flattened corner shows the live wallpaper correctly and in sync with the rest of the screen, not a static or stale image.

Run: `hyprctl plugin unload "$(pwd)/hypr-seam.so"` when the pass is clean.

- [ ] **Step 5: Remove the headless test binaries' build artifacts from the repo root if any were left behind, and do a final full rebuild from clean**

Run: `make clean && make all`
Expected: clean build with no errors.

- [ ] **Step 6: Commit**

```bash
git add SeamShader.hpp SeamHook.cpp
git commit -m "Implement per-corner squircle shader and in-place corner redraw"
```

---

## Self-Review Notes

**Spec coverage:** Base per-corner rounding (Tasks 3, 5, 7), seam adjacency incl. T/X-junctions (Tasks 2, 5), floating-window exclusion (Tasks 2, 3, 5, verified again in Task 7), `seamrule` config (Tasks 3, 4), animation (Task 5, 7), the hook-based rendering mechanism and its live-wallpaper correctness property (Tasks 6, 7), the `decoration:rounding = 0` guard (Task 4), hysteresis (Task 5). Border/shadow/blur are explicitly out of scope per the spec and this plan does not build them.

**Type consistency:** `SSeamBox`, `SCornerFlags`, `SCornerRadii`, `SGlobalSeamDefaults`, `SSeamRule`, `SResolvedWindowConfig` are defined once (Tasks 2–3) and reused by name, unchanged, through Tasks 4–7.

**Known deliberate deviations from "no placeholders":** Tasks 6 and 7 contain explicit, narrowly-scoped "confirm against the installed headers" steps for the parts of this plan that depend on exact Hyprland internal signatures this plan's author could not execute-and-verify directly (the hook calling convention, the GL scissor primitive, the shader-loading API). Each such spot names exactly what to check and where, and is followed by a concrete manual-test step that proves it was resolved correctly — this is different from an undefined "add appropriate handling," and is called out explicitly rather than hidden.
