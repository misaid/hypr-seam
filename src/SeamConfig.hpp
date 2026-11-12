#ifndef SEAMCONFIG_HPP
#define SEAMCONFIG_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct SCornerRadii {
    double topLeft     = 0.0;
    double topRight    = 0.0;
    double bottomLeft  = 0.0;
    double bottomRight = 0.0;

    bool operator==(const SCornerRadii&) const = default;
};

struct SGlobalSeamDefaults {
    SCornerRadii baseRadii;
    double       roundingPower = 0.0;
    bool         seamEnabled   = false;
    double       seamRadius    = 0.0;
    double       tolerance     = 0.0;

    // Defaulted so new fields are compared too; SeamState's tick check depends on it.
    bool operator==(const SGlobalSeamDefaults&) const = default;
};

// A workspace selected by numeric ID (`2`). Only numbered workspaces have one.
struct SWorkspaceId {
    std::int64_t value = 0;

    bool operator==(const SWorkspaceId&) const = default;
};

// A workspace selected by exact name (`web`, `special:scratch`). Never numeric: Hyprland
// forbids numeric workspace names, which keeps ID and name selectors from overlapping.
struct SWorkspaceName {
    std::string value;

    bool operator==(const SWorkspaceName&) const = default;
};

// One entry of a rule's `workspace` matcher.
using WorkspaceSelector = std::variant<SWorkspaceId, SWorkspaceName>;

// A window's current workspace, as plain data so this header needs no Hyprland includes.
// Only numbered workspaces have an ID. A name selector matches `address` (what Hyprland's
// selectors use: "2", "web", "special:scratch") or `displayName` (the renamed label).
struct SWorkspaceRef {
    std::optional<std::int64_t> id;
    std::string                 address;
    std::string                 displayName;
};

// Largest workspace ID. Mirrors Workspace::WorkspaceIDContainer (uint32_t); SeamState.cpp
// static_asserts that the two agree.
inline constexpr std::int64_t kMaxWorkspaceId = 0xFFFFFFFFLL;

// Parses one selector, ignoring surrounding whitespace. A number is an ID, anything else a
// name (optionally prefixed `name:`). std::nullopt for empty input, a numeric name, or an
// ID outside 0..kMaxWorkspaceId.
std::optional<WorkspaceSelector> parseWorkspaceSelector(std::string_view text);

struct SSeamRule {
    bool          isSeamDirective = false; // true: `seam` rule, false: `rounding` rule
    SCornerRadii  radii;                   // rounding rules only
    bool          seamOn          = false; // seam rules only
    std::string   classPattern;            // ECMAScript regex on the window class; empty = any
    std::string   titlePattern;            // ECMAScript regex on the window title; empty = any
    std::vector<WorkspaceSelector> workspaces; // any entry may match; empty = any
};

struct SResolvedWindowConfig {
    SCornerRadii radii;
    bool         seamEnabled = false;
};

// Resolves a window's config from the defaults and rules. A rule applies when every
// matcher it has matches (one with no matchers never does), and the last applicable rule
// wins. A window with no `workspace` never matches a workspace matcher. Floating windows
// never get seam.
SResolvedWindowConfig resolveWindowConfig(const std::string& windowClass, const std::string& windowTitle, bool isFloating,
                                          const SGlobalSeamDefaults& defaults, const std::vector<SSeamRule>& rules,
                                          const std::optional<SWorkspaceRef>& workspace = std::nullopt);

// Parses a rule string: `<directive>, <matcher>`, where directive is `seam <0|1>` or
// `rounding <tl> <tr> <bl> <br>` and matcher is exactly one of `class:<regex>`,
// `title:<regex>` or `workspace:<selector>`. Returns false on any malformed or trailing
// input.
bool parseSeamRuleLine(const std::string& value, SSeamRule& outRule);

#endif // SEAMCONFIG_HPP
