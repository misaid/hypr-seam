# hypr-seam — Design Spec

Date: 2026-10-04
Status: Draft, pending user review. Rendering Mechanism revised same day after
discovering the overlay-decoration approach cannot correctly handle a dynamic
backdrop (live/video wallpaper); see that section for the hook-based
replacement.

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

- Border-pass rounding. The user's config currently runs `border_size = 0`.
  Re-enabling borders later is a straightforward follow-up of the same
  shape as the content-pass work in this spec (hook the border draw call,
  scissor-exclude the 4 corners, redraw them as a per-corner ring/annulus)
  — not an open question, just not built in v1.
- Shadow-pass rounding. Shadows are disabled (`shadow:enabled = false`).
  Same status as borders: deferred, not blocked.
- Blur-aware corner compositing. Blur is currently disabled
  (`decoration:blur:enabled = false`). The chosen rendering mechanism
  (see below) reads the window's already-computed blurred backdrop texture
  from the same internal call it hooks, so re-enabling blur later requires
  no new sourcing logic — it's one extra texture read already in scope at
  the hook site, not an open technical unknown.

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
| `plugin:seam:animate` | bool | `true` | Eases a corner's radius between base and seam values instead of snapping. |
| `plugin:seam:animation_speed` | float | `300` (ms) | Duration of the easing transition. |
| `plugin:seam:animation_curve` | string | `"default"` | Name of a bezier curve already registered via Hyprland's `bezier =` / `hl.curve(...)`. |

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

**Why an overlay decoration doesn't work:** an earlier version of this
design drew the per-corner patches as a separate decoration render-pass
element running *after* Hyprland painted the window's square content.
Hyprland composites back-to-front (background/layers, then windows) into a
single framebuffer; native rounding looks correct because the window's own
shader `discard`s the corner fragments, leaving whatever was painted a
moment earlier in that same frame untouched. A later, separate overlay
pass has no such luck — by the time it runs, the opaque square content has
already overwritten the real backdrop at that pixel, so there is nothing
correct left to reveal. This is a problem independent of blur: with a live
wallpaper (a layer-shell client rendering its own animated content,
distinct from Hyprland's static `getBackground()` fallback), the real
backdrop changes every frame, so there is no static texture an overlay
pass could substitute in its place either.

**Chosen mechanism:** hook the internal call Hyprland uses to paint a
managed window's opaque content (`createFunctionHook`, the same public,
plugin-facing trampoline-hook facility used by existing community plugins
such as `hy3` — this requires no Hyprland source patch or custom build).
Inside the hook, for each of the window's 4 corners:

1. **Scissor-exclude** that corner's small box from the native paint call,
   so the native square content is never drawn there in the first place.
2. **Redraw that box ourselves**, using the plugin's own GLSL shader (the
   native shader has no per-corner radius uniform, so this can't be a
   tweak to it — it's a small shader of our own): inside the corner's
   squircle SDF curve (same `rounding_power` exponent as native, for a
   matching look), sample the window's own content texture, identical to
   what the native call would have painted there. Outside the curve,
   write nothing — since step 1 already excluded the native paint from
   this box, whatever was already in the framebuffer from the preceding
   background/layer pass (live wallpaper included) is still correct and
   untouched, automatically, with no special-casing per backdrop type.

Because this runs at the exact point in frame order where the native
paint would have happened — not in a separate later pass — it inherits
Hyprland's own "don't touch pixels you don't need to" correctness for
free, for any backdrop.

**Blur, if re-enabled later:** falls out of the same mechanism almost for
free. The hook point sits inside the same internal call where Hyprland has
already computed that window's blurred backdrop texture for this frame
(available as a local value in scope, not something we'd fetch or
snapshot separately). For a window with blur enabled, the "outside the
curve" fragments would sample that texture instead of being left
untouched; for a window without blur, they stay untouched as described
above. Not built in v1 (blur is currently disabled), but no new sourcing
logic would be needed to add it.

**Animation:** each corner's radius is a `PHLANIMVAR<float>` (Hyprland's
shared animated-variable type, the same mechanism driving native
window-move/fade/border-color animations) bound directly to the window
(`Animation::mgr()->createAnimation(v, pav, pConfig, pWindow,
AVARDAMAGE_ENTIRE)` — the overload that takes a window with no decoration
object, since this design has no decoration). The corner shader reads each
animated variable's current eased value every time the hook fires. When
adjacency recompute changes a corner's target (base ↔ `seam_radius`), the
plugin retargets the animated variable; Hyprland's animation manager
handles easing and schedules the repeated repaints (and therefore repeated
hook firings) needed to animate it, using the curve/speed named by
`plugin:seam:animation_curve` / `plugin:seam:animation_speed`, or snapping
immediately if `plugin:seam:animate = false`.

## Known Risks

- **Hooks internal, version-unstable Hyprland functions.** `PluginAPI.hpp`
  is explicit that `createFunctionHook` targets are "not guaranteed any
  API stability" — unlike the public config/window-rule/decoration APIs
  used elsewhere in this plugin. This plugin is pinned to the Hyprland
  0.56.2 internal ABI it was built against; a future Hyprland release that
  changes the hooked function's signature or removes it will require a
  matching plugin update, not just a recompile. This is a deliberate,
  accepted trade-off: the alternative (a pure overlay decoration) cannot
  correctly handle a dynamic backdrop at all, which is a worse failure
  mode than "needs an update after a Hyprland upgrade."

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
  live, open/close windows rapidly, **a live/video wallpaper visible
  correctly through a flattened corner with no stale or wrong texture**,
  and the seam/base-radius transition visibly easing rather than snapping
  when `plugin:seam:animate = true`.
- A small headless unit test (plain C++, no Hyprland runtime) for the pure
  adjacency-detection function, since that logic is decoupled from
  rendering and easy to test in isolation with fabricated window boxes.

## Out of Scope / Deferred

- Border-pass and shadow-pass per-corner rounding (both features disabled
  in current config; same hook-and-redraw pattern as the content pass when
  built).
- Blur-aware corner compositing (blur currently disabled; same hook site
  already has the blurred texture in scope when built).
