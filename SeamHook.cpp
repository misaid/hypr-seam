// Real per-corner rendering hook on Render::IElementRenderer::drawSurface (Task 7).
//
// See docs/hook-notes.md (Task 6) for the confirmed signature, calling convention, and the
// three "viable levers" this body is built from. Summary of the approach (lever 3 there):
//
//   1. Paint the main window body once, through the real original, with its damage region
//      temporarily shrunk to exclude the 4 live corner boxes, and `dontRound` forced on so
//      the body paints as a plain square everywhere except those corners (native rounding
//      is globally forced to 0 anyway via `decoration:rounding = 0`).
//   2. Separately, once per corner, redraw the SAME window texture via Hyprland's own
//      g_pHyprOpenGL->renderTexture(), restricted (via a damage region containing only that
//      corner's box) to just that corner, passing THAT corner's own live radius as `.round`.
//      Hyprland's own rounded-rect shader does the rest — no custom GLSL needed.
//
// This relies on g_pHyprRenderer->m_renderData.damage being the thing that actually
// restricts drawing (confirmed empirically + from source: the `damage` *parameter* to
// drawSurface is never read). It does NOT work when m_renderData.clipBox is non-empty
// (e.g. a floating window mid slide-animation) — in that rare case we fall back to the
// native, square-cornered draw for that one frame rather than risk a corrupted clip.
#define WLR_USE_UNSTABLE

#include "SeamHook.hpp"
#include "SeamState.hpp"
#include "globals.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
    CFunctionHook* g_hook = nullptr;

    // Confirmed against /usr/include/hyprland/src/render/ElementRenderer.hpp (Hyprland 0.56.2):
    //   void Render::IElementRenderer::drawSurface(WP<CSurfacePassElement> element, const CRegion& damage);
    // A private, NON-virtual member function. Under the Itanium C++ ABI a non-virtual
    // member is called exactly like a free function with the object pointer prepended
    // as the first argument, so `this` is modelled as an explicit leading void*.
    // WP<> is non-trivially-copyable, so it is passed by invisible reference in both
    // the original and our replacement; declaring the same C++ parameter types makes
    // the compiler produce a matching ABI on both sides.
    using drawSurface_t = void (*)(void* thisptr, WP<CSurfacePassElement> element, const CRegion& damage);

    constexpr const char* TARGET_DEMANGLED = "Render::IElementRenderer::drawSurface(Hyprutils::Memory::CWeakPointer<CSurfacePassElement>, Hyprutils::Math::CRegion const&)";

    // One corner's redraw box (in the same monitor-scaled, rounded pixel space as the
    // windowBox drawSurface itself computes) plus the already-scaled-and-clamped integer
    // radius to feed Hyprland's own rounded-rect shader for just that corner.
    struct SCornerPatch {
        CBox box;
        int  round = 0; // scaled px; 0 means "no rounding needed here, leave it to the square body paint"
    };

    SCornerPatch makeCornerPatch(const CBox& windowBox, bool left, bool top, double liveRadiusLogicalPx, double monitorScale) {
        // liveRadiusLogicalPx comes from SeamState::liveCornersFor, already clamped against
        // the window's logical size (Task 5's clampCornerRadius). Re-clamp here defensively
        // against windowBox's own (scaled, rounded-to-int) dimensions anyway, since windowBox
        // can differ very slightly from the logical size SeamState clamped against (min-size
        // floor, rounding to whole scaled pixels).
        double       r    = std::max(0.0, liveRadiusLogicalPx) * monitorScale;
        const double maxR = std::min(windowBox.width, windowBox.height) / 2.0;
        r                 = std::clamp(r, 0.0, maxR);

        const double x = left ? windowBox.x : windowBox.x + windowBox.width - r;
        const double y = top ? windowBox.y : windowBox.y + windowBox.height - r;

        return SCornerPatch{CBox{x, y, r, r}, static_cast<int>(std::lround(r))};
    }

    void hkDrawSurface(void* thisptr, WP<CSurfacePassElement> element, const CRegion& damage) {
        // Never .lock() this WP: it points at a UP<>-owned render-pass element, and
        // WP::lock() hard-asserts over a CUniquePointer (docs/hook-notes.md). Use
        // expired()/get() instead, and never retain the raw pointer past this call.
        const auto callOriginal = [&]() { (*reinterpret_cast<drawSurface_t>(g_hook->m_original))(thisptr, element, damage); };

        if (element.expired()) {
            callOriginal();
            return;
        }

        auto* el   = element.get();
        auto& data = el->m_data;

        // Only act on the main window surface. Popups, subsurfaces, and layer-shell
        // surfaces (pWindow null, or mainSurface false, or popup true) pass straight
        // through to native handling untouched (docs/hook-notes.md).
        if (!data.pWindow || !data.mainSurface || data.popup || !data.pMonitor || !data.texture) {
            callOriginal();
            return;
        }

        // The real drawSurface discards early if the texture failed to upload; our own
        // renderTexture calls below would instead hard RASSERT on a not-ok texture. Bail
        // out the same way the original does rather than risk that.
        if (!data.texture->ok()) {
            callOriginal();
            return;
        }

        auto* corners = SeamState::liveCornersFor(data.pWindow);
        if (!corners) {
            callOriginal();
            return;
        }

        auto& renderData = g_pHyprRenderer->m_renderData;

        // preDrawSurface already copied element->m_data.clipBox into
        // g_pHyprRenderer->m_renderData.clipBox before this hook ran. When it's non-empty
        // (e.g. a floating window mid workspace-slide-animation), renderTextureInternal
        // bypasses damage entirely and clips to clipBox∩clipRegion instead — a single CBox
        // that can't express "window minus 4 corner squares". Fall back to the native,
        // square-cornered draw for that one frame rather than risk a corrupted clip
        // (docs/hook-notes.md, lever 2's caveat).
        if (!renderData.clipBox.empty()) {
            callOriginal();
            return;
        }

        // Same windowBox drawSurface itself computes right before using it (getTexBox() is
        // cached on the element after the first call within this frame, so our call here and
        // the real body's later call both see the identical box).
        CBox windowBox = el->getTexBox();
        windowBox.scale(data.pMonitor->m_scale);
        windowBox.round();

        if (windowBox.width <= 1 || windowBox.height <= 1) {
            callOriginal();
            return;
        }

        static auto  PPOWER       = CConfigValue<Config::FLOAT>("plugin:seam:rounding_power");
        const float  roundingPower = *PPOWER;
        const double scale         = data.pMonitor->m_scale;

        const SCornerPatch patchTL = makeCornerPatch(windowBox, true, true, corners->topLeft, scale);
        const SCornerPatch patchTR = makeCornerPatch(windowBox, false, true, corners->topRight, scale);
        const SCornerPatch patchBL = makeCornerPatch(windowBox, true, false, corners->bottomLeft, scale);
        const SCornerPatch patchBR = makeCornerPatch(windowBox, false, false, corners->bottomRight, scale);

        // Save the actual region the original draws into (the `damage` parameter is never
        // read by drawSurface — confirmed in docs/hook-notes.md), subtract the 4 corner
        // boxes, and force the main body to paint as a plain square. Restored immediately
        // after the call below: later pass elements in this same frame rely on
        // m_renderData.damage, and our own corner draws need the ORIGINAL (unpruned) damage
        // to intersect against, not the pruned one.
        const CRegion savedDamage    = renderData.damage;
        const bool    savedDontRound = data.dontRound;

        CRegion prunedDamage = savedDamage.copy();
        if (patchTL.round > 0)
            prunedDamage.subtract(CRegion(patchTL.box));
        if (patchTR.round > 0)
            prunedDamage.subtract(CRegion(patchTR.box));
        if (patchBL.round > 0)
            prunedDamage.subtract(CRegion(patchBL.box));
        if (patchBR.round > 0)
            prunedDamage.subtract(CRegion(patchBR.box));

        renderData.damage = prunedDamage;
        data.dontRound    = true;

        callOriginal();

        renderData.damage = savedDamage;
        data.dontRound    = savedDontRound;

        // Redraw each corner with its own live radius, restricted to just that corner's box
        // via a damage region containing only it (intersected with what was actually damaged
        // this frame). Hyprland's own rounded-rect shader (invoked once per corner with that
        // corner's own radius) is the per-corner-radius mechanism — no custom GLSL needed.
        auto PSURFACE          = Desktop::View::CWLSurface::fromResource(data.surface);
        const float ALPHA      = data.alpha * data.fadeAlpha * (PSURFACE ? PSURFACE->m_alphaModifier : 1.F);
        const float OVERALLA   = PSURFACE ? PSURFACE->m_overallOpacity : 1.F;

        auto drawCorner = [&](const SCornerPatch& patch) {
            if (patch.round <= 0)
                return;

            CRegion cornerRegion = savedDamage.copy().intersect(CRegion(patch.box));
            if (cornerRegion.empty())
                return;

            Render::GL::g_pHyprOpenGL->renderTexture(data.texture, windowBox,
                                                      Render::GL::CHyprOpenGLImpl::STextureRenderData{
                                                          .damage        = &cornerRegion,
                                                          .surface       = data.surface,
                                                          .a             = ALPHA * OVERALLA,
                                                          .round         = patch.round,
                                                          .roundingPower = roundingPower,
                                                          .wrapX         = data.wrapX,
                                                          .wrapY         = data.wrapY,
                                                      });
        };

        drawCorner(patchTL);
        drawCorner(patchTR);
        drawCorner(patchBL);
        drawCorner(patchBR);
    }
} // namespace

bool SeamHook::install() {
    const auto matches = HyprlandAPI::findFunctionsByName(PHANDLE, "drawSurface");

    void*      target = nullptr;
    for (const auto& m : matches) {
        Log::logger->log(Log::DEBUG, "[hypr-seam] drawSurface candidate: {} @ {}", m.demangled, m.address);
        // On 0.56.2 only drawSurface itself was returned (not preDrawSurface), but
        // match the exact demangled form anyway in case future versions differ.
        if (m.demangled == TARGET_DEMANGLED) {
            if (target) {
                Log::logger->log(Log::ERR, "[hypr-seam] multiple exact drawSurface matches, refusing to hook");
                target = nullptr;
                break;
            }
            target = m.address;
        }
    }

    if (!target) {
        HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] Could not locate drawSurface to hook — rendering will not work.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        return false;
    }

    g_hook = HyprlandAPI::createFunctionHook(PHANDLE, target, reinterpret_cast<void*>(&hkDrawSurface));
    if (!g_hook || !g_hook->hook()) {
        Log::logger->log(Log::ERR, "[hypr-seam] failed to install drawSurface hook");
        HyprlandAPI::addNotification(PHANDLE, "[hypr-seam] Failed to hook drawSurface — rendering will not work.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        if (g_hook)
            HyprlandAPI::removeFunctionHook(PHANDLE, g_hook);
        g_hook = nullptr;
        return false;
    }

    Log::logger->log(Log::DEBUG, "[hypr-seam] drawSurface hook installed @ {}", target);
    return true;
}

void SeamHook::remove() {
    if (g_hook)
        HyprlandAPI::removeFunctionHook(PHANDLE, g_hook);
    g_hook = nullptr;
}
