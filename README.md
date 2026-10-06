# hypr-seam

A Hyprland plugin for independent per-corner rounding, plus an optional
seam effect: the corner where two tiled windows touch flattens out instead
of staying round, so your layout reads as one continuous shape instead of
a grid of separate rounded boxes. T-junctions and X-junctions, where three
or four windows meet at a point, flatten the same way across every window
involved. Both base rounding and the seam effect can be set per app, so
your terminal can stay sharp-cornered while everything else gets rounded.

## Demo

![hypr-seam corner geometry spec, shown next to its own source note](assets/demo.png)

## Requirements

- Hyprland, currently built and tested against the 0.56.2 internal ABI.
  The plugin hooks an internal, version-unstable Hyprland function
  (`createFunctionHook`, the same facility other community plugins such as
  `hy3` use), so a Hyprland upgrade that changes that function's signature
  will need a matching plugin update, not just a rebuild.
- `decoration:rounding = 0` set globally in your Hyprland config. This
  plugin fully replaces native corner rendering, scissoring out each
  corner before Hyprland's own paint call and redrawing it itself, so
  leaving native rounding on would just double up with (or fight) this
  plugin's own rounding.

## Installation

### Via hyprpm

The repo ships an `hyprpm.toml` manifest, so it can be added directly:

```sh
hyprpm add https://github.com/misaid/hypr-seam
hyprpm enable hypr-seam
```

`hyprpm` builds the plugin using the manifest's build script (`make all`,
then the resulting `build/libhypr-seam.so` is copied to `hypr-seam.so`).

### Manual build

The repo also has a plain `Makefile` wrapping a `meson`/`ninja` build, for
building outside of `hyprpm`:

```sh
git clone https://github.com/misaid/hypr-seam
cd hypr-seam
make all
```

This produces `build/libhypr-seam.so`. Load it for the running Hyprland
session with:

```sh
hyprctl plugin load "$(pwd)/build/libhypr-seam.so"
```

or add the same path to your Hyprland config's `plugin =` line to load it
on every start.

## Configuration

### Global options

All values live under the `plugin:seam:` prefix.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `plugin:seam:rounding` | int | `22` | Uniform base radius used for any corner without its own `rounding_*` override. |
| `plugin:seam:rounding_topleft` | int | = `rounding` | Base top-left corner radius. |
| `plugin:seam:rounding_topright` | int | = `rounding` | Base top-right corner radius. |
| `plugin:seam:rounding_bottomleft` | int | = `rounding` | Base bottom-left corner radius. |
| `plugin:seam:rounding_bottomright` | int | = `rounding` | Base bottom-right corner radius. |
| `plugin:seam:rounding_power` | float | `2.0` | Squircle exponent for the corner curve, matching Hyprland's native default look. |
| `plugin:seam:enabled` | bool | `false` | Global master switch for the seam effect. |
| `plugin:seam:seam_radius` | int | `2` | Radius a corner collapses to once it's flagged as touching a neighboring tiled window. |
| `plugin:seam:tolerance` | int (px) | `6` | Maximum gap between two window edges that still counts as "touching." Covers `gaps_in` plus floating-point/scale slop. |
| `plugin:seam:animate` | bool | `true` | Eases a corner's radius between its base and seam values instead of snapping. |
| `plugin:seam:animation_speed` | float (ms) | `300` | Duration of that easing transition. |
| `plugin:seam:animation_curve` | string | `"default"` | Name of a bezier curve already registered via Hyprland's `bezier =` (or `hl.curve(...)`). |
| `plugin:seam:force_round_risky_surfaces` | bool | `false` | Also round a window's subsurfaces where they reach the window's own corners. Turn this on if Firefox-based browsers (Firefox, Zen, ...) keep square corners. See below. |

Base corner radii are resolved per window as: a matching `seamrule rounding`
override if one exists, otherwise the four global `rounding_*` values. The
seam flag resolves the same way: a matching `seamrule seam` override if one
exists, otherwise `plugin:seam:enabled`. Floating windows skip seam
resolution entirely and always render with base rounding only, regardless
of what the seam flag would otherwise resolve to.

### Example

A plain hyprlang `.conf` snippet covering the common options:

```
plugin:seam:enabled = true
plugin:seam:rounding = 12
plugin:seam:seam_radius = 2
plugin:seam:tolerance = 6
plugin:seam:animate = true
plugin:seam:animation_speed = 300

seamrule = rounding 4 4 22 22, class:^(kitty)$
seamrule = seam 0, class:^(foot)$
```

### Per-app rules (`seamrule`)

Hyprland's plugin API has no hook into the native `windowrulev2` engine, so
`hypr-seam` parses its own keyword, `seamrule`, using the same
`class:`/`title:` match syntax as `windowrulev2`. This works for a plain
hyprlang `.conf`-syntax config:

```
seamrule = rounding <tl> <tr> <bl> <br>, <match>
seamrule = seam <0|1>, <match>
```

Examples:

```
seamrule = rounding 4 4 22 22, class:^(kitty)$
seamrule = seam 1, class:^(mpv)$
seamrule = seam 0, class:^(foot)$   # opt out even if plugin:seam:enabled = true
```

If your config is written in Hyprland's Lua config DSL instead (no plain
`.conf` file for `seamrule` to live in), use `hl.plugin.seam.rule(...)`,
which accepts either the exact same string a `seamrule` line would take, or
a table:

```lua
hl.plugin.seam.rule("seam 0, class:^(foot)$")
hl.plugin.seam.rule({ class = "^(kitty)$", rounding = { 4, 4, 22, 22 } })
hl.plugin.seam.rule({ title = "^Picture-in-Picture$", seam = false })
```

A malformed rule (either form) shows up as a config error with a file/line
reference, the same way Hyprland reports its own config mistakes, rather
than silently doing nothing.

## Scope and limitations

- hypr-seam only rounds a window's main surface by default, not its
  subsurfaces. Any window that renders through a subsurface covering the
  whole main surface, which is how GPU-accelerated apps such as
  Firefox-based browsers (Firefox, Zen, and others) draw, will keep square
  corners: the rounding is there, just hidden underneath. If you hit this,
  set `plugin:seam:force_round_risky_surfaces = true` to also round
  subsurfaces where they reach a window's corners. A subsurface that
  doesn't reach a corner, an embedded video for example, is never touched
  either way. This is off by default because it changes how other apps'
  subsurfaces get drawn, and it has only been tested against Firefox.
  During a resize animation the subsurface can briefly lag the window box,
  so its corners may show square for a few frames.
