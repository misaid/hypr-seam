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
