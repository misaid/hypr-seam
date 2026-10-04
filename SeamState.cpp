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

std::unordered_map<PHLWINDOW, SWindowEntry> g_entries;

// Fingerprint of every mapped window's position/size/floating state, used by
// onTick() to skip recomputeAll() when nothing that could affect adjacency has
// actually changed since the last tick.
std::size_t g_lastFingerprint = 0;

// Builds our own animation property config directly from plugin:seam:* values,
// rather than looking one up by name in Hyprland's global animation tree
// (Config::animationTree()->getAnimationPropertyConfig(name)). That tree only
// contains nodes Hyprland itself registers for its own animations (e.g. "windows",
// "border", "fade", ...) — "default" (our animation_curve default) is a *bezier
// curve* name, not a tree node name, so looking it up as a node would miss and
// hand back a dangling/empty config. Building our own SAnimationPropertyConfig
// with `overridden = true` sidesteps the tree/inheritance machinery entirely and
// just uses our own bezier name + speed directly.
SP<Hyprutils::Animation::SAnimationPropertyConfig> buildAnimationConfig() {
    static auto PCURVE = CConfigValue<Config::STRING>("plugin:seam:animation_curve");
    static auto PSPEED = CConfigValue<Config::FLOAT>("plugin:seam:animation_speed");

    auto cfg             = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    cfg->overridden       = true;
    cfg->internalBezier   = *PCURVE;
    cfg->internalSpeed    = *PSPEED;
    cfg->internalEnabled  = 1;
    return cfg;
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
// workspaces) as adjacency boxes, in lockstep with a parallel list of the windows
// themselves so `boxes[i]` and `windows[i]` always refer to the same window.
void collectVisibleWindows(std::vector<SSeamBox>& boxes, std::vector<PHLWINDOW>& windows) {
    using Desktop::View::IGeometric;

    for (auto& w : Desktop::windowState()->windows()) {
        if (!validMapped(w) || w->isHidden())
            continue;

        const auto pos  = w->position(IGeometric::GEOMETRIC_CURRENT);
        const auto size = w->size(IGeometric::GEOMETRIC_CURRENT);
        const int  id   = sc<int>(windows.size());

        windows.push_back(w);
        boxes.push_back(SSeamBox{pos.x, pos.y, size.x, size.y, w->m_isFloating, id});
    }
}

// Cheap fingerprint of everything that can affect adjacency output, so onTick()
// can skip the full O(n^2) recompute on ticks where nothing moved.
std::size_t fingerprintVisibleWindows(const std::vector<SSeamBox>& boxes) {
    std::size_t hash = boxes.size();
    for (const auto& b : boxes) {
        auto mix = [&hash](double v) { hash ^= std::hash<double>{}(v) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2); };
        mix(b.x);
        mix(b.y);
        mix(b.w);
        mix(b.h);
        hash ^= std::hash<bool>{}(b.floating) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    }
    return hash;
}

} // namespace

void SeamState::onWindowOpened(PHLWINDOW window) {
    if (!window || g_entries.contains(window))
        return;

    auto config = buildAnimationConfig();

    SWindowEntry entry;
    const float  initial = 0.F;
    Animation::mgr()->createAnimation(initial, entry.topLeft, config, window, AVARDAMAGE_ENTIRE);
    Animation::mgr()->createAnimation(initial, entry.topRight, config, window, AVARDAMAGE_ENTIRE);
    Animation::mgr()->createAnimation(initial, entry.bottomLeft, config, window, AVARDAMAGE_ENTIRE);
    Animation::mgr()->createAnimation(initial, entry.bottomRight, config, window, AVARDAMAGE_ENTIRE);

    g_entries.emplace(window, std::move(entry));
    recomputeAll();
}

void SeamState::onWindowClosed(PHLWINDOW window) {
    g_entries.erase(window);
}

SeamState::SLiveCorners* SeamState::liveCornersFor(PHLWINDOW window) {
    auto it = g_entries.find(window);
    if (it == g_entries.end())
        return nullptr;

    static SLiveCorners out; // single scratch instance; caller reads it immediately, never stores the pointer
    out = {it->second.topLeft->value(), it->second.topRight->value(), it->second.bottomLeft->value(), it->second.bottomRight->value()};
    return &out;
}

void SeamState::recomputeAll() {
    const auto defaults = currentGlobalDefaults();

    std::vector<SSeamBox>  boxes;
    std::vector<PHLWINDOW> windows;
    collectVisibleWindows(boxes, windows);

    for (size_t i = 0; i < windows.size(); ++i) {
        auto& w = windows[i];

        auto it = g_entries.find(w);
        if (it == g_entries.end())
            continue; // not tracked yet (e.g. open event hasn't fired); nothing to retarget

        auto&       entry      = it->second;
        const auto& subjectBox = boxes[i];

        auto resolved = resolveWindowConfig(w->m_class, w->m_isFloating, defaults, SeamRuleStore::rules());

        if (!resolved.seamEnabled) {
            // Seam is off for this window (globally, by rule, or because it's floating):
            // use plain per-corner base radii and drop any stale hysteresis state so a
            // later re-enable (e.g. a config reload or rule change) starts clean.
            entry.wasTouching[0] = entry.wasTouching[1] = entry.wasTouching[2] = entry.wasTouching[3] = false;

            retarget(entry.topLeft, clampCornerRadius(resolved.radii.topLeft, subjectBox.w, subjectBox.h));
            retarget(entry.topRight, clampCornerRadius(resolved.radii.topRight, subjectBox.w, subjectBox.h));
            retarget(entry.bottomLeft, clampCornerRadius(resolved.radii.bottomLeft, subjectBox.w, subjectBox.h));
            retarget(entry.bottomRight, clampCornerRadius(resolved.radii.bottomRight, subjectBox.w, subjectBox.h));
            continue;
        }

        const SCornerFlags touching = computeTouchingCorners(subjectBox, boxes, defaults.tolerance);

        // Hysteresis: once a corner is flagged as touching, don't drop the flag the
        // instant the normal-tolerance check fails — only drop it once a *widened*
        // tolerance check also fails, i.e. once the gap has clearly grown past the
        // boundary rather than merely crossed it. (The reverse direction — going
        // from unflagged to flagged — always uses the normal tolerance immediately;
        // hysteresis only guards against flicker on the way out.)
        const SCornerFlags widened = computeTouchingCorners(subjectBox, boxes, defaults.tolerance * 1.5);

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
}

void SeamState::onTick() {
    std::vector<SSeamBox>  boxes;
    std::vector<PHLWINDOW> windows;
    collectVisibleWindows(boxes, windows);

    const std::size_t fingerprint = fingerprintVisibleWindows(boxes);
    if (fingerprint == g_lastFingerprint)
        return;

    g_lastFingerprint = fingerprint;
    recomputeAll();
}

std::string SeamState::debugDump() {
    std::ostringstream out;
    out << g_entries.size() << " tracked window(s)\n";
    for (auto& [w, entry] : g_entries) {
        out << "- " << (w ? w->m_class : std::string{"<null>"}) << ": "
            << "TL=" << entry.topLeft->value() << (entry.wasTouching[0] ? "*" : "") << " "
            << "TR=" << entry.topRight->value() << (entry.wasTouching[1] ? "*" : "") << " "
            << "BL=" << entry.bottomLeft->value() << (entry.wasTouching[2] ? "*" : "") << " "
            << "BR=" << entry.bottomRight->value() << (entry.wasTouching[3] ? "*" : "") << "\n";
    }
    return out.str();
}
