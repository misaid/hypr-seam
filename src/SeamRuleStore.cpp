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
