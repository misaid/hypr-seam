#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <optional>
#include <string>

// Tracks every mapped window's live, animated corner-radius state and keeps it in
// sync with adjacency (Adjacency.hpp) + per-window config (SeamConfig.hpp).
namespace SeamState {
    struct SLiveCorners {
        double topLeft, topRight, bottomLeft, bottomRight;
    };

    // Recomputes adjacency and resolved config for every tracked window and retargets
    // its animated corner radii. Runs on every event that can change which corners
    // touch a neighbor: window open/close/floating/fullscreen, workspace change,
    // monitor add/remove/layout change, and config reload. Hyprland has no
    // window-move/resize event, so the tick fallback in main.cpp also calls it when a
    // window's position or size changes. Adjacency uses each window's GOAL geometry,
    // so a recompute that fires mid-animation sees the final layout.
    void recomputeAll();

    // Per-window live corner radii, read every frame by the render hooks (SeamHook.cpp, SeamDecorHook.cpp).
    // Returns std::nullopt if the window isn't tracked (mapped windows always are).
    std::optional<SLiveCorners> liveCornersFor(const PHLWINDOW& window);

    void onWindowOpened(const PHLWINDOW& window);
    void onWindowClosed(const PHLWINDOW& window);

    // Drops every tracked window entry and its animated variables, and cancels any
    // pending deferred recompute. PLUGIN_EXIT calls this so that no window refs,
    // animated vars or queued callbacks outlive the plugin.
    void clear();

    // Hyprland's event bus has no window move/resize event (EventBus.hpp only has
    // open, close, floating, fullscreen and moveToWorkspace for windows). main.cpp
    // calls this from its `tick` listener. It fingerprints every mapped window's GOAL
    // position, size and floating state, and runs recomputeAll() only when the
    // fingerprint changed since the last call.
    //
    // `tick` fires in bursts while the compositor renders and stops when it goes
    // idle, so this is a best-effort catch-all. Correctness comes from
    // recomputeAll() using GOAL geometry, which doesn't depend on when this fires.
    void onTick();

    // Backs the `seam:debugstate` dispatcher in main.cpp. Returns every tracked
    // window's live corner radii as text, with '*' after each corner currently
    // flagged as touching. The render path doesn't use it; it's for bug reports.
    std::string debugDump();
}
