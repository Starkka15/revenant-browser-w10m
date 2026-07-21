# Known Issues & Good First Areas

A curated list of the open problems, roughly ordered by "impact per effort," with pointers into the
source so you're not cold-starting. Difficulty is relative to *this* codebase (all of it assumes
you can build + deploy per [BUILD.md](BUILD.md)).

File paths are relative to the WebKit tree (`WebKit-2.36.8/`) after applying the patch + `src/` files.

---

> **The ordered plan for picking this back up is [TASK.md](TASK.md)** — phased, with the evidence and
> effort behind each item, and a list of things already ruled out. This file is the *state*; TASK.md is
> the *plan*.

## Status as of 2026-07-20 (start here)

The tree builds, deploys and runs on a Lumia 640 XL. Everything below is measured on device, not
inferred. **The single most useful habit for anyone picking this up: read the whole `debug.log`, not
just the lines you expect.** Nearly every wrong turn in this project came from grepping for a theory
instead of reading what the log actually said.

### The headline open bug: video decodes but never draws

    video: decoded=38.3fps drawn=0.0fps (0/77)
    gate: last97 composited=97 skipped=0

MediaFoundation decodes fine and we composite every frame, yet zero video frames reach the screen.
A standalone MP4 URL **does** play — the failure only happens with video embedded in a page. The draw
path bails at `MediaPlayerPrivateMediaFoundation::paintCurrentFrameToTextureMapper`: the upload block
(`m_fsFrameDirty && fw>0 && fh>0 && m_fsPixels.size() >= fw*fh*4 && ensureFrameServerTexture(...)`)
never succeeds, so `m_frameHasContent` stays false and the function early-returns. Proof: the
`fsvid: draw tex=…` diagnostic just past that return appears **zero** times in a 12k-line log.
Next step is to log all four terms separately in one build — do not guess which one fails.

Related and possibly causal: a rule34 post starts **three** `<video>` pipelines at once. The tier-0
single-decode slot (`g_tier0ActivePlayer`) was unsynchronised across the curl-worker, threadpool and
main threads, so all three raced past the check and created players. That is now locked
(`s_tier0SlotLock`), but the fix is **untested on device** — verify at most one `frame-server started`
per eviction before trusting it.

### Verified working this session

- **HTTP/2** — was silently OFF (the BoringSSL migration branched from a pre-HTTP/2 config script).
  149 responses now negotiate h2. See BUILD.md's warning about the `-h2` curl scripts.
- **HTTP disk cache** — `CurlCacheManager` was compiled in, wired into the loader, and permanently
  disabled because nothing ever called `setCacheDirectory()`. Now enabled (64MB under `LocalState`).
- **Bytecode cache** — went from `hit=0 miss=25 (0%)` to `hit=23 miss=79 (22%)`. It was only ever
  written on *eviction*; it now flushes on navigation-complete and on suspend.
- **Memory-release storm** — releases 20 → 7 per session, stop-the-world GC 1398ms → 453ms.
- **Frame accounting** — `GAP` is now 0; the previously unexplained time was the RunLoop block plus an
  untimed rendering update. A worst-case frame reads `js=2337 update=883 composite=243`, i.e. **JS and
  layout dominate; raster is ~7%**. Optimise accordingly — the compositor is not the problem.

### Known-bad, unfixed

- **AdaptiveMediaSource cannot resolve DNS** (`0x80072ee7`). AMS uses `Windows.Web.Http` → the system
  resolver, bypassing the DoH workaround (`CURLOPT_DOH_URL`) that curl needs on this device. Any HLS
  that reaches AMS fails on the network layer.
- **~250MB of native heap is unattributed.** `mempool:` shows `jsHeap`/`resCache`/`gpuTiles` totalling
  ~100MB against 350MB in use; the rest is CRT heap invisible under `USE_SYSTEM_MALLOC`
  (`fastMallocStatistics()` returns 0). This is what OOM-kills the app on heavy pages, and no cache
  release touches it. Needs `HeapWalk`-based reporting before anything can be fixed.
- **Layer explosion:** 87% of compositing promotions are `[overlap,]` cascade — 50 layers / 39MB of
  GPU tiles on one page. `Page::setCompositingPolicyOverride(Conservative)` is never called and
  probably should be on tier-0.
- **YouTube stops after ~1 minute.** Long-standing, cause unknown, MSE path.

---

## 1. Some videos may not play (native HLS)  ·  impact: high  ·  difficulty: medium
**State:** progressive MP4 and MSE fMP4 now play (ads, direct `<video src=…mp4>`, and MSE sites).
The remaining gap is **native HLS** (`application/vnd.apple.mpegurl`): we now advertise it in
`canPlayType` and route `master.m3u8` to WinRT `AdaptiveMediaSource`, but AMS creation currently
fails on-device (`hls: AMS create FAILED status=1 hr=0x8019019c`). Sites that only serve HLS to our
iOS-Safari identity (e.g. Pornhub main videos) therefore stay blank while their MP4 ads play.

**Where:**
- `Source/WebCore/platform/graphics/win/MediaPlayerPrivateMediaFoundation.cpp` — `load()` HLS branch
  (`hls: load via AdaptiveMediaSource`), and `supportsType`/`mfSupportsTypeImpl` (HLS now advertised).
- `Source/WebCore/shell/ShellMse.cpp` — `PortHlsPlayerStart` builds the `AdaptiveMediaSource`; the
  `AMS create FAILED hr=0x8019019c` is thrown here. Start by decoding that HRESULT and checking the
  manifest fetch (it must go over our curl/DoH stack, not the OS resolver).

**Also related — streaming fetch:** large / long-lived streaming `response.body` fetches can still
fail with a JS `NetworkError` even when the transfer succeeds; see `FetchResponse.cpp`
(`BodyLoader::didFail`, `fetchfail-webcore:` log) and the curl BUFFER→FLUSH delivery in
`CurlRequest.cpp` (`len=-1` streamed bodies).

**Where:**
- `Source/WebCore/Modules/fetch/FetchResponse.cpp` — `BodyLoader::didFail()` has a `fetchfail-webcore:`
  log that prints the failing URL + error type/code/cancelled/timeout. Start here to see *which*
  request and *why*.
- `Source/WebCore/platform/network/curl/CurlRequest.cpp` — the response BUFFER→FLUSH streaming
  delivery (`didReceiveData` / `didCompleteTransfer`). Suspect: how streamed/`len=-1` bodies are
  handed to the ReadableStream vs. completed.

**Good because:** it has a clean on-device repro, existing diagnostics, and it's a *general* fix
(helps webmail, APIs, anything using streaming fetch), not YouTube-specific.

---

## 2. MSE video: zero-copy compositing  ·  impact: high  ·  difficulty: hard
**State:** MSE plumbing works and plays; the live path is a CPU readback (choppy). A zero-copy path
(decode → ANGLE-shared D3D texture → GL composite) exists in the tree but is **dormant** because
enabling it regressed stability. (The earlier "app hard-closes before the first frame" reports were
largely a separate memory kill + a synchronous seek-recursion stack overflow, both since fixed —
see the git history — so this is worth revisiting.)

**Where:**
- `Source/WebCore/platform/graphics/win/MediaPlayerPrivateMediaFoundation.cpp` — `paintCurrentFrameToTextureMapper()`
  has the frame-server branch (guarded off); `supportsAcceleratedRendering()` gates CPU vs zero-copy.
- `Source/WebCore/shell/ShellMse.cpp` — WinRT `MediaPlayer` frame-server; `PortMsePlayerCopyFrame` /
  `PortMsePlayerNativeSize` are the zero-copy entrypoints; `onVideoFrame` is the CPU-readback path.

**Task:** figure out why turning on `supportsAcceleratedRendering()` for the frame-server closes the
app before a frame arrives (likely the accelerated video-layer setup at HAVE_METADATA), then land
zero-copy for smooth playback. Depends on #1 for reliability.

---

## 3. `window.open` / popups  ·  impact: high  ·  difficulty: medium
**Symptom:** "Sign in with Google/Microsoft" (OAuth) and other popup flows don't work — a big chunk
of real-world logins.

**Where:** `Source/WebCore/port/PortChromeClient.cpp` (`createWindow` is not implemented). Needs a
second WebView/window surface in the shell (`Source/WebCore/shell/RenderShell*.cpp`). Self-contained
and high-value.

---

## 4. Cloudflare / anti-bot  ·  impact: high  ·  difficulty: hard
**Symptom:** challenge-gated sites ("checking your browser" / Turnstile) may not pass — TLS/HTTP2
handshake fingerprint + obfuscated JS/WASM attestation.

**Where:** `Source/WebCore/platform/network/curl/` (TLS/ALPN/HTTP2 setup). This is the same class of
problem as YouTube's proof-of-origin tokens. Hard, open-ended, but high-value; even partial progress
(matching a common browser's TLS fingerprint) helps.

---

## 5. File upload (`<input type=file>`)  ·  impact: medium  ·  difficulty: easy–medium
**Where:** `Source/WebCore/port/PortChromeClient.cpp` (`runOpenPanel`) → wire a UWP `FileOpenPicker`
in the shell. Small, self-contained.

## 6. JS dialogs — `alert` / `confirm` / `prompt`  ·  impact: medium  ·  difficulty: easy
**Where:** `Source/WebCore/port/PortChromeClient.cpp` (the `runJavaScript*` hooks) + a shell
`ContentDialog`. Good first PR.

## 7. Browser chrome / UX  ·  impact: medium  ·  difficulty: medium
The shell is minimal — no tabs, basic address bar, no back/forward/reader/settings UI. This is the
"make it feel like a browser" work. `Source/WebCore/shell/RenderShell*.cpp` (C++/CX).

## 8. Performance on ARM32  ·  impact: high  ·  difficulty: hard / open-ended
JS-heavy sites peg the main thread (these are ~2013 CPUs). Areas: reducing main-thread JS stalls,
the GPU-offload compositing gate and tiled/GPU scroll (`Source/WebCore/port/WebCoreDriver.cpp`), and
anything that keeps the UI thread responsive under load. The broad, never-done one.

---

## Smaller / assorted
- `x.com` ANGLE device-loss handling; cellular-vs-WiFi networking; Web Audio (WASAPI/AudioGraph
  output backend — flag exists, backend not wired); geolocation / clipboard / notifications /
  share platform hooks. Most live in `Source/WebCore/port/` clients or the shell.

If you pick something up, opening an issue to claim it helps avoid overlap. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the patch-based PR workflow.
