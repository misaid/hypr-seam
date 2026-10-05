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
    // workspace change, monitor add/remove/layout change, config reload, and (via
    // the tick-gated fallback in main.cpp, since Hyprland has no standalone
    // window-move/resize event) whenever a tracked window's position/size actually
    // changed. Judges adjacency against each window's GOAL geometry, not its live
    // animated position, so a recompute that happens to fire mid-animation still
    // judges against the final, settled layout rather than a transient frame.
    void recomputeAll();

    // Per-window live corner radii, read every frame by the render hook (Task 7).
    // Returns nullptr if the window isn't tracked (shouldn't happen for mapped windows).
    SLiveCorners* liveCornersFor(PHLWINDOW window);

    void onWindowOpened(PHLWINDOW window);
    void onWindowClosed(PHLWINDOW window);

    // Drops every tracked window entry (and its animated variables) and cancels
    // any pending deferred recompute. Called from PLUGIN_EXIT so no window refs,
    // animated vars, or queued callbacks outlive the plugin.
    void clear();

    // Hyprland's event bus has no standalone "window moved/resized" event (confirmed
    // against the installed EventBus.hpp — only open/close/floating/fullscreen/
    // moveToWorkspace exist for windows). Call this from a `tick` listener in
    // main.cpp; it cheaply fingerprints every mapped window's GOAL position/size/
    // floating state and only runs the full recomputeAll() when that fingerprint
    // changed since the last call. Note: `tick` is not a continuous per-frame
    // heartbeat (confirmed empirically) — it fires in bursts while the compositor
    // is actively rendering and goes silent once idle, so this is a best-effort
    // catch-all, not a guaranteed poll; recomputeAll()'s own use of GOAL geometry
    // (rather than relying on a trailing tick to "settle" the result) is what
    // actually keeps the result correct regardless of when this fires.
    void onTick();

    // Diagnostic only (backs the `seam:debugstate` dispatcher in main.cpp): dumps
    // every tracked window's live corner radii and touching state ('*' = corner
    // currently flagged as touching) as a human-readable string. Not used by the
    // render path; kept as a troubleshooting aid for bug reports.
    std::string debugDump();
}
