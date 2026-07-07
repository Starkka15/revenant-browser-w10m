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
