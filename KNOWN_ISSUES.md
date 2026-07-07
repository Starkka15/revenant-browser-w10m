# Known Issues & Good First Areas

A curated list of the open problems, roughly ordered by "impact per effort," with pointers into the
source so you're not cold-starting. Difficulty is relative to *this* codebase (all of it assumes
you can build + deploy per [BUILD.md](BUILD.md)).

File paths are relative to the WebKit tree (`WebKit-2.36.8/`) after applying the patch + `src/` files.

---

## 1. Fetch `NetworkError` on streaming responses  ·  impact: high  ·  difficulty: medium
**Symptom:** large / long-lived streaming `response.body` fetches intermittently fail with a JS
`NetworkError: A network error occurred`, even when the underlying transfer reports success. This is
the main thing making video (and other streaming/heavy sites) unreliable — YouTube's media reader
gives up when its stream ends early/wrong.

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
**State:** MSE plumbing works and has decoded+painted under lab conditions, but the live path is a
CPU readback (choppy). A zero-copy path (decode → ANGLE-shared D3D texture → GL composite) exists in
the tree but is **dormant** because enabling it regressed stability (the app hard-closed before the
first frame).

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
