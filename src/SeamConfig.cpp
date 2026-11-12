#include "SeamConfig.hpp"
#include <algorithm>
#include <charconv>
#include <expected>
#include <system_error>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>
#include <unordered_map>

namespace {
struct SPatternMatchResult {
    bool ok;      // the rule may still apply: matcher is absent or matched
    bool matched; // the rule constrains this field
};

// Compiled-pattern cache, since rules re-resolve on every title change. Keyed by pattern
// text, so entries never go stale; std::nullopt marks a malformed pattern. Main thread
// only, so no lock.
const std::optional<std::regex>& compiledPattern(const std::string& pattern) {
    static std::unordered_map<std::string, std::optional<std::regex>> cache;

    auto it = cache.find(pattern);
    if (it == cache.end()) {
        std::optional<std::regex> compiled;
        try {
            compiled.emplace(pattern);
        } catch (const std::regex_error&) {
            // malformed: cached as std::nullopt so the rule is skipped
        }
        it = cache.emplace(pattern, std::move(compiled)).first;
    }
    return it->second;
}

SPatternMatchResult patternMatches(const std::string& pattern, const std::string& subject) {
    if (pattern.empty())
        return {true, false}; // unconstrained
    const auto& re = compiledPattern(pattern);
    if (!re)
        return {false, false}; // malformed: skip the rule
    return {std::regex_search(subject, *re), true};
}

// Named and special workspaces have no ID, and an empty optional never equals a value, so
// an ID selector cannot match them.
bool selectorMatches(const WorkspaceSelector& selector, const SWorkspaceRef& workspace) {
    if (const auto* id = std::get_if<SWorkspaceId>(&selector))
        return workspace.id == id->value;
    const auto& name = std::get<SWorkspaceName>(selector).value;
    return name == workspace.address || name == workspace.displayName;
}

constexpr std::string_view kWhitespace = " \t";

std::string_view trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos)
        return {};
    const auto last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

// Parses all of `text` as a decimal integer; result_out_of_range if too large, otherwise
// invalid_argument (e.g. "2b", "+2"). Signed, so "-1" is caught as a bad ID.
std::expected<std::int64_t, std::errc> parseInteger(std::string_view text) {
    std::int64_t value{};
    const auto   end     = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{})
        return std::unexpected{ec};
    if (ptr != end)
        return std::unexpected{std::errc::invalid_argument};
    return value;
}

// True if `text` reads as a number of any size, valid ID or not.
bool looksNumeric(std::string_view text) {
    const auto parsed = parseInteger(text);
    return parsed || parsed.error() == std::errc::result_out_of_range;
}

// A rule's workspace matcher. An empty list leaves the workspace unconstrained.
SPatternMatchResult workspaceMatches(const std::vector<WorkspaceSelector>& selectors, const std::optional<SWorkspaceRef>& workspace) {
    if (selectors.empty())
        return {true, false};
    if (!workspace)
        return {false, true}; // constrained, but the window has no workspace
    const bool any = std::ranges::any_of(selectors, [&](const WorkspaceSelector& s) { return selectorMatches(s, *workspace); });
    return {any, true};
}
} // namespace

std::optional<WorkspaceSelector> parseWorkspaceSelector(std::string_view text) {
    constexpr std::string_view kNamePrefix = "name:";

    text = trimmed(text);
    if (text.empty())
        return std::nullopt;

    if (text.starts_with(kNamePrefix)) {
        const auto name = trimmed(text.substr(kNamePrefix.size()));
        // Numeric names can't exist in Hyprland, so `name:2` is a mistake, not a name.
        if (name.empty() || looksNumeric(name))
            return std::nullopt;
        return SWorkspaceName{std::string{name}};
    }

    if (const auto id = parseInteger(text)) {
        if (*id < 0 || *id > kMaxWorkspaceId)
            return std::nullopt;
        return SWorkspaceId{*id};
    }
    if (looksNumeric(text))
        return std::nullopt; // too large for any workspace

    return SWorkspaceName{std::string{text}};
}

SResolvedWindowConfig resolveWindowConfig(const std::string& windowClass, const std::string& windowTitle, bool isFloating,
                                          const SGlobalSeamDefaults& defaults, const std::vector<SSeamRule>& rules,
                                          const std::optional<SWorkspaceRef>& workspace) {
    SResolvedWindowConfig result{.radii = defaults.baseRadii, .seamEnabled = defaults.seamEnabled};

    for (const auto& rule : rules) {
        const auto classResult = patternMatches(rule.classPattern, windowClass);
        if (!classResult.ok)
            continue;
        const auto titleResult = patternMatches(rule.titlePattern, windowTitle);
        if (!titleResult.ok)
            continue;
        const auto workspaceResult = workspaceMatches(rule.workspaces, workspace);
        if (!workspaceResult.ok)
            continue;
        if (!classResult.matched && !titleResult.matched && !workspaceResult.matched)
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

    // Trim the matcher; trailing whitespace can't be part of a meaningful regex.
    const auto firstNonSpace = match.find_first_not_of(' ');
    if (firstNonSpace != std::string::npos)
        match = match.substr(firstNonSpace);

    const auto lastNonSpace = match.find_last_not_of(" \t");
    match                   = lastNonSpace == std::string::npos ? std::string{} : match.substr(0, lastNonSpace + 1);

    const std::string classPrefix = "class:";
    const std::string titlePrefix = "title:";
    const std::string workspacePrefix = "workspace:";
    outRule.classPattern.clear();
    outRule.titlePattern.clear();
    outRule.workspaces.clear();
    if (match.rfind(classPrefix, 0) == 0)
        outRule.classPattern = match.substr(classPrefix.size());
    else if (match.rfind(titlePrefix, 0) == 0)
        outRule.titlePattern = match.substr(titlePrefix.size());
    else if (match.rfind(workspacePrefix, 0) == 0) {
        const auto selectorText = std::string_view{match}.substr(workspacePrefix.size());
        if (selectorText.contains(','))
            return false; // a second matcher; the string form takes exactly one
        auto selector = parseWorkspaceSelector(selectorText);
        if (!selector)
            return false;
        outRule.workspaces.push_back(std::move(*selector));
    } else
        return false;

    if (outRule.classPattern.empty() && outRule.titlePattern.empty() && outRule.workspaces.empty())
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
