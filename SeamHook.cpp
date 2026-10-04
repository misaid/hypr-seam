// Log-only spike hook on Render::IElementRenderer::drawSurface (Task 6).
// Task 7 replaces the body of hkDrawSurface with real per-corner rendering.
// See docs/hook-notes.md for the confirmed signature and calling convention.
#define WLR_USE_UNSTABLE

#include "SeamHook.hpp"
#include "globals.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <cstdint>

namespace {
    CFunctionHook* g_hook  = nullptr;
    uint64_t       g_calls = 0;

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

    void hkDrawSurface(void* thisptr, WP<CSurfacePassElement> element, const CRegion& damage) {
        ++g_calls;
        // Rate-limited: log the first few calls in detail, then a heartbeat every 2000 calls.
        if (g_calls <= 5 || g_calls % 2000 == 0) {
            std::string what = "<expired>";
            // NOTE: pass elements are owned by UP<> in the render pass, so this WP is a
            // weak-over-unique pointer. WP::lock() ASSERTS (abort) on those — that was
            // the cause of the first spike crash. Use get()/expired() instead; never lock().
            if (!element.expired()) {
                const auto* el = element.get();
                const auto& d = el->m_data;
                if (d.pWindow)
                    what = std::format("window class='{}' main={} popup={}", d.pWindow->m_class, d.mainSurface, d.popup);
                else if (d.pLS)
                    what = "layer surface";
                else
                    what = "other surface";
            }
            Log::logger->log(Log::DEBUG, "[hypr-seam] drawSurface fired (call #{}): {}", g_calls, what);
        }

        (*reinterpret_cast<drawSurface_t>(g_hook->m_original))(thisptr, element, damage);
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
