#pragma once
#include <algorithm>
#include <vector>

struct SSeamBox {
    double x, y, w, h;
    bool   floating;
    int    id;
};

struct SCornerFlags {
    bool topLeft = false, topRight = false, bottomLeft = false, bottomRight = false;
};

// Stateless point-in-time check. Hysteresis (requiring a gap to exceed, not just
// reach, `tolerance` before un-flagging) is applied by the caller across repeated
// calls — see SeamState.cpp.
SCornerFlags computeTouchingCorners(const SSeamBox& subject, const std::vector<SSeamBox>& all, double tolerance);

constexpr double clampCornerRadius(double radius, double width, double height) {
    return std::min({radius, width / 2.0, height / 2.0});
}
