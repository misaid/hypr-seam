#include "SeamConfig.hpp"
#include <regex>
#include <sstream>

namespace {
struct SPatternMatchResult {
    bool ok;      // false if the pattern is malformed, or valid but didn't match
    bool matched; // true if the pattern is non-empty and valid
};

SPatternMatchResult patternMatches(const std::string& pattern, const std::string& subject) {
    if (pattern.empty())
        return {true, false}; // this field is unconstrained by the rule
    std::regex re;
    try {
        re = std::regex(pattern);
    } catch (const std::regex_error&) {
        return {false, false}; // malformed pattern: skip the whole rule instead of throwing
    }
    return {std::regex_search(subject, re), true};
}
} // namespace

SResolvedWindowConfig resolveWindowConfig(const std::string& windowClass, const std::string& windowTitle, bool isFloating,
                                          const SGlobalSeamDefaults& defaults, const std::vector<SSeamRule>& rules) {
    SResolvedWindowConfig result{.radii = defaults.baseRadii, .seamEnabled = defaults.seamEnabled};

    for (const auto& rule : rules) {
        const auto classResult = patternMatches(rule.classPattern, windowClass);
        if (!classResult.ok)
            continue;
        const auto titleResult = patternMatches(rule.titlePattern, windowTitle);
        if (!titleResult.ok)
            continue;
        if (!classResult.matched && !titleResult.matched)
            continue; // a rule with no matcher at all never matches

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

    // trim trailing whitespace on the match part (a regex can't meaningfully end in
    // unescaped whitespace from a config line, and hyprlang may leave some)
    const auto lastNonSpace = match.find_last_not_of(" \t");
    match                   = lastNonSpace == std::string::npos ? std::string{} : match.substr(0, lastNonSpace + 1);

    const std::string classPrefix = "class:";
    const std::string titlePrefix = "title:";
    outRule.classPattern.clear();
    outRule.titlePattern.clear();
    if (match.rfind(classPrefix, 0) == 0)
        outRule.classPattern = match.substr(classPrefix.size());
    else if (match.rfind(titlePrefix, 0) == 0)
        outRule.titlePattern = match.substr(titlePrefix.size());
    else
        return false;

    if (outRule.classPattern.empty() && outRule.titlePattern.empty())
        return false;

    std::istringstream iss(directive);
    std::string        keyword;
    iss >> keyword;

    if (keyword == "seam") {
        int val{};
        if (!(iss >> val) || !(iss >> std::ws).eof())
            return false;
        outRule.isSeamDirective = true;
        outRule.seamOn          = val != 0;
        return true;
    }

    if (keyword == "rounding") {
        double tl{}, tr{}, bl{}, br{};
        if (!(iss >> tl >> tr >> bl >> br) || !(iss >> std::ws).eof())
            return false;
        outRule.isSeamDirective = false;
        outRule.radii           = {tl, tr, bl, br};
        return true;
    }

    return false;
}
