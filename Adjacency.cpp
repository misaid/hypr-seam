#include "Adjacency.hpp"
#include <algorithm>

static bool pointOnEdge(double px, double py, const SSeamBox& n, double tolerance) {
    const bool onVerticalEdge   = (std::abs(n.x - px) <= tolerance || std::abs(n.x + n.w - px) <= tolerance) &&
        (py >= n.y - tolerance && py <= n.y + n.h + tolerance);
    const bool onHorizontalEdge = (std::abs(n.y - py) <= tolerance || std::abs(n.y + n.h - py) <= tolerance) &&
        (px >= n.x - tolerance && px <= n.x + n.w + tolerance);
    return onVerticalEdge || onHorizontalEdge;
}

static bool cornerTouches(double px, double py, const SSeamBox& subject, const std::vector<SSeamBox>& all, double tolerance) {
    for (const auto& n : all) {
        if (n.id == subject.id || n.floating)
            continue;
        if (pointOnEdge(px, py, n, tolerance))
            return true;
    }
    return false;
}

SCornerFlags computeTouchingCorners(const SSeamBox& subject, const std::vector<SSeamBox>& all, double tolerance) {
    SCornerFlags flags;

    if (subject.floating)
        return flags; // floating windows never flatten, regardless of neighbors

    flags.topLeft     = cornerTouches(subject.x, subject.y, subject, all, tolerance);
    flags.topRight    = cornerTouches(subject.x + subject.w, subject.y, subject, all, tolerance);
    flags.bottomLeft  = cornerTouches(subject.x, subject.y + subject.h, subject, all, tolerance);
    flags.bottomRight = cornerTouches(subject.x + subject.w, subject.y + subject.h, subject, all, tolerance);

    return flags;
}

double clampCornerRadius(double radius, double width, double height) {
    return std::min({radius, width / 2.0, height / 2.0});
}
