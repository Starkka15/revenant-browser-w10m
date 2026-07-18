# Revenant — Known Issues Tracker

Bugs/gaps found from on-device logs. Ranked within each group. Status: 🔴 open · 🟡 in progress · 🟢 fixed (unverified) · ✅ verified on-device.

## Crashes / stability

### I-01 · YouTube video crash = GPU texture OOM at high DPR 🟡
**Device:** HP Elite X3 (Snapdragon 820, **Adreno 530**, 4GB RAM but W10M caps app at 2048MB → tier=2;
DPR **3.5**, 1440p 5.96"). **Site:** m.youtube.com/watch.
**Evidence:** `GLERR ... HRESULT: 0x8007000E (E_OUTOFMEMORY): Error allocating Texture2D` → `GL_OUT_OF_MEMORY 0x0505` → C++ `_com_error` bursts at `stage=c5:layerPaint` (caught internally, `RENDER-CRASH` never fired) → mass `TEXDEL` eviction of 1442×2000/1442×2140 tiles → failure reaches `x9:xaml-commit` (SwapChainPanel present, **UI thread — outside render-thread SEH**) → OS kills process → relaunch. System RAM was fine (31%); this is **GPU** texture memory.
**Root:** at DPR 3.5 every backing store is a 3.5× D3D texture; YouTube watch page's many big layers exhaust the app's GPU texture budget. New failure axis, orthogonal to the system-memory tiering (a tier-2 device with a huge high-DPR screen OOMs the GPU where a tier-0 low-DPR device wouldn't).
**Fix (in progress):** (a) cap the **raster/composite scale** on very-high-DPR devices — render backings + EGL surface at ≤~2.5×, let the SwapChainPanel upscale to physical; keep cssW=412 (preserve mobile layout + iOS impersonation). ~2× texture-byte cut. (b) harden the `x9:xaml-commit` present path (SEH/try-catch so a present failure drops a frame, not the app). (c) on Texture2D OOM, proactively evict the tile pool + cap the tile budget instead of retrying to alloc.

### I-11 · tier-0 memory reaps on heavy-media sites ✅ FIXED+VERIFIED (640XL) (GOAL: 1GB ≈ 2GB/3GB)
**Verified 2026-07-18 on 640XL:** same heavy session (pornhub + livejasmin galleries) → **1 launch (no
reap; was 3), peak 308MB (under the 390 limit; was 408→reap), memory now oscillates ~190-308MB** — 10
`releaseMemory` cycles each drop ~80-120MB (`285→201`, `308→189`) as `_heapmin` hands the CRT heap back
to the OS. Ratchet eliminated.
**Device:** 640XL (1GB, tier-0, 390MB limit / 340 effective). **Session:** pornhub + chaturbate + redgifs.
**Evidence:** `mempool:` line at reap = `jsHeap=42MB resCache=16MB gpuTiles=22MB gpuPool=0 malloc=0KB
appUsage=408MB lvl=3` → OS PLM-reaped the app (2× → 3 launches), abrupt (no shutdown log = OS reap, SEH
can't see it). Accounted pools = ~80MB; **~328MB is native "other" the log can't attribute** because
`malloc=0KB` (this build isn't fastMalloc, so `fastMallocStatistics().committedVMBytes` reads 0).
Trajectory: "other" is a persistent ~180-226MB baseline that SPIKES to 328MB at the reap. NOT video
(1 player alive, being torn down), NOT decoded images (resCache incl. live decoded = only 16MB), NOT
tiles at the reap (22MB; they do peak 121MB elsewhere). So the hog is diffuse native heap — candidates:
cairo raster/ImageBuffer surfaces (55 composited layers software-rastered), DOM/render tree, WebCore
object heap, filter (boxBlur) transient buffers, driver/ANGLE.
**ROOT CAUSE FOUND (instrumentation build):** the `mempool:` `other=` proved it — cold-start baseline is
only 64MB, but "other" grows to ~200MB while browsing and DOESN'T shrink. Not a live pool (images 8MB,
tiles 13MB, video torn down). The build is `USE(SYSTEM_MALLOC)` and **`releaseFastMallocFreeMemory()` was
an empty stub** — so freed allocations sit in the UCRT heap committed-but-unused and never return to the
OS → ~136MB of "committed free" heap accumulates → process blows the 390MB PLM limit → OS reap.
**FIX (WTF/wtf/FastMalloc.cpp):** implement `releaseFastMallocFreeMemory()` → `_heapmin()` (compacts the
CRT free lists, decommits pages). WebCore's `releaseMemory(critical)` + the memwatch high-water already
call it (was a no-op) — now it actually returns the heap. Confirmed `releaseMemory` drops appUsage
(224→178MB) even before this; the fix makes those drops return to the OS. Built, needs 640XL verify.
(cairoSurf= hook reads 0 — it's on the software `BackingStoreBackendCairoImpl` path; this GL-composited
port rasters via BitmapTexture. Left in; harmless. The `other=` split is what cracked it.)

### I-02 · pornhub age-gate blur freeze (tier-0) 🟢 (Fix A shipped, survivable)
640XL: full-page `filter: blur()` software-rastered every frame → 3-15s `boxBlur` main-thread burns. Fix A (tier-0 filter-scale pixel-budget clamp) cut freeze→survivable jank. Still ~3s; could go further. See `RenderLayerFilters.cpp`.

### I-03 · MSE teardown deadlock made a hung app unkillable 🟢
`PortMseDestroy` closed the MF player synchronously on the main thread during `Document::commonTeardown` → 180s+ `IDLE(deadlock/blocked)` wedge. Fixed: close on a threadpool thread (`ShellMse.cpp`).

## Fingerprint / anti-bot

### I-15 · rule34 (CF Turnstile) freeze = JS-fingerprint tells → re-challenge storm 🟡 (fixes #1-3 built)
**Device:** 640XL. **Site:** rule34.xxx (CF Turnstile). Page loads (content served) but the Turnstile
challenge re-runs (`/turnstile/f/.../rch/` hit 41×, `jsd/main.js`) → `MAIN-STALL cpuDelta=603750ms` (~10
min JS burn) → main thread pegged → images never composite → the empty-thumbnail grid. User: "verification
failed but loaded anyway." The TLS/JA3 layer ([[revenant-tls-ja3-cloudflare]]) passes; this is Turnstile's
**JS-behavior** bot-detection failing our iOS-Safari-15.4 impersonation. FPDIAG (ran INSIDE the Turnstile
iframe) vs real iPhone tells, ranked:
1. `dpr=1.75` — impossible for iPhone (2 or 3). **FIXED (#3):** iPhone SE mapping, dpr 2.
2. `screen=412×612` — 412 is Android; no iPhone. **FIXED (#3):** 375×667.
3. `Notif=function` — iOS Safari has no Notifications until 16.4 (we present 15.4). **FIXED (#1):**
   `setNotificationsEnabled(false)` → `window.Notification` undefined.
4. RTC candidates leak raw local IPs (`192.168.5.17`); Safari mDNS-obfuscates to `*.local`. **TODO (#4).**
5. `inner=0×0` (Turnstile iframe) reads as headless. **TODO (#5).**
6. `MAXTEX=4096` (Adreno-305 limit under iPhone UA; iPhone=16384). **FIXED (#2):** WebGL size-param mask.
**Approach:** honest coherence — make the JS-visible identity match a real iPhone SE / iOS 15.4, no
heuristics. #3 (viewport) also subsumes the I-01 GPU-texture cap (raster now fixed at 2×375). #1-3 built;
needs 640XL verify (render OK + Turnstile stops re-challenging). #4 (WebRTC mDNS) + #5 (iframe inner) next.

## Media

### I-14 · Rule34 video-page FREEZE — codec gate not applied to direct <video>, fluidplayer retry-storm 🟢 (fix built)
**Device:** 640XL. **Site:** rule34.xxx (after I-13 stopped the crash, the page then froze). **Evidence:**
`mf: supportsType codecs="hev1..." -> 1`, `vp9 -> 1`, `av1 -> 1`, `vp8 -> 1` — the direct-<video>
`canPlayType` claimed support for codecs the Adreno 305 (SD400) can't decode. fluidplayer PICKED one (the
video was 1920x1080), MF failed it (`MediaFailed hr=0xc00d36c4`), fluidplayer's watchdog timed out →
recreated the player 4× in ~50ms (`frame-server started` ×9, thrash) → single core pegged, compositor
starved (`decoded=2.9fps drawn=0.0fps`, `JS=41533ms/41879ms window = 99%`) → page freeze.
**Root:** `mfSupportsTypeImpl` applied the `mseCodecsUndecodable` gate ONLY inside the `isMediaSource`
branch. The direct-<video> fall-through (webm/vp9 → line 352, mp4/hev1 → line 360) returned Supported for
everything in the MIME cache — an assumed "Store codec extensions" path that doesn't exist / is too slow
on SD400. (Note: also incoherent with our iOS-Safari identity, which doesn't do VP9/WebM/AV1.)
**FIX (MediaPlayerPrivateMediaFoundation.cpp):** on tier-0, gate the direct-<video> path too — reject
`mseCodecsUndecodable` codecs + hev1/hvc1 (HEVC, no HW decode on SD400) → canPlayType reports only
H.264/AAC → sites serve the H.264 rendition. Tier-aware: tier-1/2 (Adreno 530) keep VP9/HEVC. Not a
heuristic — honest hardware-capability reporting. Needs 640XL verify.

### I-13 · Rule34 video-select CRASH — use-after-dispose in async player teardown 🟢 (fix built) [REGRESSION from I-03]
**Device:** 640XL. **Site:** rule34.xxx (worked in older builds). Selecting a video to play closes the app.
**Evidence (crashprobe):** fatal `Platform::ObjectDisposedException` from `vccorlib140_app.DLL`, SEH stack
`MFPlat.DLL → RTWorkQ.DLL → WebCoreRenderShell.exe (IRandomAccessStream::Seek)` — MF's async source-reader
worker (RTWorkQ) called `Seek` on the progressive `HttpRandomAccessStream` AFTER it was disposed.
**Root:** the I-03 async-close moved `delete player` (which synchronously drains MF's source reader) to a
threadpool worker, but the worker's `Source = nullptr` + the caller's `delete h` released the last ref to
the byte stream BEFORE Close finished draining → an in-flight Seek hit the disposed stream. Rule34's rapid
player create/destroy (preview → post page → fluidplayer.js) opens the window.
**FIX (ShellMse.cpp):** capture extra refs to `h->mediaSource` + `h->source` inside BOTH close lambdas
(PortMseDestroy + PortMsePlayerStop) so the stream outlives `delete player`; released only after Close
drains MF. Keeps I-03's no-deadlock benefit. Needs 640XL verify.

### I-12 · Undecodable video freezes the whole page (runaway seek loop) 🟢 (fix built)
**Device:** 640XL. **Site:** livejasmin promo autoplaying an ad MP4 (`adtng.com/.../1194752_video.mp4`,
progressive via frame-server). **Evidence:** player opens (`MediaOpened`, session Playing/Paused) but
delivers **0 frames**; `readyState=4 (HAVE_ENOUGH_DATA)` was reported purely from session state → WebCore
thinks it's fully playable, currentTime never advances → treats it as instantly-ended → (autoplay/loop)
re-seeks at CPU speed: **16,735 `seek settled → SeekCompleted` iterations**, nothing else runs → main
thread pegged → page freezes / "doesn't finish loading."
**Root:** `onFrameServerStateChanged` (MediaPlayerPrivateMediaFoundation.cpp:949) set `HaveEnoughData` from
the bare session state (Playing/Paused) for HLS/progressive, with no frame decoded. A "rolling" session ≠
a decoding one.
**FIX:** session state now only reaches `HaveCurrentData`; `HaveEnoughData` requires REAL progress — a
decoded video frame (`onFrameServerFrame`, already there) OR the position advancing >0.1s
(`onFrameServerTimeUpdate`, added — covers audio-only). Not a timeout/count, so a slow-but-valid stream
just becomes ready when its first output arrives (no false-positive); a genuinely undecodable one stays
honestly "loading" and the page stays responsive instead of freezing. Needs 640XL verify.

### I-04 · Chaturbate / LL-HLS live video won't play 🔴
`hls: AMS create FAILED status=2 (ManifestParseFailure) hr=0xc00d36c4` on `llhls.m3u8`. W10M-era AdaptiveMediaSource predates Low-Latency HLS tags (`#EXT-X-PART`, `#EXT-X-PRELOAD-HINT`, `#EXT-X-SERVER-CONTROL`). **Fix:** fetch the manifest ourselves, strip LL-HLS-only tags → feed a clean standard-HLS manifest to AMS (`CreateFromStreamAsync` + base URI). Codecs are fine (avc1/mp4a supported).

## Web-compat (surfaced on the capable 950, so not hardware-limited)

### I-05 · SVG `px` length rejected → broken icons 🔴 (HIGH visibility)
`Invalid value for <svg> attribute width="24px"` (also 18px/5px) — 40+×, on **YouTube c3_base** + Google Material icons. Static read of `SVGLengthValue::setValueAsString` says `px` *should* parse (parseNumber→24, parseLengthType→Pixels) — it doesn't on-device. **I've inferred wrong 3×; next step = instrument `setValueAsString` (log raw bytes + which step fails), don't guess again.** (rem was fixed separately; px is the live bug.)

### I-06 · WebAssembly missing → Telegram Web broken 🔴
`ReferenceError: Can't find variable: WebAssembly` (web.telegram.org, 8×). The 59 `Cannot access uninitialized variable` (TDZ) errors are **all telegram** too — likely cascading from failed WASM init. JSC has WASM stubbed. Big feature gap (Telegram + any WASM site).

### I-07 · `screen.orientation` undefined 🔴
`undefined is not an object (evaluating 'window.screen.orientation.type')` (8×, anime player). Need the ScreenOrientation API (type + angle + change event) wired to the platform rotation.

### I-08 · Regex named groups unsupported 🔴
`SyntaxError: Invalid regular expression: invalid group specifier name` (3×). Old JSC lacks `(?<name>…)` named capture (and likely lookbehind). Breaks modern JS regex.

### I-09 · console `%s`/`%d` not substituted → hides real errors 🔴 (cheap, high diagnostic value)
67× `js: ERROR %s` — our console capture logs the literal format string, not the substituted message. Fixing the formatter reveals what 67 real errors actually were.

### I-10 · Minor / benign (no fix needed)
CSP `require-trusted-types-for` unrecognized (36×), viewport `viewport-fit` unrecognized (6×), ad-domain XHR CORS blocks — all harmless warnings.

## Features (from user, tracked in REVENANT-TABS-SPEC.md / REVENANT-SUGGESTIONS.md)
- Tab manager (full) — Phase 1 in progress. Fixes the `window.open` new-tab hijack (`createWindow`→nullptr).
- Hardware Back button (W10M `HardwareButtons.BackPressed`).
- Custom URI hand-off (S-001, `LaunchUriAsync`).
