# hypr-seam — Design Spec

Date: 2026-10-04
Status: Draft, pending user review

## Overview

`hypr-seam` is a Hyprland plugin that replaces Hyprland's native single-radius
window rounding with a per-corner rounding system. It adds two independent
features on top of that:

1. **Base rounding** — each window's four corners can be configured
   independently (asymmetric rounding), globally and/or per application.
2. **Seam** — an optional flag (global and/or per-app) that, for tiled
   windows only, detects when a corner is touching a neighboring tiled
   window's edge and overrides just that corner's radius to a small value
   (default `2px`), producing a "book spine" seam line where two windows
   meet, while leaving the window's other corners at their configured
   radius.

## Goals

- Per-corner asymmetric rounding, configurable globally and per app.
- A `seam` flag that flattens only the corners of a tiled window that are
  actually touching another tiled window, including T-junctions and
  X-junctions (3-4 windows meeting at one point), leaving corners that face
  the monitor edge or empty space untouched.
- Floating windows always render with base rounding only; they never
  participate in seam detection, either as the window being flattened or as
  a neighbor that causes another window to flatten.
- Config feel consistent with native Hyprland (`windowrulev2`-style
  per-app matching).

## Non-goals (current scope)

- Border-pass and shadow-pass rounding. The user's config currently runs
  `border_size = 0` and `shadow:enabled = false`, so neither pass needs
  per-corner treatment. If borders or shadows are re-enabled later, this is
  a follow-up, not part of this spec.
- Blur-aware corner compositing. Blur is currently disabled
  (`decoration:blur:enabled = false`). With blur off, the corner patch only
  ever needs to redraw plain wallpaper, not a blurred backdrop. See
  "Rendering Mechanism" for what changes if blur is re-enabled later.

## Terminology

- **Base rounding**: the four corner radii a window would have with no seam
  override applied — resolved from a per-app rule if one matches, else the
  four global default values.
- **Seam**: the override that replaces a corner's base radius with
  `seam_radius` (default `2`) when that corner is detected as touching
  another tiled window.

## Configuration

Global config values (`addConfigValueV2`, prefix `plugin:seam:`):

| Key | Type | Default | Meaning |
|---|---|---|---|
| `plugin:seam:rounding` | int | `22` | Uniform base radius when corners aren't individually set. |
| `plugin:seam:rounding_topleft` | int | = `rounding` | Base top-left radius. |
| `plugin:seam:rounding_topright` | int | = `rounding` | Base top-right radius. |
| `plugin:seam:rounding_bottomleft` | int | = `rounding` | Base bottom-left radius. |
| `plugin:seam:rounding_bottomright` | int | = `rounding` | Base bottom-right radius. |
| `plugin:seam:rounding_power` | float | `2.0` | Squircle exponent, matches Hyprland's native default look. |
| `plugin:seam:enabled` | bool | `false` | Global seam master switch. |
| `plugin:seam:seam_radius` | int | `2` | Radius a flagged corner collapses to. |
| `plugin:seam:tolerance` | int | `6` (px) | Max pixel gap between two edges still considered "touching." Covers `gaps_in` plus float/scale slop. |

Per-app rules via a new config keyword `seamrule`, parsed by the plugin
itself (Hyprland's plugin API has no hook into the native `windowrulev2`
engine, so this mirrors its syntax rather than extending it):

```
seamrule = rounding <tl> <tr> <bl> <br>, <match>
seamrule = seam <0|1>, <match>
```

Example:

```
seamrule = rounding 4 4 22 22, class:^(kitty)$
seamrule = seam 1, class:^(mpv)$
seamrule = seam 0, class:^(foot)$   # opt out even if plugin:seam:enabled = true
```

**Resolution per window:**
- Corner radii: per-app `rounding` override if a `seamrule` matches this
  window's class/title, else the four global `rounding_*` values.
- Seam flag: per-app `seam` override if a `seamrule` matches, else
  `plugin:seam:enabled`.
- Floating windows always use "corner radii" resolution above and **skip**
  seam resolution entirely — base rounding only, regardless of the seam
  flag's resolved value.

## Adjacency Detection

Scope: **tiled windows only**. Floating windows are excluded both as the
subject of flattening and as a potential neighbor.

Recompute is event-driven (not per-frame): triggered by `openWindow`,
`closeWindow`, `moveWindow`, `changeFloatingMode`, `fullscreenState`,
`workspace` switch, and monitor add/remove/move. Cheap by construction — a
workspace with N visible tiled windows is an O(N²) pairwise pass over a
small N, done once per layout change and cached until the next one.

For each tiled window's real render box (gap-inset, the same box Hyprland
renders), check each of its 4 corner points individually:

> Corner `C` flattens to `seam_radius` if any other visible tiled window `N`
> has an edge — within `plugin:seam:tolerance` pixels — that both (a) sits
> at `C`'s x or y coordinate, and (b) extends far enough along the other
> axis to actually pass through `C`.

This per-corner-point check (rather than requiring one single clean shared
edge) is what makes T-junctions and X-junctions fall out with no special
casing — a 4-way split where four windows meet at one point flattens that
corner on all four. Corners facing the monitor edge or empty space are
correctly left alone, since nothing satisfies the check there.

**Hysteresis:** once a corner is flagged as touching, require the gap to
exceed `tolerance` (not just reach it) before un-flagging, to avoid flicker
from a window sitting exactly at the boundary.

## Rendering Mechanism

Requires native `decoration:rounding = 0` globally — the plugin fully owns
corner rendering for every window it manages, computed once and shared by
both the base-rounding and seam features (seam is just a corner-radius
override feeding the same renderer, not a separate code path).

Mechanism: a custom per-window decoration (`addWindowDecoration`, same
public-API pattern as the official `borders-plus-plus` plugin) pushes its
own render-pass element after Hyprland draws that window's square content.
For each of the 4 corners, the element draws a small patch sized to that
corner's resolved radius, compositing:

- **Inside** the corner's squircle curve (SDF using `rounding_power`,
  matching Hyprland's native look): keep the window's own already-painted
  content.
- **Outside** the curve: **redraw** the correct backdrop — do not simply
  write alpha 0. Hyprland paints back-to-front (wallpaper, then windows);
  by the time our pass runs, the window has already been painted fully
  opaque and square over the real backdrop pixels, so a naive transparency
  punch would not "reveal" anything — the correct pixels are already gone.
  Instead, the patch samples the monitor's wallpaper texture (public
  `getBackground()` accessor) and paints it directly into the
  outside-curve region, blending at the SDF edge for anti-aliasing.

**If blur is re-enabled later:** this patch would additionally need to
sample the same pre-blurred monitor backdrop Hyprland computes once per
frame (via its `PreBlurElement` pass) for the outside-curve region of any
window with blur active, instead of plain wallpaper. Whether that shared
blurred texture remains valid/sampleable from a later, separate pass is an
open unknown that would need prototyping at that time, with a safe fallback
of plain wallpaper (no blur) if it isn't reusable. Out of scope while blur
stays disabled.

## Error Handling & Edge Cases

- **Mid-drag resize/move:** recompute is event-driven, so radii are briefly
  stale mid-drag until the next `moveWindow` event — acceptable, matches
  how Hyprland's own layout updates behave.
- **Window smaller than radius sum:** clamp each corner's effective radius
  to `min(configured_radius, width/2, height/2)`, same safeguard Hyprland's
  native rounding already applies.
- **Flicker at exact tolerance boundary:** handled by hysteresis (above).
- **Monitor scale / HiDPI:** all geometry and radii computed in the same
  scaled pixel space Hyprland uses internally (logical × monitor scale),
  matching how native `round` is already scaled.
- **Unmanaged surfaces:** plugin only attaches to normal toplevel windows,
  never layer-shell surfaces, splash screens, or lock screens.

## Testing Plan

- Manual visual verification (no Hyprland plugin test harness exists for
  pixel-level GPU output) across: two windows side-by-side (vertical seam),
  4-way split (X-junction), 3-way T-split, single window alone (seam never
  fires), a floating window next to a tiled window (neither affects the
  other), asymmetric per-app rule applied to a matched app, resize/drag
  live, open/close windows rapidly.
- A small headless unit test (plain C++, no Hyprland runtime) for the pure
  adjacency-detection function, since that logic is decoupled from
  rendering and easy to test in isolation with fabricated window boxes.

## Out of Scope / Deferred

- Border-pass and shadow-pass per-corner rounding (both features disabled
  in current config).
- Blur-aware corner compositing (blur currently disabled).
