# drawSurface hook notes (Task 6 spike)

Verified against **Hyprland 0.56.2** (commit `efb50993780079460b0cbed1363e2166a2de1d9f`,
Arch package `hyprland 0.56.2-3`). Everything here is internal, undocumented API and must be
re-checked on every Hyprland upgrade.

## Hook target

- Header: `/usr/include/hyprland/src/render/ElementRenderer.hpp`
  (include as `<hyprland/src/render/ElementRenderer.hpp>`)
- Declaration (a **private, non-virtual** member):

  ```cpp
  namespace Render {
      class IElementRenderer {
          ...
        private:
          void drawSurface(WP<CSurfacePassElement> element, const CRegion& damage);
      };
  }
  ```

- Exact demangled symbol, as returned by `HyprlandAPI::findFunctionsByName(PHANDLE, "drawSurface")`:

  ```
  Render::IElementRenderer::drawSurface(Hyprutils::Memory::CWeakPointer<CSurfacePassElement>, Hyprutils::Math::CRegion const&)
  ```

### Differences from the plan/brief

The brief guessed `drawSurface(Render::CRenderContext&, WP<CSurfacePassElement>, const CRegion&)`.
Both parts of that guess were wrong for 0.56.2:

- There is **no `CRenderContext` parameter at all**, qualified or not. The function takes only
  `(WP<CSurfacePassElement>, const CRegion&)`.
- The `Render::` namespace is real, but it is on the **class** (`Render::IElementRenderer`), not on a
  context type.

So where does Task 7 get its context? From the element and from globals:
`element->m_data.pMonitor` (`PHLMONITORREF`), `element->m_data.pWindow` (`PHLWINDOW`, null for
layer/other surfaces), `Render::GL::g_pHyprOpenGL`, and `g_pHyprRenderer`.

There is a concrete GL subclass, `Render::GL::CGLElementRenderer`
(`src/render/gl/GLElementRenderer.hpp`). It overrides only the virtual `draw(...)` overloads.
`drawSurface` lives on the base class and is not virtual, so hooking the base symbol covers it.

## Calling convention that works

```cpp
using drawSurface_t = void (*)(void* thisptr, WP<CSurfacePassElement> element, const CRegion& damage);

void hkDrawSurface(void* thisptr, WP<CSurfacePassElement> element, const CRegion& damage) {
    // ... our work ...
    (*reinterpret_cast<drawSurface_t>(g_hook->m_original))(thisptr, element, damage);
}
```

- **`this`**: under the Itanium x86-64 ABI, a non-virtual member function is called like a free
  function with the object pointer as a hidden first argument (in `rdi`). Writing it as an explicit
  leading `void* thisptr` and passing it unchanged to `m_original` works. We never dereference it.
- **`WP<CSurfacePassElement>` by value**: `CWeakPointer` is non-trivially copyable, so the ABI passes
  it by invisible reference to a caller-owned temporary. Because the hook uses the same C++ parameter
  type, the compiler lowers both sides the same way. Do not change it to a raw pointer or a reference.
- **Disambiguation**: `findFunctionsByName("drawSurface")` returned exactly one match on 0.56.2.
  `preDrawSurface` was not returned, although it exists as a symbol. `SeamHook::install()` still
  compares against the full demangled string above and refuses to hook if it finds zero or more than
  one exact match. The symbol is in the dynamic symbol table (`nm -D /usr/bin/Hyprland`), even though
  the binary is stripped.
- **Call path** (from a crash backtrace): `CMonitorFrameScheduler::onFrame` -> `IHyprRenderer::renderMonitor`
  -> `CHyprGLRenderer::endRender` -> `CRenderPass::render` -> `IHyprRenderer::draw` ->
  `IElementRenderer::drawElement` -> `IElementRenderer::preDrawSurface` -> **`drawSurface`**.
  The call from `preDrawSurface` is a real call that reaches the trampoline; it is not inlined.

## SURPRISE / hard rule: never `lock()` the element WP

The first nested run aborted on the first window frame with:

```
Assertion failed: impl_->lockable() (tried to lock a CWeakPointer over a CUniquePointer) @ /usr/include/hyprutils/memory/WeakPtr.hpp:180
```

Render pass elements are owned by `UP<>`, so the `WP<CSurfacePassElement>` points at a unique
pointer and `WP::lock()` hard-asserts. The hook signature was fine (the backtrace showed our hook
called from `preDrawSurface` with a correctly decoded WP). The crash came from the logging body.
Use `if (!element.expired()) { auto* el = element.get(); ... }` (or `element->`) instead.
**Task 7 must never call `.lock()` on this WP.**

## Useful element fields for Task 7

From `CSurfacePassElement::SRenderData` (`src/render/pass/SurfacePassElement.hpp`, `m_data`):
`pWindow`, `pLS`, `popup`, `mainSurface`, `pMonitor`, `pos`, `localPos`, `w`, `h`, `clipBox`,
`rounding`, `dontRound`, `roundingPower`, `alpha`, `fadeAlpha`, `blur`, `decorate`, `surface`,
`texture`. The hook fires for **every** surface: windows, subsurfaces and popups
(`mainSurface == false` / `popup == true`), and layer surfaces (`pWindow` null, `pLS` set).
Task 7 must filter on `pWindow && mainSurface && !popup`, or whatever policy it settles on.

## GL scissor / clip primitive

- Header: `/usr/include/hyprland/src/render/OpenGL.hpp`, class `Render::GL::CHyprOpenGLImpl`
  (public section). Global instance: `Render::GL::g_pHyprOpenGL` (`inline UP<CHyprOpenGLImpl>`).

  ```cpp
  void scissor(const CBox&, bool transform = true);
  void scissor(const pixman_box32*, bool transform = true);
  void scissor(const int x, const int y, const int w, const int h, bool transform = true);
  ```

- All three are exported from the binary (`nm -D`), so a plugin can link against them.
- Reset/disable: `Render::IHyprRenderer::disableScissor()` (virtual, `src/render/Renderer.hpp`,
  implemented by `Render::GL::CHyprGLRenderer::disableScissor()`), reached via `g_pHyprRenderer`.
  `CHyprOpenGLImpl` also has a `CAP_STATUS_SCISSOR_TEST` cap tracked through `setCapStatus`.
- `renderRoundedShadow(const CBox&, int round, float roundingPower, int range, ...)` sits in the same
  class next to `renderRect` and `renderTexture`. The expected pattern is `scissor(box)` -> draw ->
  `disableScissor()`. Whether `scissor(nullptr)` (the `pixman_box32*` overload) also disables
  scissoring has not been checked, because only headers are installed. Task 7 should confirm this
  against the upstream `OpenGL.cpp` for this tag before relying on it.

## Verification transcript (summary)

All runs used a nested Hyprland (`WAYLAND_DISPLAY=wayland-1 Hyprland -c /tmp/seam6/nested.conf`,
socket `wayland-2`), always targeted with `hyprctl -i <signature>`. The live session was never touched.

1. Run 1: hook installed fine. On the first `foot` window frame, the nested instance aborted from
   the `WP::lock()` assertion above. Fixed by switching to `expired()`/`get()`.
2. Run 2 (fresh instance):
   - load -> `drawSurface hook installed @ 0x...` logged.
   - Opened one `foot` window -> `drawSurface fired (call #1..5): window class='foot' main=true popup=false`.
     Instance alive, `clients` OK, screenshot (`grim`) rendered correctly.
   - Opened a second window, toggled floating, moved and resized it (20 moveactive steps).
     Still alive, rendered correctly.
   - Ran a window with ~40 s of continuous output. Heartbeat lines at calls #2000, #4000 and #6000,
     with no crash or corruption.
   - `plugin unload`: rendering continued normally after the hook was removed.
   - Reloaded the plugin as a fresh `.so` copy: hook re-installed at the same address and fired
     again on the next window.
   - Unloaded, killed the nested instance, and confirmed `hyprctl instances` showed only the live one.
