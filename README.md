# Revenant — a modern WebKit browser engine for Windows 10 Mobile (ARM32 UWP)

> ## ⚠️ VERY EARLY ALPHA
> This is a hobbyist proof-of-concept, not a usable daily browser. Expect rough edges and crashes.
> **Specifically:**
> - **YouTube does not work.**
> - **Video playback does not work** (the media pipeline is experimental and unreliable).
> - **Cloudflare detection is finnicky at best** — some challenge-gated sites won't pass.
> - **Pages run slowly** — these are ~2013-era ARM32 phones; even with the JIT and DFG JIT doing
>   their best, heavy modern JavaScript is a lot to ask of the hardware.
>
> What it *does* do is genuinely render the modern web on a phone Microsoft abandoned — basic
> browsing, logins/forms, and standards support far beyond EdgeHTML. Treat it as a "look, it's
> possible" milestone, not a finished product.

Revenant is a port of **WebKit 2.36.8** (the WinCairo port) to **Windows 10 Mobile**, running
**on-device** on ARM32 UWP hardware (developed/tested on a **Nokia Lumia 640 XL**, Adreno 305,
Direct3D 11 feature level 9_3). It renders real pages on a phone that Microsoft abandoned years ago,
with a modern JavaScript engine, HTTP/2 + HTTP/3 networking, and hardware-accelerated compositing —
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
- **Networking** — libcurl 8.11.1 with HTTP/2 (nghttp2) and HTTP/3, OpenSSL, persistent cookies.
- **Storage** — localStorage / sessionStorage / document.cookie, Cache API (`window.caches`),
  in-process ServiceWorkers.
- **Input** — soft keyboard (SIP) on focus, touch, text editing. Logins/forms generally work.
- **Graphics** — WebGL, OffscreenCanvas; Cairo 2D with a DirectWrite font backend.

## Does NOT work / known limitations

- **Video playback does not work** — the media stack (Media Foundation for progressive, a WinRT
  `MediaPlayer` frame-server for MSE) is experimental and unreliable. **YouTube does not work.** The
  plumbing is in-tree and MSE has been observed to decode+paint once under lab conditions, but it is
  not dependable and should be considered non-functional in this alpha. See [`docs/`](docs).
- **Cloudflare / anti-bot** — finnicky at best; challenge-gated sites may not pass (TLS/JS
  fingerprinting).
- **Performance** — ~2013 ARM32 CPUs; heavy modern sites are slow even with JIT + DFG JIT.
- **Missing platform features** — `window.open`/popups (breaks some OAuth logins), file upload,
  some JS dialogs / platform pickers.
- **Stability** — crashes happen; this is alpha.

## Repository layout

| Path | Contents |
|------|----------|
| `patches/webkit-2.36.8-revenant.patch` | All modifications to tracked WebKit source (~150 files) |
| `patches/deps/*.patch` | Source patches to dependencies (cairo, harfbuzz, zlib) |
| `src/Source/WebCore/port/` | **New** platform driver layer (loader/chrome/editor/frame clients, storage, cache, service worker, the render driver) |
| `src/Source/WebCore/shell/` | **New** C++/CX UWP app shell + Revenant tile assets + MSE `MediaPlayer` bridge |
| `src/Source/WebCore/platform/graphics/win/*MediaFoundation*`, `ShellMseBridge.h` | **New** MSE glue |
| `cmake/Toolchain-W10M-ARM32-UWP.cmake` | CMake toolchain for ARM32 WindowsStore / SDK 16299 / v142 |
| `scripts/` | WebKit configure + build + appx-manifest scripts |
| `deps/` | Per-dependency ARM32-UWP build scripts + ICU build + meson cross file |
| `docs/` | Design specs (feasibility, build SPEC, non-headless render, feature parity) |

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

## Credits / license

Revenant is derived from [WebKit](https://webkit.org/) (BSD/LGPL) and links the dependencies listed
in [BUILD.md](BUILD.md), each under its own license. Our patches and new files follow the license of
the file they modify (WebKit's BSD/LGPL) unless otherwise noted. This is an independent hobbyist port,
not affiliated with Apple, the WebKit project, or Microsoft.

## AI Disclosure

This project is developed with AI assistance. See **[AI.md](AI.md)** for how AI is used here, the ethical obligations behind it, and how upstream credit and licensing are handled.
