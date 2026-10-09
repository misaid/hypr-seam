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

namespace {

// PHLWINDOW is SP<Desktop::View::CWindow> (Hyprutils::Memory::CSharedPointer<T>).
// <hyprutils/memory/SharedPtr.hpp> gives it an `operator==` and a
// `std::hash<CSharedPointer<T>>` specialization, both based on the control-block
// pointer, so it works directly as an unordered_map key.
struct SWindowEntry {
    PHLANIMVAR<float> topLeft, topRight, bottomLeft, bottomRight;
    bool              wasTouching[4] = {false, false, false, false}; // TL, TR, BL, BR
};

// g_entries, g_lastFingerprint and g_pendingCloseRecompute are unsynchronized. That's
// safe because Hyprland runs every event-bus callback, including `tick`, on its main
// thread.
std::unordered_map<PHLWINDOW, SWindowEntry> g_entries;

// Fingerprint of every mapped window's position, size and floating state. onTick()
// uses it to skip recomputeAll() when nothing that affects adjacency has changed.
std::size_t g_lastFingerprint = 0;

// Sequence id of the doLater recompute scheduled by onWindowClosed(), or 0 if none.
// clear() cancels it on plugin unload, since a callback still queued after the .so
// is unmapped would jump into freed code.
uint64_t g_pendingCloseRecompute = 0;

// Builds the plugin's own animation property config from plugin:seam:* values.
// Looking one up in Hyprland's animation tree
// (Config::animationTree()->getAnimationPropertyConfig(name)) won't work: the tree
// only has nodes Hyprland registers for its own animations ("windows", "border",
// "fade", ...), and "default", the animation_curve default, is a bezier curve name.
// Looking it up as a node returns an empty config. With `overridden = true`, this
// config skips the tree's inheritance and uses our bezier name and speed.
//
// The config must have static lifetime. CBaseAnimatedVariable keeps only a weak
// pointer to its config (m_pConfig in <hyprutils/animation/AnimatedVariable.hpp>),
// and AnimationConfig.hpp says config properties "need to have a static lifetime
// to allow for config reload." A local in onWindowOpened() would leave every
// animated var's weak pointer dangling. The animation system then can't resolve
// enabled()/getBezierName()/getCurveStep() and warps every retarget, whatever
// plugin:seam:animate says.
//
// `pValues` points back at the config itself. CAnimationConfigTree::createNode's
// doc comment says "If parent is empty, a root node will be created that
// references its own values", and the value accessors read through pValues, so a
// config built outside the tree has to set that up by hand.
SP<Hyprutils::Animation::SAnimationPropertyConfig>& seamAnimationConfig() {
    static SP<Hyprutils::Animation::SAnimationPropertyConfig> config;
    if (!config) {
        config         = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
        config->pValues = config;
    }
    return config;
}

// Copies the live plugin:seam:* values into the shared animation config. Every
// recomputeAll() calls it, so an edited and reloaded animation_curve or
// animation_speed takes effect. A bare `hyprctl keyword` doesn't trigger a
// recompute; see registerEventListeners() in main.cpp.
void refreshAnimationConfig() {
    static auto PCURVE = CConfigValue<Config::STRING>("plugin:seam:animation_curve");
    static auto PSPEED = CConfigValue<Config::FLOAT>("plugin:seam:animation_speed");

    auto& cfg            = seamAnimationConfig();
    cfg->overridden      = true;
    cfg->internalBezier  = *PCURVE;
    // plugin:seam:animation_speed is in ms, but Hyprland's animation speed unit is
    // 100ms (1.0 == 100ms). Passing the raw value would turn 300ms into about 30s.
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

// Collects every visible mapped window as an adjacency box, including windows on a
// special workspace that is toggled open. The three output lists stay in step:
// `boxes[i]`, `windows[i]` and `workspaces[i]` describe the same window.
//
// `w->isHidden()` reads m_hidden, which is group visibility (a window hidden in a
// collapsed group). A window on an inactive workspace keeps its monitor-space
// coordinates and passes that check, so `w->m_workspace->isVisible()` is checked
// too. Without it, a window on workspace 2 could be flattened against one on
// workspace 1 because their coordinates overlap.
//
// When a workspace has a fullscreen or maximized window, the tiled windows behind
// it still pass every check above. Their edges line up with the covering window's
// screen-edge corners and would flag those corners as touching. On such a
// workspace only the covering window and any floating windows are collected.
// Floating windows never act as neighbors or flatten; they're kept so their base
// radii still update on config changes. The covered windows aren't on screen, and
// the window.fullscreen event runs recomputeAll() again once they're uncovered.
//
// `geomType` picks GEOMETRIC_CURRENT (the live, animated position) or
// GEOMETRIC_GOAL (the animation target). recomputeAll() and onTick() both use
// GOAL; see recomputeAll().
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

// Cheap hash of everything that affects adjacency, so onTick() can skip the
// O(n^2) recompute on ticks where nothing moved.
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

// Seam is off for this window (globally, by rule, or because it floats). Uses the
// base radii and clears hysteresis state, so turning seam back on later (by config
// reload or rule change) starts clean.
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

    // Hysteresis: a corner that is already flagged stays flagged until the check
    // with 1.5x tolerance also fails, so it doesn't flicker when the gap sits right
    // at the boundary. Becoming flagged uses the normal tolerance.
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

    // The closed window's neighbors may have lost a touching corner, or will be
    // reflowed into its space, so update them now instead of waiting for a tick.
    // The recompute runs at the next event-loop idle. window.close fires at the
    // start of CWindow::unmapWindow(), while the window is still m_isMapped and
    // before the layout has removed it and moved its neighbors' goal geometry, so
    // an inline recompute would see the old layout.
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

    // Use GOAL geometry, not the live animated position. An event such as
    // window.fullscreen can fire while a window is still animating toward its new
    // slot, and CURRENT geometry at that moment is an in-between box. `tick` stops
    // firing soon after the compositor goes idle, so a later recompute may never
    // run to fix a hysteresis flag set from that box. GOAL geometry is the final
    // layout no matter when the recompute fires.
    std::vector<SSeamBox>           boxes;
    std::vector<PHLWINDOW>          windows;
    std::vector<const CWorkspace*> workspaces;
    collectVisibleWindows(boxes, windows, workspaces, Desktop::View::IGeometric::GEOMETRIC_GOAL);

    // Adjacency is judged only between windows on the same workspace, and so the
    // same monitor. Otherwise a screen-edge corner can land within tolerance of a
    // window on the next monitor (only gaps_out away) or on a special workspace
    // shown on top, and get flattened.
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
    // Static so the vectors are reused across ticks. This runs on every tick, and
    // allocating each time would cost something even when nothing changed.
    static std::vector<SSeamBox>           boxes;
    static std::vector<PHLWINDOW>          windows;
    static std::vector<const CWorkspace*> workspaces;
    boxes.clear();
    windows.clear();
    workspaces.clear();

    // Fingerprint the GOAL geometry. CURRENT geometry changes on every frame of a
    // window animation (open, slide, resize), which would force the O(n^2)
    // recompute every frame and judge hysteresis against in-between positions.
    // GOAL geometry changes once per layout change.
    collectVisibleWindows(boxes, windows, workspaces, Desktop::View::IGeometric::GEOMETRIC_GOAL);

    // Don't hold strong window refs in the statics between ticks. They would keep
    // closed windows alive and outlive a plugin unload.
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
