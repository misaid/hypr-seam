#ifndef SEAMCONFIG_HPP
#define SEAMCONFIG_HPP

#include <string>
#include <vector>

struct SCornerRadii {
    double topLeft     = 0.0;
    double topRight    = 0.0;
    double bottomLeft  = 0.0;
    double bottomRight = 0.0;
};

struct SGlobalSeamDefaults {
    SCornerRadii baseRadii;
    double       roundingPower = 0.0;
    bool         seamEnabled   = false;
    double       seamRadius    = 0.0;
    double       tolerance     = 0.0;
};

struct SSeamRule {
    bool          isSeamDirective = false; // true = "seam <0|1>" rule, false = "rounding tl tr bl br" rule
    SCornerRadii  radii;                   // valid when !isSeamDirective
    bool          seamOn          = false; // valid when isSeamDirective
    std::string   classPattern;    // ECMAScript regex source, matched against window class; empty = don't care
    std::string   titlePattern;    // ECMAScript regex source, matched against window title; empty = don't care
};

struct SResolvedWindowConfig {
    SCornerRadii radii;
    bool         seamEnabled = false;
};

// Pure resolution: later-registered rules win on conflict (last match wins). A
// rule matches when every non-empty pattern it carries (class and/or title)
// matches; a rule with neither pattern never matches. `isFloating` short-circuits seam resolution to `false`
// regardless of any rule or global default.
SResolvedWindowConfig resolveWindowConfig(const std::string& windowClass, const std::string& windowTitle, bool isFloating,
                                          const SGlobalSeamDefaults& defaults, const std::vector<SSeamRule>& rules);

// Parses one `seamrule = ...` line's value (the part after `seamrule = `) into a rule.
// Format: `<directive>, <matcher>` where directive is `seam <0|1>` or
// `rounding <tl> <tr> <bl> <br>`, and matcher is `class:<regex>` or `title:<regex>`.
// Returns false if the line is malformed (including trailing garbage after the
// directive's arguments, or an empty pattern).
bool parseSeamRuleLine(const std::string& value, SSeamRule& outRule);

#endif // SEAMCONFIG_HPP
