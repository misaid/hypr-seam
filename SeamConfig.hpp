#ifndef SEAMCONFIG_HPP
#define SEAMCONFIG_HPP

#include <string>
#include <vector>

struct SCornerRadii {
    double topLeft;
    double topRight;
    double bottomLeft;
    double bottomRight;
};

struct SGlobalSeamDefaults {
    SCornerRadii baseRadii;
    double       roundingPower;
    bool         seamEnabled;
    double       seamRadius;
    double       tolerance;
};

struct SSeamRule {
    bool          isSeamDirective; // true = "seam <0|1>" rule, false = "rounding tl tr bl br" rule
    SCornerRadii  radii;           // valid when !isSeamDirective
    bool          seamOn;          // valid when isSeamDirective
    std::string   classPattern;    // ECMAScript regex source, matched against window class
};

struct SResolvedWindowConfig {
    SCornerRadii radii;
    bool         seamEnabled;
};

// Pure resolution: later-registered rules win on conflict (last match wins),
// matching Hyprland windowrulev2 semantics. `isFloating` short-circuits seam
// resolution to `false` regardless of any rule or global default.
SResolvedWindowConfig resolveWindowConfig(const std::string& windowClass, bool isFloating,
                                          const SGlobalSeamDefaults& defaults, const std::vector<SSeamRule>& rules);

// Parses one `seamrule = ...` line's value (the part after `seamrule = `) into a rule.
// Returns false if the line is malformed.
bool parseSeamRuleLine(const std::string& value, SSeamRule& outRule);

#endif // SEAMCONFIG_HPP
