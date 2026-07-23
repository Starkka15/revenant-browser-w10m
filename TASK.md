# Revenant — work plan

**Last updated:** 2026-07-20 (development paused; resuming with a Lumia 950 available)

This is the ordered plan. It exists so the next session starts by *doing* rather than re-deriving.
Every item cites the evidence behind it. Where something is a hypothesis rather than a measurement,
it says so — do not treat the two the same.

Companion docs: [KNOWN_ISSUES.md](KNOWN_ISSUES.md) (state + open bugs), [BUILD.md](BUILD.md) (build),
[CONTRIBUTING.md](CONTRIBUTING.md) (workflow).

---

## Ground rules (learned the hard way)

1. **Read the whole `debug.log`, not the lines you expect.** Nearly every wrong turn in this project
   came from grepping for a theory. The `drawn=0.0fps` counter that pins the video bug was present in
   every log for hours while the investigation chased routing, codecs and pipeline startup.
2. **Trace callers before writing the call.** Several changes were unreachable by construction — most
   notably a media path gated on a `readyState` only reachable via the code path it had skipped.
3. **One instrumentation build, not five.** Design the whole diagnostic, then build once. A build is
   ~15-30 min; a wrong guess costs a full cycle.
4. **A clean compile proves nothing.** Several changes built fine and were inert (installed before the
   object they needed existed). Only the device log shows it.
5. **Verify one change at a time on device** where attribution matters.

---

## Device note: 640 XL vs 950

**Target device is the 640 XL** (Snapdragon 400, 4×Cortex-A7, 1GB → **tier-0**, 390MB cap, Adreno 305,
D3D FL9_3). Everything below was derived there.

The **950** (Snapdragon 808, **3GB / 32GB**, Adreno 418) exercises the same code on different
hardware. The AppMemoryUsageLimit scales with device RAM, so a 3GB device lands at **tier-1 at minimum,
likely tier-2** — read the actual cap from the `membudget:` log line on first launch. Code-path
differences that will change observed behaviour there:

- **Different budget branch** (`WebCoreSetMemoryBudgetFromLimit`, `WebCoreDriver.cpp:376`): tier-1 is
  memCache 48MB / MSE keep-behind 20s; tier-2 is memCache 96MB and a much higher ceiling. Either way
  the tier-0 survival constraints relax hard.
- **The single-video decode slot is tier-0 only.** `g_tier0ActivePlayer` and the eviction path around it
  do not run above tier-0 — including the slot-race fix in T-0.2, which cannot reproduce there.
- **The tier-0 memory-survival work (release gating, the 250MB attribution, MSE forward trim) is largely
  moot above tier-0.** The device-independent bugs (video draw, input, @font-face, back button, HLS DNS)
  are what matter on the 950.
- **Adreno 418 supports a higher D3D feature level**, so anything attributed to FL9_3 needs re-checking
  before it counts as a hardware limit.
- **T-1 (video draw) is a suspected scheduling race.** A faster CPU may resolve it the other way, so the
  two devices disagreeing is itself a signal about the hypothesis.

## Phase 0 — costs nothing, do first (no build)

### T-0.1 · Settle the video bug from the existing log
The decisive greps need no rebuild. Pull `debug.log` from a session where a rule34 video post was
opened, then:

```sh
grep -n 'cmplayer:.*RenderVideo' debug.log     # was the video promoted to a composited layer?
grep -c 'TMPAINT-C#'            debug.log      # was its contents layer painted?
grep -c 'fsvid:'                debug.log      # did the video draw path run? (known: 0)
```

Decision table:

| `cmplayer: RenderVideo` | `TMPAINT-C` | Meaning | Go to |
|---|---|---|---|
| absent | — | never composited; no contents layer exists to paint | **T-1** |
| present | absent | composited but the contents layer is not painted | **T-1b** |
| present | present | painted, but `m_useFrameServer` false at paint time | **T-1c** |

*(Log pulls need the app closed — it holds the file open. WDP pairing expires; re-pair via
`POST /api/authorize/pair?pin=<PIN>`.)*

### T-0.2 · Verify the two untested changes already on `master`
Both compiled and deployed but were **never confirmed on device**:
- **Media slot locking** (`ShellMse.cpp`, `s_tier0SlotLock`). Expect **at most one**
  `mse: MediaPlayer frame-server started` per eviction. The bug it fixes: three `<video>` elements
  probed simultaneously on three curl worker threads, all read `g_tier0ActivePlayer` as null before any
  wrote it, and all three created pipelines on a device that permits one.
  **Tier-0 only — will not reproduce on the 950.**
- **HLS divert** (`ShellMse.cpp`, `hlsStartOnHandle`). Expect `prog: probe says HLS ... -> diverting`
  on playlist content and **no divert** on ordinary MP4.

---

## Phase 1 — one instrumentation build

Do these **together, in a single build**. None changes behaviour; all of them answer questions that are
currently blocking real fixes.

### T-1.1 · Fix three accounting bugs in the `mempool:` line
`WebCoreDriver.cpp` ~2387-2403. The `other=` figure is inflated by known errors, so the real
unattributed number is smaller than the ~250MB reported:
- `jsHeap` uses `vm->heap.size()`; **committed** memory is `heap.capacity()` (`Heap.cpp:916` vs `:921`).
  MarkedSpace capacity does not shrink back after a spike, and the delta is real committed memory
  currently binned into `other`.
- `malloc=` reads `fastMallocStatistics().committedVMBytes`, which returns a hardcoded 0 under
  `USE_SYSTEM_MALLOC` (`FastMalloc.cpp:293`). Delete the term or relabel it — it is not evidence.
- `cairoSurf=` is **partially dead**: `trackCairoImageSurface()` has two call sites and one
  (`BackingStoreBackendCairoImpl`) is never compiled in this single-process build. The live one tracks
  only ImageBuffers, which are transient inside `BitmapTexture::updateContents()` — so it can only ever
  sample ~0. Decoded images and canvas backing are not covered at all.

### T-1.2 · Add `MemoryManager::GetAppMemoryReport()` to the memwatch line
App-partition WinRT, available since 10.0.14393, **currently unused**. Gives `PrivateCommitUsage`,
`TotalCommitUsage`, `PeakPrivateCommitUsage`. Splitting `appUsage` into private-vs-total commit halves
the search space for the unattributed memory at zero risk.

### T-1.3 · Add `cachestorage:` totals
Sum `Record::responseBodySize` (already a field, `DOMCacheEngine.h:79`) across `PortCacheStorage`.
Three lines. If T-4.1 is the answer, this proves it in one device run.

### T-1.4 · Add a `vmwalk:` address-space census
`VirtualQuery` **is permitted under AppContainer** — already used in-tree at
`ExecutableAllocator.cpp:400`. (PSAPI is blocked: `PlatformWin.cmake:22` deliberately compiles
`MemoryFootprintGeneric.cpp` because `QueryWorkingSetEx` is desktop-only.)

Walk the address space at the existing gate cadence (~1-3ms, not per-frame) and report commit split by
`MEM_IMAGE` / `MEM_MAPPED` / `MEM_PRIVATE`, an exec-protection total, private-allocation size buckets,
region counts, and largest free block. This single line settles the biggest unknown: whether the
unattributed memory is **private commit (ours)** or **image/mapped (mostly not ours)**, and whether it
is *a few huge buffers* or *many small allocations*. The JIT's 16MB RWX reservation doubles as a
calibration constant — if `exec` reads ~16MB, the walk is correct.

### T-1.5 · Soft-link `HeapSummary` for CRT heap totals
Header-blocked by `WINAPI_PARTITION_DESKTOP` but likely runtime-permitted; resolve via `GetProcAddress`
using the existing `SoftLinking.h`. Returns `cbAllocated`/`cbCommitted`/`cbReserved` in O(segments).
`cbCommitted − cbAllocated` **is** the fragmentation number, measured rather than argued.
**Log whether it resolved** — a "not available" line is itself a result we do not have today.
Do *not* use `HeapWalk` periodically: it is O(live blocks), i.e. 100ms-1s+ with a WebKit heap.

### T-1.6 · Log the video draw path's state
Only if T-0.1 lands on T-1c. One throttled line at the top of `paintCurrentFrameToTextureMapper`
printing `m_useFrameServer`, `m_fsFrameDirty`, `m_fsFrameW/H`, `m_fsGlTexture`.

---

## Phase 2 — one-line fixes, high confidence

Independent of everything above. Cheap enough to fold into the Phase 1 build.

### T-2.1 · Touch coordinates are ~9.7% wrong
`WebCoreDriver.cpp:3150` — `WebCoreBrowserTouch` builds `IntPoint pos(x, y)` from raw DIPs and is the
**only** input entry point that never calls `dipToCss`. `Tap` (`:3112`) and `ScrollBy` (`:2980`) both
convert correctly. On the 640 XL a touch near the bottom reports `y≈623` when the page is 568 CSS tall,
and the same physical tap reports **different coordinates via `click` vs `touchstart`**. Breaks
touch hit-testing, canvas drawing coordinates and swipe-threshold maths.

### T-2.2 · `psl.lib` is built with the debug CRT
`deps/libpsl-build.bat:6` calls `meson setup` with no `--buildtype`; meson defaults to `debug` → `/Od`
and **`/MDd`**. Verified: `psl.lib` is the only dependency whose `DEFAULTLIB` is `MSVCRTD`. It is not
actively corrupting anything — the shell's explicit `msvcrt.lib` plus `/FORCE:MULTIPLE` beats the
directive — but it survives on link-order luck. Add `--buildtype release -Doptimization=2
-Db_ndebug=true` and rebuild. (Dated Jun 26; predates the cairo/pixman fix pass.)

### T-2.3 · HarfBuzz ships with assertions enabled
`deps/harfbuzz-build.bat:8` passes `--buildtype release` but not `-Db_ndebug=true`, so `NDEBUG` is never
defined and HarfBuzz's internal sanity machinery runs in production. `pixman-build.bat` and
`cairo-build.bat` both pass it. HarfBuzz runs on every complex-text run — inside the measured
`update=883ms`.

### T-2.4 · `WebCoreBrowserResize` ignores its `deviceScale` argument
`WebCoreDriver.cpp:3371` recomputes `g_winW/H`, `g_cssW/H` and `g_deviceScale` but never updates
`g_panelDipScale`, which is written only at init. Benign under rotation; wrong the moment
`RawPixelsPerViewPixel` changes (Continuum, external display, system text-scale). One line.

### T-2.5 · Register `PointerCaptureLost` / `PointerCanceled`
`RenderShellXaml.cpp:162-164` registers only `PointerPressed/Moved/Released`. A system gesture, incoming
call or PLM suspend fires the capture-lost events instead, so the touch point is **never erased** from
the `static std::map` at `WebCoreDriver.cpp:3158`. Every later event then carries N points — pages read
it as a pinch — and the map grows unboundedly. Route both to phase 3 (TouchCancel), which the driver
already handles.

---

## Phase 3 — the video bug

**This is the highest-value functional fix in the project.** Video decodes at full rate and draws zero
frames in-page; a standalone `.mp4` URL plays fine.

Static analysis has **eliminated** the texture-upload block as the cause:
- `m_fsFrameDirty` is set at 38fps and cleared only inside the block — cannot be stale-false.
- `m_fsPixels.size() >= fw*fh*4` is an **identity**: the writer sets W, H and the buffer together under
  one lock, sized to exactly `w*h*4`.
- `ensureFrameServerTexture` returns false only if `glGenTextures` yields 0, and never checks
  `glGetError` — it is a silent-*true* path, not a silent-false one.

So the block is **never reached**. Route per T-0.1.

### T-1 · Video layer never promoted → refresh the accelerated-rendering cache
`RenderVideo::supportsAcceleratedRendering` reads `HTMLMediaElement`'s **cached** flag, refreshed only
in `mediaEngineWasUpdated()` (`HTMLMediaElement.cpp:5323`). `MediaPlayer.cpp:610` fires
`mediaPlayerEngineUpdated()` **before** `m_private->load()` at `:625` — and `m_useFrameServer` is still
false at that moment, so the cache is computed **false**. `renderingModeChanged()` only invalidates
style; it does not refresh the cache. Whether the compositing update lands before or after the scheduled
refresh is a **race** — a page with ~50 composited layers and heavy JS resolves it differently from a
two-element MediaDocument. *Hypothesis, but it is the only surviving explanation for
"standalone works, in-page doesn't".*
Fix at the media/compositing seam, **not** in the GL code: refresh the cache from
`mediaPlayerRenderingModeChanged`, or follow `renderingModeChanged()` in `load()` with an explicit
`contentChanged(VideoChanged)`.

Secondary gate to check while there: `requiresCompositingForVideo` requires `video.shouldDisplayVideo()`
(i.e. not showing a poster). rule34 posts ship a `poster`; a MediaDocument does not.

### T-1b · Composited but not painted
`TextureMapperLayer::paintSelf` early-returns on `!m_state.visible || !m_state.contentsVisible` or an
empty **layer** rect (not contents rect).

### T-1c · `m_useFrameServer` false at paint time
Falls through to `if (!m_deviceSharedWithAngle) return;` — and **`m_deviceSharedWithAngle` is declared
`{ false }` and never assigned `true` anywhere in the tree**. Lines ~1512-1550 are dead code that
returns silently with no log. Delete the branch or make it log.

### T-3 · Add a software fallback for the frame-server path
Independent of the above, and worth doing regardless. `paint()` fills `m_lastFrame` only inside a
`#if USE(CAIRO)` block guarded by `m_mediaEngine` — and a frame-server player **never creates one**
(`load()` returns before `createMediaEngine()`). So if the layer is not composited, **nothing draws,
ever**. `paint()` should draw from `m_fsPixels` when `m_useFrameServer` is set. This turns a total
blank into degraded-but-visible video and removes a whole class of "video invisible" failure.

---

## Phase 4 — bound the unbounded (memory)

Two of these are safe **without** waiting for Phase 1's measurements, because the code is unambiguously
unbounded regardless of what the numbers say.

### T-4.1 · Cap and evict `PortCacheStorage` — strongest suspect for the unattributed memory
`port/PortCacheStorage.cpp:211` holds `HashMap<uint64_t, unique_ptr<PortCacheEntry>>` where each record
carries the **full response body**. It is: process-wide, memory-only, **never persisted**, with **no cap,
no quota, no LRU, no eviction**, **never cleared on navigation**, and **absent from
`MemoryRelease.cpp`** — so no release actuator can touch it. Every service-worker site (YouTube, Google,
Twitter, Reddit) calls `cache.put()` on its JS bundles, fonts and images. Functionally a leak.
Matches the observed symptom precisely: 17 releases reclaiming nothing.
Add a byte cap with LRU eviction, and include it in `releaseMemory`.

### T-4.2 · MSE has no forward-buffer cap
`SourceBufferPrivateMediaFoundation::evictCodedFrames` is an **empty function body** — WebCore's entire
MSE quota mechanism is disabled because samples live inside MediaFoundation, not in WebCore's
`TrackBuffer`s. The page appends as far ahead as its ABR logic wants. The only counterweight is
`PortMseEmergencyTrim` at **≥92% of the effective ceiling** (~15MB from death), and it trims only
*behind* the playhead. Add a forward trim at a normal threshold (~75%) using the ranges
`MseSourceBuffer::Buffered` already provides.

### T-4.3 · Subscribe to `AppMemoryUsageLimitChanging`
The limit **drops at runtime** when the app backgrounds (observed: 390MB → ~195MB, putting the app
instantly at ~110% of cap). We detect it by polling every 250ms and only log it after the fact.
`AppMemoryUsageLimitChanging` fires **before** the new limit is enforced and carries `OldLimit`/`NewLimit`
— the documented hook for shedding in time. Also consider `AppMemoryUsageIncreased/Decreased` to replace
three cross-ABI WinRT property reads **per frame** (~180/sec at 60fps) in `onRendering`.

### T-4.4 · Stop purging font data on marginal releases
`glyph=80ms/691(4851glyphs)` works out to ~116µs/call, consistent with a **cold cache** rather than
steady-state drawing — the glyph cache is being repeatedly emptied by our own `releaseMemory` calls,
then re-rasterised. The fix is not to optimise the glyph path; it is to stop purging fonts on releases
that are not genuinely critical.

---

## Phase 5 — larger items, ordered by value

### T-5.1 · `@font-face` is completely non-functional — root cause found
`OpenTypeUtilities.cpp:271-277`: on UWP the GDI activation branch is compiled out and hardcodes
`HANDLE fontHandle = nullptr;` then `if (!fontHandle) return { };`. Font activation therefore **always**
fails → `createFontCustomPlatformData()` always returns null → **every downloadable font fails**. This is
the "tofu": icon fonts (Font Awesome, Material Icons) have no fallback family by design, so they render
as blank boxes on a large fraction of the web.
Fix: `IDWriteFactory::CreateCustomFontFileReference` + an in-memory `IDWriteFontFileLoader` /
`IDWriteFontCollectionLoader`. **Not** WinRT-restricted; available in the UWP `dwrite.h` surface.

### T-5.2 · `preventDefault()` on touch does nothing
`WebCoreDriver.cpp:3180` discards `handleTouchEvent`'s return value, and the shell fires touchmove **then**
`ScrollBy` unconditionally (`RenderShellXaml.cpp:813-818`). A carousel, map or custom scroller that claims
the gesture gets it *and* the page scrolls underneath. Same for `touchstart` → synthetic click, giving a
double-fire. Plumb the bool back and gate the `ScrollBy`/`Tap` calls on it.

### T-5.3 · ICU's 32MB data blob is linked into three binaries
`icudt.lib` appears in WTF, JavaScriptCore **and** the shell: `WTF.dll` 33MB, `JavaScriptCore.dll` 42MB,
`WebCoreRenderShell.exe` 63MB, each carrying ~4,210 `icudt77` symbols. ~64MB of wasted image **and three
independent ICU instances** with separate collator/break-iterator caches — and `ubrk_*` is on the layout
path. Link it into one module, or map it at runtime via `udata_setCommonData()`.

### T-5.4 · No `mousemove` / `pointermove` entry point at all
The entire input ABI is scroll/tap/touch/key/char. Sliders, drag-to-dismiss, canvas drawing and any
pointerdown→move→up interaction receive **down then up with nothing between**. `:hover` never activates.
Touch also never produces PointerEvents, because `PointerCaptureController.cpp:199` gates that path on
`PLATFORM(IOS_FAMILY)`.

### T-5.5 · Soft keyboard occludes the page
Show/hide is correct (`InputPane::TryShow/TryHide`, and the editable predicate covers all three kinds),
but there is **no `InputPane::Showing`/`Hiding` subscription and no `OccludedRect` use**. The SIP covers
roughly the bottom half of the screen, the viewport is never resized, and nothing scrolls the focused
field into view. Typing into any field in the lower half means typing blind.
Related: `PortShowKeyboard(int)` takes only a bool, so `<input type=email/number/tel/url>` all get the
generic QWERTY layout. W10M honours `InputScope` well and this is cheap.

### T-5.6 · No selection, clipboard or context menu
`PortPlatformStrategies.cpp:83` — `createPasteboardStrategy()` returns `nullptr`. Nothing uses
`Windows.ApplicationModel.DataTransfer.Clipboard`. `PortEditorClient` denies DOM paste and hardwires
undo/redo off. No long-press handler, so `ENABLE_CONTEXT_MENUS` is on but nothing triggers a menu.
The user cannot copy a URL or paste a password.

### T-5.7 · No zoom of any kind
No `setPageScaleFactor`, `ViewportConfiguration`, pinch or double-tap handling anywhere. At a fixed
375 CSS px viewport, a desktop-layout site is permanently unusable with no recovery path.

### T-5.8 · Session state is never persisted
`OnLaunched` takes its args **unnamed**, so `PreviousExecutionState` is never read — the app cannot tell
"OS reclaimed us" from "fresh launch". The start URL is hardcoded to Google.
`ApplicationData::LocalSettings` is used **nowhere in the tree**. `TabManager` holds tabs in memory with
no serialisation. The suspend handler is the natural place to write `{active url, tabs, scrollY}`.

### T-5.9 · No `<Extensions>` in the manifest
No `windows.protocol` for http/https (no other app can hand Revenant a URL, and it cannot appear in
"open with"), no `windows.shareTarget` (the normal W10M way to send a link to a browser), no file-type
associations.

### T-5.10 · File upload and downloads are empty function bodies
`PortChromeClient::runOpenPanel` is `{ }`. Under AppContainer, `FileOpenPicker` is the **only** way to
obtain a broker-granted handle to a user file — so this is not merely missing UI, the required API is
absent. `convertMainResourceLoadToDownload` and `startDownload` are likewise empty; the UWP-correct
implementation is `FileSavePicker` + `BackgroundDownloader` so transfers survive suspension.

### T-5.11 · Try `/GL` + `/LTCG`
Absent everywhere, and structurally so — WebKit's CMake only wires LTO for Clang. Compounding it, JSC and
WTF are **DLLs** while WebCore is static in the shell, so every WebCore→JSC call is an IAT-indirect call
with zero cross-module inlining, on an in-order CPU. Given `js=2337ms`, plausibly the largest
build-level lever available.
**Risk:** the shell links individual `.obj` files out of `WebCore.dir`; with `/GL` those become IL
objects, and combined with `/FORCE:MULTIPLE` this is untested territory. Expect long links and high
linker memory. Try after the cheap wins, not before.

### T-5.12 · Convert the detached watcher thread to `ThreadPoolTimer`
`RenderShellXaml.cpp:420` spawns a detached `std::thread` that calls `RoInitialize` with **no matching
uninitialize**, never exits, has no cancellation, polls across suspension, and calls back into the engine
(`PortMseEmergencyTrim`) from a thread the platform does not know about. Under PLM the OS can freeze it
mid-trim while it holds MSE state. `ThreadPoolTimer::CreatePeriodicTimer` is the correct primitive and
the codebase already uses `ThreadPool::RunAsync` elsewhere.

### T-5.13 · Unbounded on-disk growth
`debug.log` is opened append-only with **no rotation or size cap** (multi-MB per session, forever).
`dfgdis.txt` is written unconditionally on every run. `localStorage` quota is hardcoded off
(`PortStorageProvider.cpp:137` sets `quotaException = false`), so a page can write until the flash fills.
Consider moving the HTTP and bytecode caches to `LocalCacheFolder`/`TemporaryFolder` so the OS can
reclaim them under disk pressure.

---

## Phase 6 — incomplete APIs (scan of 2026-07-22)

Found by reading the port + mediastream layers. **Verified by reading each function** — an automated
scan for empty bodies had a high false-positive rate (`createFrame`, `userAgent` and the ServiceWorker
fetch client all read as "empty" and are fully implemented), so only confirmed items are listed.

### T-6.1 · `shouldGoToHistoryItem` returns `false` — ALL back/forward navigation is dead
`port/PortFrameLoaderClient.cpp:731`. `HistoryController::goToItem` — the function behind every
back/forward navigation — does:
```cpp
if (!m_frame.loader().client().shouldGoToHistoryItem(targetItem))
    return;
```
We inherited the `EmptyClients` null-client default (`EmptyClients.cpp:1031` returns false), which is
correct for a do-nothing client and wrong for a real one. WebKitLegacy/win returns `true`.
**One-line fix.** Affects the hardware Back button, `history.back()` and `history.go(-1)`.

This also explains the reported "hardware back just closes the app", which the app-model audit could not
account for (it correctly found `SystemNavigationManager::BackRequested` *is* registered):
- **mid-session:** `canGoBackOrForward(-1)` is true → the shell marks the event handled and calls
  `goBackOrForward(-1)` → blocked here → **back appears dead**.
- **start of history:** `canGoBack` false → event unhandled → the OS closes the app.

### T-6.2 · Form controls compiled in but non-functional
`port/PortChromeClient.cpp` — all return `nullptr`, while the features are **enabled** in
`cmakeconfig.h`:
- `createColorChooser` (`:77`) with `ENABLE_INPUT_TYPE_COLOR 1` → `<input type=color>` does nothing
- `createDataListSuggestionPicker` (`:81`) with `ENABLE_DATALIST_ELEMENT 1` → `<datalist>` dead
- `createDateTimeChooser` (`:85`) — `ENABLE_DATE_AND_TIME_INPUT_TYPES` is **not** defined, so this stub
  is moot unless that flag is turned on

### T-6.3 · HTTP authentication cannot be answered
`canAuthenticateAgainstProtectionSpace` → `false` (`:275`) and
`dispatchDidReceiveAuthenticationChallenge` → empty (`:269`). Any site behind 401 Basic/Digest is
unreachable: no prompt, no way to supply credentials. `shouldUseCredentialStorage` → `false` (`:259`).

### T-6.4 · WebRTC: signaling and data channels only, no media either way
Deliberate phase-1 scoping (documented in `LibWebRTCProviderWinCairo.cpp`), recorded here so the
remaining work is explicit. **Working:** `RTCPeerConnection`, DTLS identity (`rtc::InitializeSSL()` is
called once — added because Turnstile's probe faulted without it), ICE gathering (device log:
`candidates=16`, `setLocalDescription ok`), builtin **audio** codec factories.

**Stubbed:**
| Stub | File | Consequence |
|---|---|---|
| all 3 capture factories return error strings | `mediastream/RealtimeMediaSourceCenterWinCairo.cpp` | `getUserMedia`: no camera, mic or screen share |
| `audioSamplesAvailable` → `{ }` | `mediastream/libwebrtc/RealtimeMediaSourcesWinCairo.cpp` | outgoing audio never sends samples |
| `videoSampleAvailable` → `{ }`, `createBlackFrame` → nullptr | same | outgoing video never sends frames |
| **`OnFrame` → `{ }`** | same | **incoming video frames are received and discarded** |
| `createEncoderFactory` / `createDecoderFactory` → nullptr | `mediastream/libwebrtc/LibWebRTCProvider.cpp:377-385` | no video codecs at all |

The audio device module is the dummy `LibWebRTCAudioModule`, so there is no real audio I/O even with
codecs present. Net effect: a call negotiates, gathers candidates and completes DTLS, then nothing
renders. `RTCDataChannel` (SCTP) is the one path that needs no codecs — **untested, not claimed**.

Note `OnFrame` being empty is the WebRTC twin of T-1: the frame arrives and never reaches the screen.

**Manifest dependency:** `microphone`/`webcam` are correctly *absent* today, matching the capture stubs.
They become **required** the moment capture is implemented — see T-5.9.

### T-6.5 · Already tracked elsewhere, confirmed at source
`runOpenPanel` empty (T-5.10), `startDownload` + `convertMainResourceLoadToDownload` empty (T-5.10),
`dispatchCreatePage` → nullptr (`window.open`), `createPasteboardStrategy` → nullptr (T-5.6).

### Verified COMPLETE — do not chase
`createFrame` (full subframe implementation with child client wiring), `userAgent` (full identity
logic), `PortServiceWorkerFetchClient::didReceiveResponse` / `didReceiveData` / `didNotHandle` (all
real). `canCachePage` → `false` is **deliberate** — the back/forward cache is disabled by design
(`BackForwardCache::setMaxSize(0)`).

---

## Explicitly NOT worth doing

- **Force `CompositingPolicy::Conservative`.** It only affects 3D transforms, canvas and `will-change` —
  **overlap is untouched**, and 87% of promotions here are overlap cascade. A code comment at
  `RenderLayerCompositor.cpp:1838` already records the measurement ("336 composited layers … Conservative
  did not stop them"). The port already ships the effective lever by dropping `FilterTrigger` in
  `PortChromeClient.h:39`. This was on the plan and was removed after being disproven.
- **Enable concurrent GC on ARM32.** The upstream guard is exactly the JSVALUE64 platform set —
  concurrent marking must read a JSValue atomically, and on 32-bit it is two words. Same hazard that made
  concurrent JIT unsound here, but in the collector, where the failure mode is heap corruption. The
  low-core GC tuning is also inert: those options are read only by the two *concurrent* schedulers, never
  by the stop-the-world one this port actually runs.
- **Port bmalloc.** Not a config oversight: `VMAllocate.h` calls `mmap()` unconditionally and there is
  **no `PlatformWin.cmake` in `Source/bmalloc/`**. It was never built for any Windows port of WebKit.
  Instrument the `fastMalloc` funnel instead (T-1.x).
- **MSVC PGO.** Requires `/GL` + `/LTCG` first, and the PGO runtime writes its `.pgc` to a path baked in
  at link time — inside an AppContainer that is very likely undeliverable. Cross-architecture profiles
  are not an option. Check whether v142 even ships an ARM32 `pgobootrun.lib` before spending time here.
- **Chase `swapInterval`, the frame-accounting `GAP`, or the tile raster scheduler.** All done and
  verified.

---

## Open questions needing device evidence

| Question | How to settle |
|---|---|
| Is the video layer composited in-page? | T-0.1 greps — no build needed |
| Is the unattributed memory private commit or image/mapped? | `vmwalk:` (T-1.4) |
| Is `PortCacheStorage` the bulk of it? | `cachestorage:` (T-1.3) |
| Does `HeapSummary` resolve under AppContainer? | T-1.5 — log either outcome |
| Does the SIP's autocorrect survive as backspace+retype? | Type "teh " into a web `<input>`, read `INPUT:` lines |
| Does the URL TextBox steal typed characters? | Tap a web field, type, watch whether the address bar changes |
| ~~Is hardware Back actually broken?~~ | **ROOT-CAUSED — see T-6.1.** The shell hook is fine; `shouldGoToHistoryItem` returns `false` so the navigation is blocked one layer deeper. One-line fix. |
| Does video work on the 950? | If yes and it fails on the 640 XL, that supports the T-1 race hypothesis |
