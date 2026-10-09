#pragma once
#include "SeamConfig.hpp"

#include <cstddef>
#include <optional>

// Decides when SeamState::onTick() has to recompute. Hyprland has no event for a window
// move or resize, or for a `hyprctl keyword` change, so every tick passes in a
// fingerprint of the visible windows' GOAL geometry and the current resolved defaults,
// and the gate reports whether either one changed since the last tick.
class CRecomputeGate {
  public:
    // True on the first call after construction or reset(), and whenever either input
    // differs from the previous call.
    [[nodiscard]] bool changed(std::size_t geometryFingerprint, const SGlobalSeamDefaults& defaults) noexcept;

    // Forgets the previous inputs, so the next changed() call returns true.
    void reset() noexcept;

  private:
    std::optional<std::size_t>         m_lastGeometry;
    std::optional<SGlobalSeamDefaults> m_lastDefaults;
};
