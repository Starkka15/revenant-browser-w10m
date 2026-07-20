# Building Revenant

This guide reproduces the ARM32-UWP (Windows 10 Mobile) build from a clean machine. It is a
**cross-build**: you build on a Windows 10/11 desktop and deploy the resulting `.appx` to the phone.

> **Path convention.** Every script in this repo hardcodes the project root as `Z:\w10m-webengine\`.
> The simplest reproduction is to lay the tree out at that path and `subst Z: C:\path\to\root` (or
> map a drive). Otherwise, find/replace `Z:\w10m-webengine` in the `.bat`/`.ps1` scripts and the
> `configure-wincairo.bat` `-D…` paths. Paths below use `Z:\w10m-webengine` = the project root.

---

## 1. Prerequisites (build machine)

| Tool | Version / notes |
|------|-----------------|
| Windows SDK | **10.0.16299.0** (exactly — see the min-version note below) |
| Visual Studio 2022 | with **v142** toolset (ARM32 UWP + C++20). **v141** is also needed to build ICU. Install the "Universal Windows Platform development" + "C++ ARM build tools" workloads. |
| CMake | 3.21+ (we used a portable CMake 4.3.3; any recent works). Generator: `Visual Studio 17 2022`. |
| Python | 3.x, with `pip install pkgconf` (provides `pkgconf.exe` used by the meson/cmake dep builds) |
| Ruby | any recent (JavaScriptCore's `offlineasm` LLInt generator) |
| gperf, bison, flex | on `PATH` (WebCore code generators). We staged win_bison/win_flex + gperf 3.0.1 under `deps\tools\bin`. |
| Meson + Ninja | for the cairo/harfbuzz/pixman dependency builds (meson cross-file provided) |

> **⚠ min-version gotcha (all W10M UWP apps):** build *against* SDK 16299
> (`TargetPlatformVersion=10.0.16299.0`) but set **`TargetPlatformMinVersion=10.0.15063.0`**. Phones
> report OS build 15254; a package whose min is 16299/15254 will not install on the device. The
> toolchain file and `configure-wincairo.bat` already do this.

---

## 2. Get the WebKit source and apply our changes

We build **full, non-pruned WebKit 2.36.8** (the webkitgtk.org release tarballs strip the Windows
platform files WebCore needs — clone from GitHub instead).

```bat
git clone https://github.com/WebKit/WebKit.git Z:\w10m-webengine\WebKit-2.36.8
cd Z:\w10m-webengine\WebKit-2.36.8
git checkout webkitgtk-2.36.8

rem 1. Apply the port patch (~150 modified files: WTF, JSC, WebCore, ANGLE, curl, cmake)
git apply Z:\w10m-webengine\patches\webkit-2.36.8-revenant.patch

rem 2. Drop in our new source files (port driver layer, C++/CX shell, MSE glue).
xcopy /E /I /Y Z:\w10m-webengine\src\Source Z:\w10m-webengine\WebKit-2.36.8\Source
```

The new files land at:
- `Source/WebCore/port/` — the platform driver (render loop, loader/chrome/editor/frame clients,
  DOM storage, Cache API, ServiceWorker, display-refresh monitor, platform strategies).
- `Source/WebCore/shell/` — the C++/CX UWP app (`RenderShell*.cpp`), Revenant tile assets, the
  MSE `MediaPlayer` frame-server bridge (`ShellMse.cpp`), and the CA bundle (`cacert.pem`).
- `Source/WebCore/platform/graphics/win/*MediaFoundation*` + `ShellMseBridge.h` — MSE ↔ WebCore glue.

The build wiring for these (CMakeLists / PlatformWin.cmake / PlatformWinCairo.cmake) is included in
the patch.

---

## 3. Build the dependencies (ARM32 UWP static libs)

All dependencies are built as **static** libs (`/MD` CRT — AppContainer forbids `/MT`) and installed
to `Z:\w10m-webengine\deps\_install\{lib,include}`. Fetch each at the version below, apply our patch
where present (`patches/deps/`), then run the matching build script in `deps/`.

| Dependency | Version | Patch? | Build script |
|------------|---------|--------|--------------|
| zlib | 1.3.1 | `patches/deps/zlib.patch` | `deps/zlib-build.bat` |
| libpng | 1.6.43 | — | `deps/deps-batch1.bat` |
| libjpeg-turbo | 3.0.3 | — | `deps/deps-batch1.bat` (`WITH_CRT_DLL=ON`) |
| sqlite | amalgamation (WinRT VFS) | — | `deps/deps-batch1.bat` (`deps/sqlite-amalg`) |
| libxml2 | 2.12.6 | — | `deps/libxml2-build.bat` |
| libpsl | 0.21.5 | — | `deps/libpsl-build.bat` |
| libwebp | 1.4.0 | — | `deps/webp-brotli.bat` |
| brotli | 1.1.0 | — | `deps/webp-brotli.bat` |
| woff2 | 1.0.2 | — | `deps/woff2-build.bat` |
| pixman | 0.42.2 | — | (meson cross build) |
| harfbuzz | 4.4.1 | `patches/deps/harfbuzz.patch` | `deps/harfbuzz-build.bat` |
| cairo | 1.18.2 | `patches/deps/cairo.patch` (DirectWrite font backend rewrite for UWP) | `deps/cairo-cfg.bat` + `deps/cairo-build.bat` |
| BoringSSL | (chromium-stable) | `deps/boringssl-uwp/` — **required**, see below | `deps/configure-boringssl-new.bat` + `deps/build-boringssl-new.bat` |
| nghttp2 | 1.61.0 (HTTP/2 for curl) | — | `deps/nghttp2-build.bat` |
| libcurl | 8.11.1 | — | `deps/curl-cfg-boringssl-h2.bat` + `deps/curl-build-boringssl-h2.bat` |
| ~~OpenSSL~~ | 3.0.14 | — | `deps/openssl-*.bat` — **superseded by BoringSSL, kept for reference only** |

### TLS: BoringSSL, not OpenSSL

The TLS backend is **BoringSSL**. This is not a preference — Cloudflare Turnstile fingerprints the TLS
ClientHello (JA3), and OpenSSL cannot emit the GREASE values a real Safari sends. That absence was the
tell that kept the browser stuck in an endless re-challenge loop. BoringSSL can produce the iOS Safari
ClientHello, which is what makes Cloudflare-protected sites usable at all.

BoringSSL upstream has no ARM32-UWP build, so `deps/boringssl-uwp/` carries ours:
- `CMakeLists.txt` — replaces upstream's; drops the tool/test targets, forces `/MD`, targets
  `WindowsStore 10.0.16299.0`, and excludes the assembly that will not assemble for ARM32 UWP.
- `uwp_compat.h` — shims the handful of Win32 APIs BoringSSL calls that AppContainer does not expose.

Copy both over a fresh BoringSSL checkout before configuring.

### curl must be built with HTTP/2 explicitly

Use the `-h2` scripts. `deps/curl-cfg.bat` and `deps/curl-cfg-boringssl.bat` both pass
`-DUSE_NGHTTP2=OFF` and are kept only for history — building with either silently produces an
HTTP/1.1-only libcurl, which costs one TCP+TLS connection per parallel resource on image-heavy pages.
Verify after building, rather than trusting the flag:

```
findstr USE_NGHTTP2 deps\curl\build-armuwp-bssl3\lib\curl_config.h
rem must print:  #define USE_NGHTTP2 1
```

The configure script prints its `Features:` line too — check `HTTP2` appears in it.

Notes:
- Meson dep builds (cairo/harfbuzz/pixman) use `deps/cross-armuwp.txt`. They need the **native**
  `pkgconf.exe` from the pip `pkgconf` package (the `…\site-packages\pkgconf\.bin\pkgconf.exe`, not
  the Scripts wrapper), `.pc` files under `_install\lib\pkgconfig`, and
  `-Dc_args=/Zc:preprocessor-` (the 16299 SDK's legacy preprocessor).
- The cairo patch replaces `cairo-dwrite-font.cpp` to build glyph surfaces via
  `IDWriteGlyphRunAnalysis` (no GDI), disables the win32 surface/font, and keeps `DWRITE_FONT`.
- The `deps/*probe*.bat` / `find*.bat` scripts are one-off environment probes — ignore them.

---

## 4. Build ICU 77.1 (static, ARM32 UWP)

```bat
rem Get ICU4C 77.1 source into Z:\w10m-webengine\icu, then:
Z:\w10m-webengine\deps\icu-build-static.bat
```

This builds `icuuc` (common) + `icuin` (i18n) as **static** libs with data linked in
(`/DU_STATIC_IMPLEMENTATION`, `ConfigurationType=StaticLibrary`, toolset **v141**, min 15063) to
`Z:\w10m-webengine\icu\libARMuwp-static`. Edit the `MSB`/`LIBEXE` paths at the top of the script to
your VS install. (ICU built with v141 links fine into the v142 WebKit build — MSVC ABI compatible.)

---

## 5. Configure WebKit

```bat
Z:\w10m-webengine\configure-wincairo.bat
```

This runs CMake with the `WinCairo` port for ARM32 WindowsStore (SDK 16299, v142), points every
dependency at `deps\_install` + `icu\libARMuwp-static`, enables the feature set (JIT + DFG, MSE,
WebGL, WebAssembly, ServiceWorker, WebCrypto, Media Foundation, touch, etc.), and writes the build
tree to `Z:\w10m-webengine\build-wincairo`. It finishes by running `patch-manifest.ps1` to add the
AppContainer network capabilities + Revenant branding that CMake strips on regenerate.

Key gotchas already handled in the toolchain/configure:
- `-DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0` on the command line.
- `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY` (else the compiler probe links an exe → `WindowsApp.lib` LNK1104).
- `CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION_MAXIMUM=10.0.16299.0` (else CMake selects a newer SDK and the ARM config trips the TPV≤22621 gate).
- CMake dependency paths use **forward slashes** (backslash = escape).

---

## 6. Build and package the app

```bat
Z:\w10m-webengine\build-shell.bat
```

`build-shell.bat` builds the `WebCoreRenderShell` target twice: the first pass may auto-reconfigure
(ZERO_CHECK) and regenerate the manifest without our network caps, so it re-runs `patch-manifest.ps1`
and builds again (repackage-only, fast), then copies the signed package to
`Z:\w10m-webengine\WebCoreRenderShell_ARM.appx`. Success prints `staged` + `SHELL_EXIT=0`.

---

## 7. Deploy to the phone

- Put the phone in **Developer mode** (Settings → Update & security → For developers).
- Deploy over USB with `WinAppDeployCmd`:
  ```bat
  WinAppDeployCmd install -file Z:\w10m-webengine\WebCoreRenderShell_ARM.appx -ip <phone-ip>
  ```
  (or use the phone's **Device Portal** upload). The package identity is stable, so redeploying
  updates in place; no need to uninstall first. It installs/appears as **Revenant**.

---

## Troubleshooting

- **`WindowsApp.lib` LNK1104 during compiler probe** — the toolchain sets
  `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`; make sure you're using the provided toolchain file.
- **App installs but won't launch on the phone** — check `TargetPlatformMinVersion` is `10.0.15063.0`
  in the generated manifest, not 16299.
- **Native/JNI/serialization features work in debug but not release** — release builds differ; always
  test the actual packaged `.appx`, not a loose/debug drop.
- **Meson can't find a dependency** — confirm the native `pkgconf.exe` (pip package, `.bin\pkgconf.exe`)
  is first on `PATH` and the `.pc` files exist under `_install\lib\pkgconfig`.
- **Long builds** — WebCore is large; a clean build is multi-hour. Incremental rebuilds of just the
  shell target (after touching a few files) are minutes.

See [`docs/`](docs) for the design specs (feasibility spike, build order SPEC, the non-headless
render plan, and the feature-parity spec).
