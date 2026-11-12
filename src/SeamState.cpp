#include "SeamState.hpp"
#include "Adjacency.hpp"
#include "RecomputeGate.hpp"
#include "SeamConfig.hpp"
#include "SeamRuleStore.hpp"
#include "globals.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

#include <limits>
#include <unordered_map>
#include <sstream>
#include <functional>
#include <cstddef>

static_assert(kMaxWorkspaceId == std::numeric_limits<Workspace::WorkspaceIDContainer>::max(),
              "kMaxWorkspaceId (SeamConfig.hpp) no longer matches Hyprland's workspace ID type");

namespace {

// Keyed by PHLWINDOW: hyprutils hashes and compares shared pointers by control block.
struct SWindowEntry {
    PHLANIMVAR<float> topLeft, topRight, bottomLeft, bottomRight;
    bool              wasTouching[4] = {false, false, false, false}; // TL, TR, BL, BR
};

// Unsynchronized: every event-bus callback, including `tick`, runs on Hyprland's main thread.
std::unordered_map<PHLWINDOW, SWindowEntry> g_entries;

// Last tick's geometry fingerprint and defaults, so onTick() can skip unchanged ticks.
CRecomputeGate g_recomputeGate;

// doLater id of the recompute queued by onWindowClosed(), or 0. clear() cancels it so no
// callback runs after the plugin is unloaded.
uint64_t g_pendingCloseRecompute = 0;

// The plugin's own animation config. Hyprland's animation tree has no node for it, so it
// is built standalone with `overridden = true`.
//
// - Static lifetime: animated vars hold only a weak pointer to their config; a dangling
//   one makes every retarget warp instead of animate.
// - `pValues` must point at itself, as the tree does for a root node, because the value
//   accessors read through it.
SP<Hyprutils::Animation::SAnimationPropertyConfig>& seamAnimationConfig() {
    static SP<Hyprutils::Animation::SAnimationPropertyConfig> config;
    if (!config) {
        config         = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
        config->pValues = config;
    }
    return config;
}

// Copies the live animation_curve/animation_speed into the shared config. Called before
// every recompute, so changes apply from the next animation however they were made.
void refreshAnimationConfig() {
    static auto PCURVE = CConfigValue<Config::STRING>("plugin:seam:animation_curve");
    static auto PSPEED = CConfigValue<Config::FLOAT>("plugin:seam:animation_speed");

    auto& cfg            = seamAnimationConfig();
    cfg->overridden      = true;
    cfg->internalBezier  = *PCURVE;
    // animation_speed is in ms; Hyprland's unit is 100ms.
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

// Collects every visible mapped window as an adjacency box. `boxes[i]`, `windows[i]` and
// `workspaces[i]` describe the same window.
//
// - isHidden() only covers collapsed groups; windows on inactive workspaces keep their
//   coordinates, so workspace visibility is checked separately.
// - Tiled windows behind a fullscreen/maximized window are skipped, or they would flag
//   its screen-edge corners as touching. window.fullscreen recomputes on uncover.
// - Floating windows are kept so their base radii track config, but never flatten.
void collectVisibleWindows(std::vector<SSeamBox>& boxes, std::vector<PHLWINDOW>& windows, std::vector<const Workspace::CHLWorkspace*>& workspaces,
                           Desktop::View::IGeometric::eGeometricValueType geomType = Desktop::View::IGeometric::GEOMETRIC_CURRENT) {
    using Desktop::View::IGeometric;

    for (auto& w : Desktop::windowState()->windows()) {
        if (!validMapped(w) || w->isHidden())
            continue;

        if (!w->m_workspace || !w->m_workspace->visible())
            continue;

        if (!w->isFloating()) {
            const auto FSWINDOW = Fullscreen::controller()->getFullscreenWindow(w->m_workspace);
            if (FSWINDOW && FSWINDOW != w)
                continue; // tiled window covered by this workspace's fullscreen/maximized window
        }

        const auto pos  = w->position(geomType);
        const auto size = w->size(geomType);
        const int  id   = sc<int>(windows.size());

        windows.push_back(w);
        workspaces.push_back(w->m_workspace.get());
        boxes.push_back(SSeamBox{pos.x, pos.y, size.x, size.y, w->isFloating(), id});
    }
}

// Hash of everything adjacency depends on, so onTick() can skip the O(n^2) recompute.
std::size_t fingerprintVisibleWindows(const std::vector<SSeamBox>& boxes, const std::vector<const Workspace::CHLWorkspace*>& workspaces) {
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

// Seam is off for this window: use the base radii and clear hysteresis so re-enabling
// starts clean.
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

    // Hysteresis: a flagged corner stays flagged until it also fails at 1.5x tolerance,
    // so a gap right at the boundary doesn't flicker.
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

    // Deferred to the next idle: window.close fires before the layout reflows the
    // neighbors, so an inline recompute would see the old geometry.
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
    g_recomputeGate.reset();
}

std::optional<SeamState::SLiveCorners> SeamState::liveCornersFor(const PHLWINDOW& window) {
    auto it = g_entries.find(window);
    if (it == g_entries.end())
        return std::nullopt;

    return SLiveCorners{it->second.topLeft->value(), it->second.topRight->value(), it->second.bottomLeft->value(), it->second.bottomRight->value()};
}

// Shared body of recomputeAll() and recomputeWindow(). With `only` set, just that window
// is retargeted, but every window is still collected since adjacency needs its neighbors.
static void recompute(const PHLWINDOW& only) {
    refreshAnimationConfig();
    const auto defaults = currentGlobalDefaults();

    // GOAL geometry: events can fire mid-animation, and `tick` stops when idle, so a flag
    // set from an in-between CURRENT box might never be corrected.
    std::vector<SSeamBox>           boxes;
    std::vector<PHLWINDOW>          windows;
    std::vector<const Workspace::CHLWorkspace*> workspaces;
    collectVisibleWindows(boxes, windows, workspaces, Desktop::View::IGeometric::GEOMETRIC_GOAL);

    // Adjacency is per workspace, so windows on another monitor or an overlaid special
    // workspace never flatten each other.
    std::unordered_map<const Workspace::CHLWorkspace*, std::vector<SSeamBox>> pools;
    for (size_t i = 0; i < boxes.size(); ++i)
        pools[workspaces[i]].push_back(boxes[i]);

    for (size_t i = 0; i < windows.size(); ++i) {
        auto& w = windows[i];
        if (only && w != only)
            continue;

        auto it = g_entries.find(w);
        if (it == g_entries.end())
            continue; // open event hasn't fired yet

        auto&       entry      = it->second;
        const auto& subjectBox = boxes[i];

        // Non-null: collectVisibleWindows() skips windows without a workspace.
        const auto& ws       = *workspaces[i];
        const auto  numbered = ws.numberedID();
        const auto  resolved = resolveWindowConfig(
            w->metadata().appID(), w->metadata().title(), w->isFloating(), defaults, SeamRuleStore::rules(),
            SWorkspaceRef{.id          = numbered ? std::optional<std::int64_t>{*numbered} : std::nullopt,
                          .address     = ws.addressableName(),
                          .displayName = ws.displayName()});

        if (!resolved.seamEnabled)
            retargetDisabled(entry, resolved, subjectBox);
        else
            retargetEnabled(entry, resolved, subjectBox, pools[workspaces[i]], defaults);
    }
}

void SeamState::recomputeAll() {
    recompute(nullptr);
}

void SeamState::recomputeWindow(const PHLWINDOW& window) {
    if (window)
        recompute(window);
}

void SeamState::onTick() {
    // Static to reuse the allocations; this runs every frame while anything animates.
    static std::vector<SSeamBox>           boxes;
    static std::vector<PHLWINDOW>          windows;
    static std::vector<const Workspace::CHLWorkspace*> workspaces;
    boxes.clear();
    windows.clear();
    workspaces.clear();

    // GOAL geometry changes once per layout change; CURRENT changes every animation frame.
    collectVisibleWindows(boxes, windows, workspaces, Desktop::View::IGeometric::GEOMETRIC_GOAL);

    // Drop strong refs so closed windows aren't kept alive past this tick or an unload.
    windows.clear();

    // Defaults are compared too: `hyprctl keyword` changes fire no event.
    if (!g_recomputeGate.changed(fingerprintVisibleWindows(boxes, workspaces), currentGlobalDefaults()))
        return;

    recomputeAll();
}

std::string SeamState::debugDump() {
    std::ostringstream out;
    out << g_entries.size() << " tracked window(s)\n";
    for (auto& [w, entry] : g_entries) {
        out << "- " << (w ? w->metadata().appID() : std::string{"<null>"}) << " [" << (w ? w->metadata().title().substr(0, 32) : std::string{}) << "]";
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
