# Revenant — a modern WebKit browser engine for Windows 10 Mobile (ARM32 UWP)

> ## ⚠️ VERY EARLY ALPHA
> This is a hobbyist proof-of-concept, not a usable daily browser. Expect rough edges and crashes.
> **Specifically:**
> - **Video does not appear in pages.** It decodes at full frame rate and never reaches the screen —
>   see [the open bug](#the-headline-open-bug-video-decodes-but-never-draws) below. A standalone
>   `.mp4` URL *does* play.
> - **YouTube stops after about a minute.**
> - **Pages run slowly** — these are ~2013-era ARM32 phones; even with the JIT and DFG JIT doing
>   their best, heavy modern JavaScript is a lot to ask of the hardware. Measured: JS and layout are
>   ~93% of a worst-case frame; rasterisation is ~7%.
> - **Heavy pages can still be OOM-killed** — ~250MB of native heap is currently unattributed.
>
> Cloudflare **Turnstile now passes** (re-challenge count went 41 → 0) after migrating TLS to
> BoringSSL and fixing the JS-visible fingerprint.
>
> What it *does* do is genuinely render the modern web on a phone Microsoft abandoned — basic
> browsing, logins/forms, and standards support far beyond EdgeHTML. Treat it as a "look, it's
> possible" milestone, not a finished product.

> **Project status:** paused as of **2026-07-20**. The tree builds, deploys and runs. Current state,
> including exactly where the video bug sits and what to try next, is in
> [KNOWN_ISSUES.md](KNOWN_ISSUES.md) — start there.

Revenant is a port of **WebKit 2.36.8** (the WinCairo port) to **Windows 10 Mobile**, running
**on-device** on ARM32 UWP hardware (developed/tested on a **Nokia Lumia 640 XL**, Adreno 305,
Direct3D 11 feature level 9_3). It renders real pages on a phone that Microsoft abandoned years ago,
with a modern JavaScript engine, HTTP/2 networking, and hardware-accelerated compositing —
none of which EdgeHTML (the last stock W10M browser engine) can do.

This repository is the **recipe** to reproduce the build: our changes to WebKit and its dependencies
as patches, the new source files we authored, the ARM32-UWP toolchain + build scripts, and a full
build guide. It does **not** vendor the multi-gigabyte WebKit tree or the prebuilt dependency
binaries — you fetch those from upstream and apply our changes (see [BUILD.md](BUILD.md)).

## What works (on-device, to varying degrees)

- **Basic browsing** — real layout + paint via WebCore, composited through TextureMapper on GL
  (ANGLE → D3D11). Not headless; actual pixels on the phone screen. Slow on this hardware, but real.
- **JavaScript with JIT** — JavaScriptCore including a **custom MSVC-ARM32 LLInt assembler backend**
  (WebKit never shipped one) and the DFG JIT, with W^X via `VirtualAllocFromApp`/`VirtualProtectFromApp`.
  Fast *for* an ARM32 phone; still limited by the hardware on JS-heavy sites.
- **Networking** — libcurl 8.11.1 over **BoringSSL**, with HTTP/2 (nghttp2), DNS-over-HTTPS, and an
  iOS-Safari TLS ClientHello. Verified on device: 149 of 151 responses in a session negotiated h2.
- **TLS fingerprint parity** — BoringSSL emits the GREASE values and cipher/extension ordering a real
  Safari sends. OpenSSL cannot, and that absence was the tell that kept Cloudflare Turnstile in an
  endless re-challenge loop.
- **Caching** — HTTP disk cache (64MB, persists across launches) and a JSC bytecode disk cache
  (measured 22% hit rate on relaunch).
- **Storage** — localStorage / sessionStorage / document.cookie, Cache API (`window.caches`),
  in-process ServiceWorkers.
- **Input** — soft keyboard (SIP) on focus, touch, text editing. Logins/forms generally work.
- **Graphics** — WebGL, OffscreenCanvas; Cairo 2D with a DirectWrite font backend.

## Does NOT work / known limitations

Everything here is measured on a Lumia 640 XL, with the log line that shows it. Full detail and
next steps: [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

### The headline open bug: video decodes but never draws

    video: decoded=38.3fps drawn=0.0fps (0/77)
    gate: last97 composited=97 skipped=0

Media Foundation decodes at full rate, the page composites *every* frame, and **zero** video frames
reach the screen. Opening the same `.mp4` directly plays fine — the failure only occurs for video
embedded in a page. It is isolated to one block in
`MediaPlayerPrivateMediaFoundation::paintCurrentFrameToTextureMapper`: the texture-upload condition
never succeeds, so the function early-returns before drawing. Proof: the `fsvid:` diagnostic that
sits immediately past that return appears **zero** times in a 12,000-line log. The next step is to
log its four sub-conditions in a single build and see which one fails — not to guess.

### Other known-bad

- **YouTube stops after ~1 minute.** Long-standing, cause unknown, MSE path.
- **HLS cannot resolve DNS.** `AdaptiveMediaSource` fetches via `Windows.Web.Http` → the system
  resolver, bypassing the DNS-over-HTTPS workaround this device needs. Fails `0x80072ee7`.
- **~250MB of native heap is unattributed.** The tracked pools (JS heap, resource cache, GPU tiles)
  total ~100MB against ~350MB in use; the rest is CRT heap invisible under `USE_SYSTEM_MALLOC`. This
  is what OOM-kills heavy pages, and no cache release touches it.
- **Compositing-layer explosion.** 87% of layer promotions are `[overlap,]` cascade — one page
  reached 50 layers / 39MB of GPU tiles.
- **Performance** — ~2013 ARM32 CPUs. A worst-case frame measured `js=2337ms update=883ms
  composite=243ms`: **JS and layout dominate; rasterisation is ~7%.** Optimise accordingly.
- **Missing platform features** — `window.open`/popups (breaks some OAuth logins), file upload,
  some JS dialogs / platform pickers, WebAssembly.
- **Stability** — crashes happen; this is alpha.

## Repository layout

| Path | Contents |
|------|----------|
| `patches/webkit-2.36.8-revenant.patch` | All modifications to tracked WebKit source (242 files) |
| `patches/deps/*.patch` | Source patches to dependencies (cairo, harfbuzz, zlib) |
| `deps/boringssl-uwp/` | **New** BoringSSL ARM32-UWP `CMakeLists.txt` + `uwp_compat.h`. Upstream has no ARM32-UWP build; copy these over a fresh checkout before configuring |
| `src/Source/WebCore/port/` | **New** platform driver layer (loader/chrome/editor/frame clients, storage, cache, service worker, the render driver) |
| `src/Source/WebCore/shell/` | **New** C++/CX UWP app shell + Revenant tile assets + MSE `MediaPlayer` bridge |
| `src/Source/WebCore/platform/graphics/win/*MediaFoundation*`, `ShellMseBridge.h` | **New** MSE glue |
| `src/Source/WebCore/platform/{audio/win,mediastream}/` | **New** WASAPI audio + libwebrtc mediastream glue |
| `cmake/Toolchain-W10M-ARM32-UWP.cmake` | CMake toolchain for ARM32 WindowsStore / SDK 16299 / v142 |
| `scripts/` | WebKit configure + build + appx-manifest scripts |
| `deps/` | Per-dependency ARM32-UWP build scripts + ICU build + meson cross file |
| `docs/` | Design specs (feasibility, build SPEC, non-headless render, feature parity, tabs) |
| [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md) | **Current state + open bugs, with the device evidence for each. Start here.** |

Not vendored, fetch from upstream: the WebKit tree, BoringSSL, curl, nghttp2, ICU, and
`Source/ThirdParty/wamr` (34MB). [BUILD.md](BUILD.md) lists every version and build script.

## Working on this — read this before you start

The device writes a very detailed `debug.log` (`LocalState\debug.log`, pull it over Windows Device
Portal with the app closed — it holds the file open). It is the single most valuable thing in the
project, and the lesson from every wrong turn so far is the same:

> **Read the whole log, not the lines you expect.**

Concrete examples from one session:

- The `drawn=0.0fps` counter — which pins the video bug exactly — was present in *every* log for
  hours while the investigation went after routing, codec gating and pipeline startup instead.
- `cmplayer:` logs every compositing-layer promotion with its reason. Nobody had looked at it; it
  turned out 87% were `[overlap,]` cascade, which explains the GPU tile growth.
- HTTP/2 had been silently off since a dependency migration. The curl configure output prints a
  `Features:` line that would have shown it immediately.

Useful lines to grep once you know they exist: `SLOWFRAME` (per-frame cost breakdown, `GAP=` should
be 0), `gate:` (rolling CPU split + video decoded/drawn fps), `mempool:` (where memory actually is),
`bcache:` (bytecode cache hit rate), `gc:` (stop-the-world pause durations), `cmplayer:` (layer
promotions), `tmsched:` (tile raster scheduler), `mse:`/`hls:`/`prog:` (media pipeline lifecycle).

Two engineering notes that cost real time to learn:

- **Trace a symbol's callers before writing the call.** Several fixes here were "unreachable by
  construction" — most notably a media path that waited on a `readyState` only reachable via the
  code path it had skipped.
- **Verify one change at a time on device.** A clean compile proves nothing; several changes that
  built fine were inert (installed before the object they needed existed) and only the log showed it.

## Toolchain summary

- **Windows SDK 10.0.16299.0** (build against 16299, `TargetPlatformMinVersion` **10.0.15063.0** — phones report OS build 15254 and won't install packages with a higher min).
- **Visual Studio 2022** with the **v142** toolset (ARM32 UWP + C++20); **v141** is used to build ICU.
- CMake (Visual Studio 17 2022 generator, `-A ARM -T v142,host=x64`, `CMAKE_SYSTEM_NAME=WindowsStore`).
- Python 3 (+`pkgconf`), Ruby (JSC offlineasm), gperf / bison / flex.

## Related projects — and how Revenant differs

The other effort in this space is **[Project-Apotheosis](https://github.com/Jimmyxiao2009/Project-Apotheosis)** —
same goal (a modern WebKit/WebCore engine on Windows 10 Mobile ARM32). Revenant is **independent work**
(we studied their public approach, but didn't fork it — we're on WebKit 2.36.8, they're on 2.52.4), and
this is meant as an objective "different choices" comparison, not a ranking. Both use ANGLE (D3D11) +
WebCore's TextureMapper and present into a `SwapChainPanel` — that part is common ground. The
differences are real and cut both ways:

| | Revenant | Project-Apotheosis |
|---|---|---|
| **WebKit version** | 2.36.8 (last C++17-era release) | 2.52.4 (newer engine) |
| **Toolchain** | **MSVC v142**, no clang | clang-cl + MSVC v143 |
| **JSC LLInt** | **custom hand-written MSVC-ARM32 (armasm) LLInt backend** (WebKit's offline-asm targets GCC/clang; a pure-MSVC build needed one from scratch) | existing offline-asm path (clang) |
| **Render resolution** | renders at the panel's **native pixel density** — `setDeviceScaleFactor(DPR)`, presented **1:1** (sharper) | renders a **fixed 720×1080** engine surface and **stretches** it to fill the panel (softer on non-720×1080 screens) |
| **Viewport / screen fit** | CSS viewport, `screen.*`, `@media` width all derived from the **real panel size ÷ DPR** (`view->resize`, `setPlatformScreenBounds`) — the page is laid out to the actual device | fixed 720×1080 logical space regardless of device |
| **WebCrypto (`crypto.subtle`)** | **implemented** — WebKit's OpenSSL backend, patched for OpenSSL 3.x opaque structs (RSA/EC/ECDSA/ECDH/AES/HMAC/HKDF/PBKDF2) | **stubbed** — SubtleCrypto unavailable (real SHA digests kept for Subresource Integrity) |
| **Browser shell** | minimal (address bar) | fuller: tabs, address bar, find-in-page |
| **Media** | experimental MSE/adaptive video via a WinRT `MediaPlayer` frame-server | — |
| **Test device** | Lumia 640 XL (Adreno 305) | Lumia 950 |

**Where Revenant is stronger:** it renders at the device's native resolution and ties the page's
viewport/`screen`/`@media` metrics to the real panel dimensions, so text and layout are crisp and
sized correctly per device (Apotheosis's fixed-720×1080-then-stretch approach is simpler but non-native
scaling looks softer and the page isn't sized to the actual screen). It also ships a **working
`crypto.subtle`** — which real sites (logins, banking, modern web apps) increasingly need — where
Apotheosis stubs WebCrypto out. And it has an experimental MSE/adaptive-video path.

**Where Apotheosis is stronger:** a **newer engine** (2.52.4 vs 2.36.8), a **more complete browser
shell** (tabs / find-in-page), a **clang-cl** toolchain that's more conventional and easier to keep
current, and a clean **readback-free direct-present** fast path.

### If you actually want to ship a browser: combine the two

Neither project is the whole answer — but between them, most of the hard parts are solved. A
genuinely usable W10M browser would most likely take **Apotheosis's newer engine + fuller shell +
clang-cl toolchain** as the base, and fold in **Revenant's native-resolution/DPR rendering,
real-screen viewport, and working WebCrypto** (and the MSE video groundwork). We each do a few things
well; the fastest path to a real browser is putting those pieces together rather than either of us
starting over. PRs and cross-pollination between the projects are genuinely welcome.

## Contributing

A modern browser is a community-scale effort — this is opened in that spirit. Forks, patches, and PRs
welcome. See **[CONTRIBUTING.md](CONTRIBUTING.md)** for the (patch-based) workflow and dev loop, and
**[KNOWN_ISSUES.md](KNOWN_ISSUES.md)** for open problems with file pointers so you can pick something
scoped rather than cold-starting on a WebKit port.

**Active development is paused as of 2026-07-20.** The tree builds, deploys and runs, and the open
problems are written up with the device evidence behind each one. If you want to pick something up,
the highest-value item by a distance is the video draw bug at the top of
[KNOWN_ISSUES.md](KNOWN_ISSUES.md): it is narrowed to a single `if` condition, the diagnostic that
would identify which term fails is one `snprintf` away, and fixing it makes video work in pages —
which is the difference between a demo and something usable.

## Credits / license

Revenant is derived from [WebKit](https://webkit.org/) (BSD/LGPL) and links the dependencies listed
in [BUILD.md](BUILD.md), each under its own license. Our patches and new files follow the license of
the file they modify (WebKit's BSD/LGPL) unless otherwise noted. This is an independent hobbyist port,
not affiliated with Apple, the WebKit project, or Microsoft.

App icon by **thiagoaraujoxd** (created with Gemini). Thanks!
