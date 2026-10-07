#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>

#include "SeamConfig.hpp"

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
    SP<Config::Values::CBoolValue>   forceRoundRiskySurfaces;
    SP<Config::Values::CBoolValue>   roundBorders;
    SP<Config::Values::CBoolValue>   roundShadows;
};

inline SVars vars = {};

// Builds the resolved global seam defaults from the live plugin:seam:* config values.
// Defined (non-static, intentionally) in main.cpp so other translation units (e.g.
// Task 5's SeamState.cpp) can call it directly.
SGlobalSeamDefaults currentGlobalDefaults();
