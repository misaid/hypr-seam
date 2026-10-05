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
(Only main window surfaces were observed in testing. Popup, subsurface and layer firing is inferred
from the source: `drawElement` routes every `EK_SURFACE` here. Filtering out subsurfaces means
subsurfaces such as video or GL child surfaces that reach the window corners keep square corners.)
`element.get()` is valid only for the duration of that one hook call; never store it.

## GL scissor / clip primitive, and how to actually clip the window draw

Source checked: the v0.56.2 tag files at commit `efb50993780079460b0cbed1363e2166a2de1d9f`
(`src/render/ElementRenderer.cpp`, `src/render/OpenGL.cpp`, `src/render/GLRenderer.cpp`,
`src/render/gl/GLElementRenderer.cpp`, `src/render/pass/SurfacePassElement.cpp`), fetched from
`raw.githubusercontent.com/hyprwm/Hyprland/<commit>/...`. Upstream `main` was not used. Line numbers
below refer to those files.

### The primitive

- `Render::GL::CHyprOpenGLImpl::scissor(...)` (`OpenGL.hpp`, public; global
  `Render::GL::g_pHyprOpenGL`). It has three overloads, `(const CBox&, bool transform = true)`,
  `(const pixman_box32*, bool transform = true)` and `(int x, int y, int w, int h, bool transform = true)`.
  All three are exported (`nm -D`).
- `scissor(nullptr)` **does** disable scissoring: the `pixman_box32*` overload calls
  `setCapStatus(GL_SCISSOR_TEST, false)` on null (OpenGL.cpp:1020-1026).
  `g_pHyprRenderer->disableScissor()` is just `g_pHyprOpenGL->scissor(nullptr)` (GLRenderer.cpp:278-280).
- `scissor(CBox)` caches the last box in a function-local static `m_lastScissorBox` and only calls
  `glScissor` when the box changes (OpenGL.cpp:991-1018). Raw `glScissor` calls made outside this
  function desync that cache, so always go through `scissor()`.

### VERIFIED UNSAFE: wrapping `m_original` in an outer scissor

Do **not** use `scissor(box)` -> `m_original(...)` -> `disableScissor()`. On 0.56.2 the original
destroys any outer scissor state:

- `drawSurface` (ElementRenderer.cpp:219-392) draws through
  `drawElement(makeShared<CTexPassElement>(...))` -> `drawTex` -> `CGLElementRenderer::draw(CTexPassElement)`
  -> `CHyprOpenGLImpl::renderTexture`.
- `renderTextureInternal` calls `scissor(&RECT, ...)` once per damage or clip rectangle before each
  `glDrawArrays` (OpenGL.cpp:1574-1595). Each call replaces our glScissor box.
- `renderTexture` then ends with an unconditional `scissor(nullptr)` (OpenGL.cpp:1142), which turns
  `GL_SCISSOR_TEST` off.
- The blur path (`renderTextureWithBlurInternal`) also calls `scissor(nullptr)` at its start and end
  (OpenGL.cpp:2017, 2138).

An outer scissor would therefore be silently ignored, with no error. The scissor primitive is still
useful **for Task 7's own draws** (e.g. a corner patch it renders itself after `m_original` returns),
just never as a wrapper around the original.

### VERIFIED NOT VIABLE: shrinking the `damage` parameter

`drawSurface` **never reads its `damage` parameter.** Both texture draws pass
`m_renderData.damage.copy().intersect(windowBox)` (ElementRenderer.cpp:327, 345, 367, 385), where
`m_renderData` is `g_pHyprRenderer->m_renderData`. Passing a smaller `CRegion` to `m_original` has
no effect.

### Also not viable: editing `element->m_data.clipBox`

`preDrawSurface` copies `element->m_data.clipBox` into `g_pHyprRenderer->m_renderData.clipBox`
**before** calling `drawSurface` (ElementRenderer.cpp:396), which is before our hook runs. Changing
the element's `clipBox` inside the hook does nothing. `CSurfacePassElement::visibleRegion()`, which
produces the `clipRegion` that drawSurface forwards, ignores `clipBox` too (SurfacePassElement.cpp:139-179).
`m_renderData.clipBox` is in any case a single `CBox` and cannot describe "window minus corners".

### VIABLE levers (all read by the original *during* the call, so set them before and restore after)

1. **`element->m_data.rounding` / `roundingPower` / `dontRound`: viable, single radius only.**
   drawSurface reads these from `element->m_data` at ElementRenderer.cpp:285-293
   (`rounding = m_data.rounding - 1`, forced to 0 if `dontRound`) and passes them to the rounded
   texture shader as `.round` / `.roundingPower`. Writing them through `element.get()->m_data`
   before calling through works. The shader applies one radius to all four corners. Pass elements
   are rebuilt every frame, so the edit only affects that one draw. Note that `rounding <= 0` lets
   Hyprland disable blending for opaque windows (`CANDISABLEBLEND`, line 296).

2. **`g_pHyprRenderer->m_renderData.damage` (public `CRegion`, `render/types.hpp`): viable as a
   region mask, with one caveat.** This global is the actual source of the damage drawSurface draws
   into. Save it, subtract the four corner boxes (in monitor-local *scaled* pixels, the same space
   as `windowBox`), call `m_original`, then **restore it unconditionally** (scope guard). Later
   elements in the pass rely on it. The blur path also reads it (OpenGL.cpp:2031) and the render
   pass reuses it afterwards. Caveat: when `m_renderData.clipBox` or the surface's `clipRegion`
   (from `visibleRegion()`) is non-empty, `renderTextureInternal` draws per rectangle of
   `clipBox ∩ clipRegion` and **does not intersect with damage** (OpenGL.cpp:1574-1589). In that
   case a damage-only mask is bypassed. To cover it, Task 7 must also narrow
   `g_pHyprRenderer->m_renderData.clipBox`, which the hook *can* still change because preDrawSurface
   set it before the call. That only works to a single box, so it cannot remove corners. For such
   surfaces Task 7 must either accept the gap or skip them and keep native behaviour. Excluded
   corner pixels show whatever is already in the framebuffer, i.e. what lies behind the window,
   because the pass draws back to front.

3. **Recommended Task 7 composition, using levers 1 and 2:** call `m_original` once with
   `m_data.rounding = 0`/`dontRound = true` and `m_renderData.damage` minus the 4 corner boxes. Then,
   for each corner, draw the window texture again yourself with
   `g_pHyprOpenGL->renderTexture(m_data.texture, windowBox, {.damage = &cornerRegion, .round = r_corner, .roundingPower = p, ...})`,
   where `cornerRegion = savedDamage ∩ cornerBox`. A single-radius rounded draw restricted to one
   corner's box gives per-corner radii with Hyprland's own shader and no custom shader. This needs
   the same `windowBox` / UV setup drawSurface computes (`getTexBox()` scaled and rounded, plus
   `primarySurfaceUVTopLeft/BottomRight`). drawSurface **resets those UVs to (-1,-1) on exit**
   (scope guard at lines 223-226), so Task 7 must recompute or capture them, not read them after
   the call. That part is unverified and is Task 7's job.
   Do **not** call `m_original` several times per frame instead. Each call has side effects:
   `presentFeedback` (line 390-391), `discard()`, blur framebuffer work, and the blend toggle.

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

## Second hook: `CSurfacePassElement::opaqueRegion()` (black/stale corner fix)

### Why it is needed

The plugin requires `decoration:rounding = 0`. With that setting, `m_data.rounding` is 0 for every
window, so the native `CSurfacePassElement::opaqueRegion()` reports the whole window box as opaque
for any opaque surface (most terminals and GTK apps). `CRenderPass::simplify()` subtracts that
region from the damage of everything drawn underneath: the wallpaper or clear colour, other
windows, layer surfaces. The corner cut-outs that the `drawSurface` hook leaves are therefore never
repainted. On screen they show black (a fresh buffer) or stale content from an earlier frame instead
of the real backdrop.

### Target and ABI (0.56.2)

- Declaration: `virtual CRegion CSurfacePassElement::opaqueRegion();` in
  `src/render/pass/SurfacePassElement.hpp`. It overrides `IPassElement::opaqueRegion()`, which is
  documented as returning monitor-local logical coordinates.
- Exported symbol: `CSurfacePassElement::opaqueRegion()` (`nm -DC /usr/bin/Hyprland`). It is found
  with the same exact-demangled-name lookup as `drawSurface` and is the only match.
- The calling convention differs from `drawSurface`'s. `CRegion` is not trivially copyable, so the
  Itanium ABI returns it through a hidden caller-allocated slot passed in `rdi`, and `this` moves to
  `rsi`. Disassembly of the installed body confirms this: `rdi` is kept as the result slot, and
  `[rsi+0x50]` is read as `m_data.surface`. A free function `CRegion hk(void* thisptr)` lowers the
  same way (sret in `rdi`, `thisptr` in `rsi`). Declaring `CRegion` as the return type on both the
  hook and the `m_original` cast therefore matches the ABI. Do not declare it as returning `void`,
  a pointer, or a trivially copyable struct. That would shift `this` into the wrong register.
- Hooking the function body covers every virtual dispatch, because the vtable points at this body.
  `CSurfacePassElement` has single, non-virtual inheritance from `IPassElement`, so `this` needs no
  adjustment.

### What the hook does

1. Calls the original to get the native region. If it is empty, it is returned unchanged.
2. Applies the same filter as `hkDrawSurface`: main window surface only, no popups, a monitor
   present, and not `dontRound`. Internal-fullscreen (`dontRound`) windows are drawn square, so
   their native region is already correct and they keep occluding everything at full efficiency.
3. For each corner with a live radius above 0, it subtracts a box of `ceil(radius) + 2` logical px
   from the region. The box is anchored on that corner of `getTexBox()` and snapped outward to whole
   logical px. The 2 px of padding and the outward snap cover `simplify()` scaling and rounding
   every rect by the monitor scale, and the corner patches using `ceil(radius * scale)` physical
   px. If the cut is slightly too large, a few extra backdrop pixels are painted and then covered by
   the corner draw. If it were too small, a black or stale fringe would remain.

Making the opaque region smaller is always safe for correctness. It can only cause extra, correct
drawing underneath, never missing drawing. The cost is a small amount of extra overdraw per rounded
window.

Install failure is not fatal. If the symbol is missing or ambiguous, or the hook fails to install,
the plugin logs an error and shows an orange notification, and the `drawSurface` hook keeps
working. Corners may then show black or stale content. `SeamHook::remove()` removes this hook
before the `drawSurface` hook.

### Verification (nested instance, scale 1 and 1.25)

Background colour `rgb(ff8800)`, `gaps_out = 40`, no borders, shadows, blur or animations.
- Old build (HEAD before the fix), two tiled opaque `foot` windows with the seam on: all 4 outer
  corner cut-outs were pure `(0,0,0)`. That was 410 black pixels in total, all at the window
  corners.
- New build, same scene after a hot-swap: the same pixels are `(255,136,0)`, the background. The
  only black pixels left belong to the mouse cursor. The flattened seam corners still render
  correctly.
- Regression checks with the new build: a single window has all 4 corners rounded over the
  background. A floating window over a tiled one shows the tiled window's content in its corners.
  A fullscreen window is fully square with no cut-outs. At scale 1.25 with two tiled windows, there
  are no black pixels anywhere on the frame.
