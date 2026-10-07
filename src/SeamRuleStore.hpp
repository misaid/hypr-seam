#pragma once
#include "SeamConfig.hpp"
#include <vector>

// Holds the live set of SSeamRule entries parsed from `seamrule = ...` config lines.
// Needed because Hyprlang's addConfigKeyword handler is a plain function pointer with
// no captured state, so the parsed rules have to live somewhere external.
namespace SeamRuleStore {
    void                           clear();
    void                           add(const SSeamRule& rule);
    const std::vector<SSeamRule>& rules();
}
