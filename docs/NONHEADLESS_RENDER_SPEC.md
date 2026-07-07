# Revenant — Non‑Headless Rendering Architecture SPEC

Status: DRAFT for review. Target: replace the hand‑driven "headless" render integration with a real
on‑screen platform integration so **WebCore drives its own rendering, timing, visibility, and
compositing**, exactly as it would in a shipping WebKit port. No per‑frame hacks.

---

## 0. Principle

The port's job is to give WebCore the platform objects a real on‑screen view has, then get out of the
way. The port must NOT hand‑pump layout/animation/paint. Concretely, the port owns four things and
nothing else:

1. A GL surface + a `TextureMapper` to composite WebCore's layer tree onto it.
2. A **`HostWindow`** (window/screen geometry + invalidation) — via `ChromeClient`.
3. A **`DisplayRefreshMonitor`** (vsync) so WebCore's `RenderingUpdateScheduler` runs.
4. Input delivery (touch/keyboard) into WebCore's `EventHandler`.

Everything else — when to lay out, when to run animations/rAF, what's visible, what to repaint, how
to scroll, DPR scaling — is WebCore's, driven by the above. This is what makes GIFs animate, scrolling
smooth, visibility correct, and media queries right *by construction* instead of by patch.

---

## 1. Current headless architecture — what's wrong (all deleted)

`WebCoreBrowserRenderFrame()` hand‑drives every frame:

| Hack | Why it exists | Why it's wrong |
|---|---|---|
| Manual `RunLoop::iterate()` + explicit `performMicrotaskCheckpoint()` each frame | no event loop integration | fine to keep the pump, but must not *force* rendering |
| Manual `WebCoreDriverRunRenderingUpdate()` every frame | no `DisplayRefreshMonitor` → scheduler dead | forces layout+anim+flush every vsync even when idle; wrong cadence; rAF timestamps wrong |
| **Flat content layer** (`BrowserContentClient` paints whole `FrameView`) *plus* the composited root | compositor root "only paints composited layers" | that's only true when NOT in forced‑compositing mode; the flat layer is a workaround for not letting WebCore own the whole tree |
| **Manual DPR scale‑wrapper** (`g_scaleLayer`, `scale(DPR)` transform) | DPR never handed to WebCore | WebCore should own DPR via `setDeviceScaleFactor`; layout, hit‑testing, image `srcset`, media `resolution` are all wrong without it |
| `fullRepaint` / `safetyFull` / `takeDirtyRegion` / `g_needsRepaint` | manual dirty tracking | per‑layer invalidation is WebCore's job in forced‑compositing mode |
| `resumeVisibleImageAnimations` / `viewportContentsChanged` patch | GIFs paused (visibility broken) | symptom of empty `windowClipRect`; fixed by a real `HostWindow`/viewport |
| `setActivityState(...)` as a standalone poke | animations throttled | correct to set, but it's part of the integration, not a one‑off |
| DOM/PRIM console probes in the frame loop | debugging | debug cruft |

Also stubbed today and blocking correctness:
- `PortChromeClient::windowRect()` / `pageRect()` return **empty** `FloatRect()`.
- `rootViewToScreen` / `screenToRootView` are **identity** (ignore DPR + screen origin).
- `WebCoreBrowserScrollBy` does `view->setScrollPosition(...)` + `g_needsRepaint = true` (manual).

---

## 2. Target architecture

### 2.1 Page/Settings (init, once)
- `settings().setAcceleratedCompositingEnabled(true)` (already on)
- **`settings().setForceCompositingMode(true)`** — WebCore composites the *entire* document: the
  `RenderLayerCompositor` root content layer paints all non‑composited content, composited elements
  are its children. The whole page is one GPU layer tree. (This is what WebKit2's drawing area uses.)
- **`page->setDeviceScaleFactor(DPR)`** — WebCore owns DPR. Layout is in CSS px; backing stores are
  device px. No manual scale wrapper.
- `page->setActivityState({ IsVisible, WindowIsActive, IsFocused, IsInWindow, IsVisibleOrOccluded })`.

### 2.2 FrameView / viewport
- `FrameView` sized to the **CSS viewport** (`cssW × cssH = physical / DPR`).
- `view->setPaintsEntireContents(...)` NOT needed — forced compositing + tiled backing cover it. The
  visible content rect is the viewport; scroll moves it.
- Background: opaque page background (white/theme) so no cleared‑to‑white flashes.

### 2.3 HostWindow (via `PortChromeClient`) — make these REAL
- `windowRect()` = `pageRect()` = `{0, 0, cssW, cssH}` (root‑view coords) — non‑empty. This feeds
  `windowClipRect()` → `isVisibleInViewport()` → **GIF/animation visibility works with no patch**.
- `rootViewToScreen(r)` / `screenToRootView(p)` = apply device scale + screen origin (identity origin
  is fine for fullscreen; scale matters for `window.screenX/Y`, popups).
- `invalidateContentsAndRootView(rect)` = mark the compositor dirty (WebCore already routes repaint
  here; we just schedule a composite, not a manual dirty region).
- `attachRootGraphicsLayer(layer)` = store the root; `setNeedsOneShotDrawingSynchronization` /
  `triggerRenderingUpdate` = request a composite on next vsync (no synchronous render).

### 2.4 PlatformScreen
- Provide real `screenRect`/`screenSize`/`screenDepth` (device px, 24‑bit) so `window.screen` and
  `@media (resolution/device-width)` are correct. (Port a minimal `PlatformScreen` for W10M.)

### 2.5 DisplayRefreshMonitor (DONE — keep)
- `PortDisplayRefreshMonitor` + factory, wired via `PortChromeClient::displayRefreshMonitorFactory()`.
- Driven by the shell's 60Hz `CompositionTarget::Rendering` → `WebCoreBrowserVsyncTick()` →
  `displayLinkFired` → `RenderingUpdateScheduler` → `updateRendering`. Idle‑throttles itself.

### 2.6 Compositing (the render output)
- Composite **WebCore's root graphics‑layer tree** (`rootGraphicsLayer()`), which in forced mode paints
  the entire page, via `TextureMapper` to the default framebuffer at device px.
- Per‑layer backing stores; only changed layers re‑raster (WebCore's flush decides). No flat layer, no
  scale wrapper, no manual dirty rects.

### 2.7 Input / scroll
- Touch/keyboard → `EventHandler` (already mostly wired for key/char/tap).
- Scroll → deliver a wheel/scroll event to WebCore (or `FrameView` scroll) and let the **scrolling
  coordinator / compositor** move the layers; do NOT set `g_needsRepaint`. Smooth scroll = moving
  cached layers, not re‑rastering.

---

## 3. The new frame loop (shape)

```
WebCoreBrowserRenderFrame():   // called once per vsync from the shell (CompositionTarget::Rendering)
    makeContextCurrent()
    RunLoop::current().iterate()             // deliver curl completions, run timers + JS
    performMicrotaskCheckpoint()             // advance promise/module graph
    WebCoreBrowserVsyncTick()                // -> RenderingUpdateScheduler -> updateRendering
                                             //    (layout, animations, rAF, IntersectionObserver,
                                             //     compositing flush) ONLY if WebCore has work
    // composite whatever WebCore produced:
    root = chrome.rootGraphicsLayer()
    bind default FBO; viewport(winW,winH); clear(pageBackground)
    if root:
        updateBackingStoreIncludingSubLayers(textureMapper)   // upload dirty tiles
        textureMapper.beginPainting()
        root.layer().paint(textureMapper)
        textureMapper.endPainting()
    swapBuffers()
```

No `WebCoreDriverRunRenderingUpdate`, no fullRepaint logic, no DOM probes, no scale wrapper.

---

## 4. Coordinate systems

- **CSS px**: layout, `FrameView` size = `cssW × cssH`. Scroll position in CSS px.
- **Device px**: backing stores + framebuffer = `winW × winH = cssW*DPR × cssH*DPR`.
- **DPR**: `page->setDeviceScaleFactor(DPR)`. WebCore scales CSS→device internally; the root layer's
  backing is device px; we composite 1:1 to the framebuffer.
- Input arrives in **device px** from the shell → convert to CSS px (`/DPR`) before handing to
  `EventHandler` (or let WebCore convert if we deliver in root‑view coords).

---

## 5. What gets DELETED
`BrowserContentClient` (flat layer), `g_contentLayer`, `g_scaleLayer` (scale wrapper),
`WebCoreDriverRunRenderingUpdate` (manual), `viewportContentsChanged` patch, `resumeVisibleImageAnimations`
poke, `g_needsRepaint`/`fullRepaint`/`safetyFull`/`g_postLoadFrames`/`takeDirtyRegion` machinery, DOM/PRIM
probes, `g_flatOnlyFallback` (the flat fallback — see risks), the `img:`/`gif:` diagnostics.

## 6. What STAYS
The GL/ANGLE surface + `TextureMapper`, `PortDisplayRefreshMonitor`, the SEH crash wrapper + native
stall detector (diagnostics for the new path too), curl/loader stack, all the Settings feature‑enables,
`PortChromeClient` (extended), the C ABI the shell calls (`Init`, `RenderFrame`, `Scroll`, `Tap`, `Key`,
`Navigate`, `Resize`, back/forward), memory logging.

## 7. Risks & open questions
1. **x.com ANGLE `layerPaint` crash** — the composited‑root path is the SAME path that crashes FL9_3 on
   x.com. Without the flat fallback, x.com may hard‑fault (SEH catches it, page can't paint). Decision:
   keep the SEH guard; treat x.com's specific ANGLE fault as its own task (#19). Do NOT keep the flat
   fallback as a crutch. (This is the one place the rewrite could regress a site that "worked.")
2. **Does forced‑compositing's root layer actually paint ALL non‑composited content in this port?**
   Needs verification on‑device (the whole rewrite hinges on it). If a page renders only its composited
   sublayers, the RenderLayerCompositor isn't building the root content layer — investigate
   `RenderLayerCompositor::requiresCompositingLayer`/root‑content‑layer creation.
3. **`rootGraphicsLayer()` type** — must be `GraphicsLayerTextureMapper` for our composite path. Confirm
   the default `GraphicsLayer::create` factory yields TextureMapper layers for the compositor's layers
   (it does for our manual layers today).
4. **DPR change blast radius** — `setDeviceScaleFactor` moves the whole coordinate system; tap/scroll/
   viewport math must be re‑checked together.
5. **Scroll model** — do we let WebCore's scrolling coordinator composite‑scroll, or keep
   `setScrollPosition`? Compositor scroll is the "proper" smooth path but needs the scrolling tree wired.
6. **PlatformScreen** — which impl compiles today? May need a minimal W10M `PlatformScreen`.

## 8. Implementation phases (each builds + is tested on‑device)
- **P1 — WebCore owns the tree + DPR.** `setForceCompositingMode(true)` + `setDeviceScaleFactor(DPR)` +
  composite `rootGraphicsLayer()` directly; delete flat layer + scale wrapper. Gate: a simple page
  (charavault) renders correctly at the right scale from the composited root only.
- **P2 — WebCore owns timing.** Delete `WebCoreDriverRunRenderingUpdate`; the `DisplayRefreshMonitor`
  drives `updateRendering`. Gate: Google Doodle GIF animates on its own; no white flicker.
- **P3 — Real HostWindow + viewport.** Real `windowRect`/`pageRect`/`rootViewToScreen` + `PlatformScreen`.
  Gate: `isVisibleInViewport` true without any poke; media queries/`window.screen` correct.
- **P4 — Compositor scroll + input cleanup.** Scroll via events → compositor moves layers (no forced
  repaint). Gate: smooth scroll on a long page.
- **P5 — Delete remaining hacks + diagnostics; final sweep.**

Each phase deletes its corresponding hack — no phase leaves a workaround "just in case" except the SEH
crash guard (safety, not behavior).
