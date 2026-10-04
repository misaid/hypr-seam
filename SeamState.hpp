#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <string>

// Tracks every mapped window's live, animated corner-radius state and keeps it in
// sync with adjacency (Adjacency.hpp) + per-window config (SeamConfig.hpp).
//
// Consumes (do not modify): SSeamBox/computeTouchingCorners/clampCornerRadius (Task 2),
// SResolvedWindowConfig/resolveWindowConfig (Task 3), SeamRuleStore::rules() and
// currentGlobalDefaults() (Task 4).
namespace SeamState {
    struct SLiveCorners {
        double topLeft, topRight, bottomLeft, bottomRight;
    };

    // Recomputes adjacency + resolved config for every tracked window and retargets
    // its animated corner radii accordingly. Called on every event that can change
    // which corners touch a neighbor: window open/close/floating-toggle/fullscreen,
    // workspace change, monitor add/remove/layout change, and (via the tick-gated
    // fallback in main.cpp, since Hyprland has no standalone window-move/resize
    // event) whenever a tracked window's position/size actually changed.
    void recomputeAll();

    // Per-window live corner radii, read every frame by the render hook (Task 7).
    // Returns nullptr if the window isn't tracked (shouldn't happen for mapped windows).
    SLiveCorners* liveCornersFor(PHLWINDOW window);

    void onWindowOpened(PHLWINDOW window);
    void onWindowClosed(PHLWINDOW window);

    // Hyprland's event bus has no standalone "window moved/resized" event (confirmed
    // against the installed EventBus.hpp — only open/close/floating/fullscreen/
    // moveToWorkspace exist for windows). Call this from a `tick` listener in
    // main.cpp; it cheaply fingerprints every mapped window's position/size/floating
    // state and only runs the full recomputeAll() when that fingerprint changed
    // since the last call, so idle ticks are nearly free.
    void onTick();

    // Debug-only: dumps every tracked window's live corner radii and touching state
    // as a human-readable string, for manual verification via a temporary dispatcher
    // (see main.cpp) before Task 7 adds real rendering. Not part of the steady-state
    // plugin interface; safe to remove once rendering lands if it's no longer useful.
    std::string debugDump();
}
