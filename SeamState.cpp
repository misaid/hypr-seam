#include "SeamState.hpp"
#include "Adjacency.hpp"
#include "SeamConfig.hpp"
#include "SeamRuleStore.hpp"
#include "globals.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

#include <unordered_map>
#include <sstream>
#include <functional>
#include <cstddef>

// NOTE on currentGlobalDefaults(): globals.hpp (Task 4) already declares this
// non-static, so we include it directly rather than re-declaring it here.

namespace {

// PHLWINDOW is SP<Desktop::View::CWindow> (Hyprutils::Memory::CSharedPointer<T>).
// Confirmed against the installed <hyprutils/memory/SharedPtr.hpp>: it has both
// `operator==` (compares the shared control-block pointer) and a
// `std::hash<CSharedPointer<T>>` specialization (hashes that same pointer), so it's
// directly usable as an unordered_map key out of the box — no raw-pointer key or
// custom comparator needed.
struct SWindowEntry {
    PHLANIMVAR<float> topLeft, topRight, bottomLeft, bottomRight;
    bool              wasTouching[4] = {false, false, false, false}; // TL, TR, BL, BR
};

// g_entries/g_lastFingerprint/g_pendingCloseRecompute below are plain unsynchronized
// module state: safe only because Hyprland always dispatches plugin event-bus
// callbacks (and `tick`) from its single main thread, never concurrently.
std::unordered_map<PHLWINDOW, SWindowEntry> g_entries;

// Fingerprint of every mapped window's position/size/floating state, used by
// onTick() to skip recomputeAll() when nothing that could affect adjacency has
// actually changed since the last tick.
std::size_t g_lastFingerprint = 0;

// Sequence id of a pending deferred (doLater) recompute scheduled by
// onWindowClosed(), or 0 if none. Tracked so clear() (called on plugin unload)
// can cancel it — an idle callback still queued after the .so is unmapped
// would jump into freed code.
uint64_t g_pendingCloseRecompute = 0;

// Builds our own animation property config directly from plugin:seam:* values,
// rather than looking one up by name in Hyprland's global animation tree
// (Config::animationTree()->getAnimationPropertyConfig(name)). That tree only
// contains nodes Hyprland itself registers for its own animations (e.g. "windows",
// "border", "fade", ...) — "default" (our animation_curve default) is a *bezier
// curve* name, not a tree node name, so looking it up as a node would miss and
// hand back a dangling/empty config. Building our own SAnimationPropertyConfig
// with `overridden = true` sidesteps the tree/inheritance machinery entirely and
// just uses our own bezier name + speed directly.
//
// This MUST have static lifetime: CBaseAnimatedVariable only keeps a *weak*
// pointer to whatever config it's given (see m_pConfig in the installed
// <hyprutils/animation/AnimatedVariable.hpp>), and AnimationConfig.hpp's own
// doc comment says config properties "need to have a static lifetime to allow
// for config reload." A per-window local (as this used to be) is destroyed the
// instant onWindowOpened() returns, so every animated var's config weak
// pointer would dangle immediately — the animation system then can't resolve
// enabled()/getBezierName()/getCurveStep() and silently falls back to warping
// every retarget, regardless of plugin:seam:animate.
//
// Also sets `pValues` to reference itself: per CAnimationConfigTree::
// createNode's doc comment, "If parent is empty, a root node will be created
// that references its own values" — value accessors read through pValues, so
// a standalone config built outside that tree (as ours is) needs that
// self-reference wired by hand to expose its bezier/speed at all.
SP<Hyprutils::Animation::SAnimationPropertyConfig>& seamAnimationConfig() {
    static SP<Hyprutils::Animation::SAnimationPropertyConfig> config;
    if (!config) {
        config         = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
        config->pValues = config;
    }
    return config;
}

// Refreshes the shared animation config's fields from the live plugin:seam:*
// config values. Called on every recomputeAll() (and so, transitively, on
// every config reload / live keyword change that triggers one) rather than
// only once, so a live `hyprctl keyword plugin:seam:animation_curve ...` (or
// an edited + reloaded config file) takes effect without needing its own
// special-cased refresh path.
void refreshAnimationConfig() {
    static auto PCURVE = CConfigValue<Config::STRING>("plugin:seam:animation_curve");
    static auto PSPEED = CConfigValue<Config::FLOAT>("plugin:seam:animation_speed");

    auto& cfg            = seamAnimationConfig();
    cfg->overridden      = true;
    cfg->internalBezier  = *PCURVE;
    // plugin:seam:animation_speed is documented (Task 4) as a duration in ms,
    // but Hyprland's native animation speed unit is hundreds of milliseconds
    // (i.e. 1.0 == 100ms) — confirmed during review — so convert here rather
    // than handing the raw ms value to the animation engine (which would turn
    // a configured 300ms into a ~30s transition).
    cfg->internalSpeed   = *PSPEED / 100.0F;
    cfg->internalEnabled = 1;
}

void retarget(PHLANIMVAR<float>& anim, double target) {
    static auto PANIMATE = CConfigValue<Config::INTEGER>("plugin:seam:animate");
    if (!*PANIMATE) {
        anim->setValueAndWarp(sc<float>(target));
        return;
    }
    *anim = sc<float>(target);
}

// Collects every currently-visible mapped window (including ones on special
// workspaces, as long as that special workspace is actually toggled open) as
// adjacency boxes, in lockstep with parallel lists of the windows themselves and
// of each window's workspace, so `boxes[i]`, `windows[i]` and `workspaces[i]`
// always refer to the same window.
//
// `w->isHidden()` (checked below) is a *group* visibility flag (m_hidden —
// e.g. a window hidden inside a collapsed window group), not workspace
// visibility: a window on an inactive workspace keeps its normal monitor-space
// coordinates and still passes that check. Without also filtering on
// `w->m_workspace->isVisible()`, a window on workspace 2 could get flattened
// against a window on workspace 1 just because their stale monitor-space
// coordinates happen to overlap — confirmed during review as a real bug in an
// earlier version of this function, caught only because a single-workspace
// manual test can't exercise it.
//
// Fullscreen/maximized: when a workspace has a covering fullscreen or maximized
// window, the tiled windows it covers are still mapped and "visible" by every
// flag above, but they're hidden behind it — and their edges sit exactly where the
// covering window's screen-edge corners are, so they'd falsely flag those corners
// as touching. So on such a workspace only the covering window itself (plus any
// floating windows, which never act as neighbors and never flatten — kept only so
// their base radii still get retargeted on config changes) participates. The
// covered tiled windows are left out entirely: they aren't on screen, and the
// window.fullscreen event re-runs recomputeAll() as soon as they're uncovered.
//
// `geomType` lets callers choose GEOMETRIC_CURRENT (the live, animated
// position) or GEOMETRIC_GOAL (the animation target — what both recomputeAll()
// and onTick()'s cheap dirty-check use; see recomputeAll()).
void collectVisibleWindows(std::vector<SSeamBox>& boxes, std::vector<PHLWINDOW>& windows, std::vector<const CWorkspace*>& workspaces,
                           Desktop::View::IGeometric::eGeometricValueType geomType = Desktop::View::IGeometric::GEOMETRIC_CURRENT) {
    using Desktop::View::IGeometric;

    for (auto& w : Desktop::windowState()->windows()) {
        if (!validMapped(w) || w->isHidden())
            continue;

        if (!w->m_workspace || !w->m_workspace->isVisible())
            continue;

        if (!w->m_isFloating) {
            const auto FSWINDOW = Fullscreen::controller()->getFullscreenWindow(w->m_workspace);
            if (FSWINDOW && FSWINDOW != w)
                continue; // tiled window covered by this workspace's fullscreen/maximized window
        }

        const auto pos  = w->position(geomType);
        const auto size = w->size(geomType);
        const int  id   = sc<int>(windows.size());

        windows.push_back(w);
        workspaces.push_back(w->m_workspace.get());
        boxes.push_back(SSeamBox{pos.x, pos.y, size.x, size.y, w->m_isFloating, id});
    }
}

// Cheap fingerprint of everything that can affect adjacency output, so onTick()
// can skip the full O(n^2) recompute on ticks where nothing moved.
std::size_t fingerprintVisibleWindows(const std::vector<SSeamBox>& boxes, const std::vector<const CWorkspace*>& workspaces) {
    std::size_t hash = boxes.size();
    auto        mix  = [&hash](std::size_t v) { hash ^= v + 0x9e3779b97f4a7c15ULL /* splitmix64-style mix constant */ + (hash << 6) + (hash >> 2); };
    for (size_t i = 0; i < boxes.size(); ++i) {
        const auto& b = boxes[i];
        mix(std::hash<double>{}(b.x));
        mix(std::hash<double>{}(b.y));
        mix(std::hash<double>{}(b.w));
        mix(std::hash<double>{}(b.h));
        mix(std::hash<bool>{}(b.floating));
        mix(std::hash<const void*>{}(workspaces[i]));
    }
    return hash;
}

// Seam is off for this window (globally, by rule, or because it's floating):
// use plain per-corner base radii and drop any stale hysteresis state so a
// later re-enable (e.g. a config reload or rule change) starts clean.
void retargetDisabled(SWindowEntry& entry, const SResolvedWindowConfig& resolved, const SSeamBox& subjectBox) {
    entry.wasTouching[0] = entry.wasTouching[1] = entry.wasTouching[2] = entry.wasTouching[3] = false;

    retarget(entry.topLeft, clampCornerRadius(resolved.radii.topLeft, subjectBox.w, subjectBox.h));
    retarget(entry.topRight, clampCornerRadius(resolved.radii.topRight, subjectBox.w, subjectBox.h));
    retarget(entry.bottomLeft, clampCornerRadius(resolved.radii.bottomLeft, subjectBox.w, subjectBox.h));
    retarget(entry.bottomRight, clampCornerRadius(resolved.radii.bottomRight, subjectBox.w, subjectBox.h));
}

void retargetEnabled(SWindowEntry& entry, const SResolvedWindowConfig& resolved, const SSeamBox& subjectBox, const std::vector<SSeamBox>& pool,
                      const SGlobalSeamDefaults& defaults) {
    const SCornerFlags touching = computeTouchingCorners(subjectBox, pool, defaults.tolerance);

    // Hysteresis: once a corner is flagged as touching, don't drop the flag the
    // instant the normal-tolerance check fails — only drop it once a *widened*
    // tolerance check also fails, i.e. once the gap has clearly grown past the
    // boundary rather than merely crossed it. (The reverse direction — going
    // from unflagged to flagged — always uses the normal tolerance immediately;
    // hysteresis only guards against flicker on the way out.)
    const SCornerFlags widened = computeTouchingCorners(subjectBox, pool, defaults.tolerance * 1.5);

    auto resolveCorner = [&](bool rawTouching, bool widenedTouching, bool& wasTouching, double baseRadius) {
        bool effectiveTouching;
        if (rawTouching)
            effectiveTouching = true;
        else if (wasTouching)
            effectiveTouching = widenedTouching;
        else
            effectiveTouching = false;

        wasTouching = effectiveTouching;
        return effectiveTouching ? defaults.seamRadius : baseRadius;
    };

    const double tl =
        resolveCorner(touching.topLeft, widened.topLeft, entry.wasTouching[0], clampCornerRadius(resolved.radii.topLeft, subjectBox.w, subjectBox.h));
    const double tr =
        resolveCorner(touching.topRight, widened.topRight, entry.wasTouching[1], clampCornerRadius(resolved.radii.topRight, subjectBox.w, subjectBox.h));
    const double bl = resolveCorner(touching.bottomLeft, widened.bottomLeft, entry.wasTouching[2],
                                    clampCornerRadius(resolved.radii.bottomLeft, subjectBox.w, subjectBox.h));
    const double br = resolveCorner(touching.bottomRight, widened.bottomRight, entry.wasTouching[3],
                                    clampCornerRadius(resolved.radii.bottomRight, subjectBox.w, subjectBox.h));

    retarget(entry.topLeft, tl);
    retarget(entry.topRight, tr);
    retarget(entry.bottomLeft, bl);
    retarget(entry.bottomRight, br);
}

} // namespace

void SeamState::onWindowOpened(const PHLWINDOW& window) {
    if (!window || g_entries.contains(window))
        return;

    refreshAnimationConfig();
    auto& config = seamAnimationConfig();

    SWindowEntry entry;
    const float  initial = 0.F;
    Animation::mgr()->createAnimation(initial, entry.topLeft, config, window, AVARDAMAGE_ENTIRE);
    Animation::mgr()->createAnimation(initial, entry.topRight, config, window, AVARDAMAGE_ENTIRE);
    Animation::mgr()->createAnimation(initial, entry.bottomLeft, config, window, AVARDAMAGE_ENTIRE);
    Animation::mgr()->createAnimation(initial, entry.bottomRight, config, window, AVARDAMAGE_ENTIRE);

    g_entries.emplace(window, std::move(entry));
    recomputeAll();
}

void SeamState::onWindowClosed(const PHLWINDOW& window) {
    g_entries.erase(window);

    // The closed window's former neighbors may have just lost a touching corner
    // (or will be reflowed into its space); update them promptly instead of
    // waiting for the tick fallback. Deferred to the next event-loop idle rather
    // than run inline: window.close is emitted at the very start of
    // CWindow::unmapWindow(), while the closing window is still m_isMapped and
    // before the layout has removed it / retargeted its neighbors' goal
    // geometry, so an inline recompute would still see the old layout.
    if (g_pendingCloseRecompute == 0 && g_pEventLoopManager) {
        g_pendingCloseRecompute = g_pEventLoopManager->doLater([] {
            g_pendingCloseRecompute = 0;
            recomputeAll();
        });
    }
}

void SeamState::clear() {
    if (g_pendingCloseRecompute != 0 && g_pEventLoopManager)
        g_pEventLoopManager->removeDoLater(g_pendingCloseRecompute);
    g_pendingCloseRecompute = 0;
    g_entries.clear();
    g_lastFingerprint = 0;
}

std::optional<SeamState::SLiveCorners> SeamState::liveCornersFor(const PHLWINDOW& window) {
    auto it = g_entries.find(window);
    if (it == g_entries.end())
        return std::nullopt;

    return SLiveCorners{it->second.topLeft->value(), it->second.topRight->value(), it->second.bottomLeft->value(), it->second.bottomRight->value()};
}

void SeamState::recomputeAll() {
    refreshAnimationConfig(); // picks up any live curve/speed config change
    const auto defaults = currentGlobalDefaults();

    // Use GOAL geometry here, not the live/animated current position, and not
    // only in onTick()'s fingerprint. Found during fix-round-1 verification:
    // a discrete event (e.g. window.fullscreen) can fire while the window's
    // position/size animation is still mid-flight toward its new layout slot,
    // so a recompute driven by CURRENT geometry at that instant can capture a
    // transient, not-yet-settled box — and since Hyprland's `tick` event is
    // NOT a continuous per-frame heartbeat (confirmed empirically: it stops
    // firing once the compositor goes render-idle, which can happen very
    // shortly after the animation completes), there's no guarantee a later
    // correcting recompute ever runs to fix a wrong hysteresis flag baked in
    // from that transient moment. Using the GOAL geometry instead means every
    // recompute — whenever it happens to fire — always judges adjacency
    // against the final, authoritative layout, never an in-between frame, so
    // there's nothing for a later recompute to need to "correct".
    std::vector<SSeamBox>           boxes;
    std::vector<PHLWINDOW>          windows;
    std::vector<const CWorkspace*> workspaces;
    collectVisibleWindows(boxes, windows, workspaces, Desktop::View::IGeometric::GEOMETRIC_GOAL);

    // Adjacency is only ever judged between windows on the SAME workspace (and so,
    // implicitly, the same monitor). Pooling everything together let a window's
    // screen-edge corner land within tolerance of a window on the neighboring
    // monitor (only gaps_out apart, or less with a raised tolerance), or of a window
    // on a special workspace overlaid on top, and get falsely flattened.
    std::unordered_map<const CWorkspace*, std::vector<SSeamBox>> pools;
    for (size_t i = 0; i < boxes.size(); ++i)
        pools[workspaces[i]].push_back(boxes[i]);

    for (size_t i = 0; i < windows.size(); ++i) {
        auto& w = windows[i];

        auto it = g_entries.find(w);
        if (it == g_entries.end())
            continue; // not tracked yet (e.g. open event hasn't fired); nothing to retarget

        auto&       entry      = it->second;
        const auto& subjectBox = boxes[i];

        auto resolved = resolveWindowConfig(w->m_class, w->m_title, w->m_isFloating, defaults, SeamRuleStore::rules());

        if (!resolved.seamEnabled)
            retargetDisabled(entry, resolved, subjectBox);
        else
            retargetEnabled(entry, resolved, subjectBox, pools[workspaces[i]], defaults);
    }
}

void SeamState::onTick() {
    // Reused across calls rather than allocated fresh every tick — this runs
    // on every compositor tick, so a fresh heap allocation per call here would
    // be a steady, avoidable cost even when nothing changed. (Confirmed during
    // fix-round-1 verification — via a temporary invocation counter, since
    // removed — that `tick` is NOT a continuous per-frame heartbeat: it fires
    // in bursts while the compositor is actively rendering/animating and goes
    // fully silent once idle. recomputeAll() no longer depends on a trailing
    // tick to "catch" a settled layout — see the GOAL-geometry comment there.)
    static std::vector<SSeamBox>           boxes;
    static std::vector<PHLWINDOW>          windows;
    static std::vector<const CWorkspace*> workspaces;
    boxes.clear();
    windows.clear();
    workspaces.clear();

    // Fingerprint the *goal* geometry, not the live/animated current geometry.
    // Using GEOMETRIC_CURRENT here would make the fingerprint change on every
    // single frame of any window-geometry animation in progress (an open
    // pop-in, a slide, even an unrelated border-angle loop elsewhere in
    // Hyprland), forcing a full recomputeAll() — including the O(n^2)
    // adjacency work and hysteresis evaluated against in-between, not-yet-
    // settled positions — every frame for as long as anything is animating.
    // The goal/target geometry only changes once per actual layout change, so
    // fingerprinting that instead keeps this gate cheap and correct.
    collectVisibleWindows(boxes, windows, workspaces, Desktop::View::IGeometric::GEOMETRIC_GOAL);

    // Don't keep strong window refs alive in these reused statics between ticks
    // (they'd pin closed windows, and would outlive a plugin unload).
    windows.clear();

    const std::size_t fingerprint = fingerprintVisibleWindows(boxes, workspaces);
    if (fingerprint == g_lastFingerprint)
        return;

    g_lastFingerprint = fingerprint;
    recomputeAll();
}

std::string SeamState::debugDump() {
    std::ostringstream out;
    out << g_entries.size() << " tracked window(s)\n";
    for (auto& [w, entry] : g_entries) {
        out << "- " << (w ? w->m_class : std::string{"<null>"}) << " [" << (w ? w->m_title.substr(0, 32) : std::string{}) << "]";
        if (w) {
            const auto pos  = w->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);
            const auto size = w->size(Desktop::View::IGeometric::GEOMETRIC_GOAL);
            out << " @" << pos.x << "," << pos.y << " " << size.x << "x" << size.y;
        }
        out << ": "
            << "TL=" << entry.topLeft->value() << (entry.wasTouching[0] ? "*" : "") << " "
            << "TR=" << entry.topRight->value() << (entry.wasTouching[1] ? "*" : "") << " "
            << "BL=" << entry.bottomLeft->value() << (entry.wasTouching[2] ? "*" : "") << " "
            << "BR=" << entry.bottomRight->value() << (entry.wasTouching[3] ? "*" : "") << "\n";
    }
    return out.str();
}
