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

// Checks one moment and keeps no state. SeamState.cpp adds hysteresis across calls:
// a corner that is already flagged stays flagged until the gap exceeds 1.5x `tolerance`.
SCornerFlags computeTouchingCorners(const SSeamBox& subject, const std::vector<SSeamBox>& all, double tolerance);

constexpr double clampCornerRadius(double radius, double width, double height) {
    return std::min({radius, width / 2.0, height / 2.0});
}
