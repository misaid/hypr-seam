-- hypr-seam example config (Lua)
--
-- This is a copy-paste starting point, not something to use verbatim. It
-- shows every `plugin:seam:*` value hypr-seam registers, set explicitly to
-- its real built-in default, with a comment explaining what it does. Most
-- users will only want to override a handful of these (commonly just
-- `enabled`, and maybe `rounding`) — delete the rest once you've picked your
-- values, or leave them as documentation of what's tunable.
--
-- Requires `decoration.rounding = 0` in your Hyprland config: this plugin
-- fully replaces native corner rendering, so leaving native rounding on
-- will double up with (or fight) hypr-seam's own rounding.

hl.config({
    plugin = {
        seam = {
            -- Uniform base corner radius used for any corner that doesn't
            -- have its own rounding_* override below. This is the radius
            -- windows render at when the seam effect isn't flattening them.
            rounding = 22,

            -- Per-corner base radius overrides. Each one falls back to
            -- `rounding` above when left unset, so you only need to set the
            -- corners you want to differ. Commented out here since "unset"
            -- has no literal Lua value — uncomment and set a number to
            -- override a specific corner.
            -- rounding_topleft = 22,
            -- rounding_topright = 22,
            -- rounding_bottomleft = 22,
            -- rounding_bottomright = 22,

            -- Squircle exponent for the corner curve shape. 2.0 matches a
            -- regular rounded-rectangle curve (Hyprland's native look).
            -- Raise it for a more squared-off "squircle" corner.
            rounding_power = 2.0,

            -- Global master switch for the seam effect. When false, windows
            -- just render with ordinary per-corner rounding and nothing
            -- ever flattens, regardless of adjacency.
            enabled = false,

            -- Radius a corner collapses to once it's flagged as touching a
            -- neighboring tiled window (a book-seam corner instead of a
            -- fully rounded one).
            seam_radius = 2,

            -- Maximum gap, in pixels, between two window edges that still
            -- counts as "touching" for seam purposes. -1 (the default)
            -- derives this from general.gaps_in automatically; set an
            -- explicit value only if the auto default doesn't match your
            -- layout.
            tolerance = -1,

            -- Eases a corner's radius between its base and seam values
            -- instead of snapping instantly when adjacency changes.
            animate = true,

            -- Duration, in milliseconds, of that easing transition.
            animation_speed = 300,

            -- Name of a bezier curve already registered via hl.curve(...)
            -- (or a `bezier =` line in a plain .conf config) to drive the
            -- easing above. "default" is Hyprland's built-in curve.
            animation_curve = "default",

            -- Also round a window's subsurfaces where they reach the
            -- window's own corners. Turn this on if Firefox-based browsers
            -- (Firefox, Zen, ...) keep square corners: they draw the whole
            -- window into a subsurface that hides the rounded main surface.
            -- Off by default because it affects how every app's
            -- subsurfaces are drawn.
            force_round_risky_surfaces = false,

            -- Off by default. Round the native border to match this
            -- window's live corner radii (including seam flattening).
            -- Only visible with general.border_size > 0.
            round_borders = false,

            -- Off by default. Round the native drop shadow the same way.
            -- Only visible with decoration.shadow.enabled = true.
            round_shadows = false,
        },
    },
})

-- Per-app rules (hl.plugin.seam.rule)
--
-- Hyprland's plugin API has no hook into the native windowrulev2 engine, so
-- hypr-seam parses its own rules using the same class:/title: match syntax
-- as windowrule2. hl.plugin.seam.rule(...) accepts either the exact string
-- a `seamrule = ...` config line would take, or an equivalent table.

-- String form: opt a window out of the seam effect even if
-- plugin:seam:enabled is true above.
hl.plugin.seam.rule("seam 0, class:^(foot)$")

-- Table form: give a specific app its own per-corner base rounding
-- (top-left, top-right, bottom-left, bottom-right), independent of the
-- global `rounding`/`rounding_*` values set above.
hl.plugin.seam.rule({ class = "^(kitty)$", rounding = { 4, 4, 22, 22 } })

-- Table form: force a window to never flatten (seam = false), useful for
-- floating-like windows such as a picture-in-picture player that shouldn't
-- visually merge with whatever tiled window it happens to sit next to.
hl.plugin.seam.rule({ title = "^Picture-in-Picture$", seam = false })
