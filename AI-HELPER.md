# AI-HELPER — building Revenant with an AI agent

This file is written for an AI coding agent (or a human who wants a terse, imperative checklist).
It tells you exactly what to download, where to put it, and how to build the Revenant appx from
this recipe. Human-oriented context is in [BUILD.md](BUILD.md); dependency versions are pinned in
[deps/MANIFEST.md](deps/MANIFEST.md).

Revenant is **not** a full source tree. It is a set of patches + new files that overlay a specific
WebKit release, plus patched dependencies. Your job is to reconstruct the build tree, apply the
overlay, build the dependencies, then build the shell appx.

## 0. What this repo contains

| Path | Meaning |
|---|---|
| `patches/webkit-2.36.8-revenant.patch` | apply to a pristine WebKit @ tag `webkitgtk-2.36.8` (modified upstream files only) |
| `src/` | NEW port-authored files — copy over the WebKit tree, preserving paths (`src/Source/...` → `<webkit>/Source/...`) |
| `patches/deps/*.patch` | per-dependency patches (curl, pixman, cairo, harfbuzz, zlib, wamr, boringssl) |
| `deps/MANIFEST.md` | exact upstream repo + pinned commit for every dependency |
| `deps/*.bat`, `scripts/*.bat`, `scripts/*.ps1` | build scripts (genericized — paths come from `env.bat`) |
| `cmake/` | the ARM32-UWP CMake toolchain file |
| `env.example.bat` | copy to `env.bat`, fill in machine paths (see step 2) |

## 1. Host prerequisites (install / verify)

- **Windows 10/11 x64** host (the cross toolchain is Windows-only).
- **Visual Studio 2022** (Community is fine) with workloads: *Desktop development with C++*,
  *Universal Windows Platform development*, and the **ARM (not ARM64) build tools** + the
  **v142** and **v141** toolsets. Confirm `vcvarsall.bat x64_arm` succeeds.
- **Windows SDK 10.0.16299.0** — this exact version. Build *against* 16299 but the app targets
  `TargetPlatformMinVersion=10.0.15063.0` (phones report OS build 15254 and reject a higher min).
- **CMake ≥ 3.12**, **Ninja**, **Meson** (cairo/pixman/harfbuzz use Meson), **Python 3** with
  **pkgconf** (`pip install pkgconf`), and **gperf**, **bison**, **flex** on `PATH`.
- **git**, and a couple of GB of disk.

## 2. Lay out the build tree (`ROOT`)

Create an empty folder — this is `ROOT` (e.g. `C:\revenant`). Target layout:

```
ROOT\
  WebKit-2.36.8\        WebKit source + overlay (step 3)
  deps\                 dependency sources + build scripts (step 4)
  boringssl-new\        BoringSSL source + patch (step 4)
  cmake\                copy this repo's cmake\ here (toolchain file)
  icu\                  ICU source or prebuilt static libs (deps\icu-build-static.bat)
  tools\                cmake, ninja, gperf, bison, flex, meson (or have them on PATH)
  build-wincairo\       created by the build; leave empty
```

Then, in this repo: copy `env.example.bat` → `env.bat` and set `ROOT`, `CMAKE`, `VCVARSALL`,
`PKGCONF`, `VSGEN`. Every script sources `env.bat` automatically.

## 3. WebKit source + overlay

1. `git clone https://github.com/WebKit/WebKit` then check out the tag in `deps/MANIFEST.md`
   (`webkitgtk-2.36.8`). Move/rename it to `ROOT\WebKit-2.36.8`.
2. From the WebKit root: `git apply <repo>/patches/webkit-2.36.8-revenant.patch`
   (or `patch -p1 <`). This modifies existing upstream files only.
3. Copy the overlay of new files: everything under `src/` maps to the WebKit tree at the same path,
   e.g. `src/Source/WebCore/shell/ShellMse.cpp` → `ROOT\WebKit-2.36.8\Source/WebCore/shell/ShellMse.cpp`.
4. **WAMR** is a git submodule at `Source/ThirdParty/wamr` (NOT in `src/`). Clone it at the pinned
   commit from `deps/MANIFEST.md`, then apply `patches/deps/wamr.patch`.

## 4. Dependencies

For **each** dependency in `deps/MANIFEST.md`:
1. `git clone <upstream>` and check out the **pinned commit**.
2. If a `patches/deps/<name>.patch` exists, apply it.
3. Build with the matching `deps\<name>-build.bat` (and `-cfg`/`-boringssl-h2` variants where
   present). Static libs and headers install into `ROOT\deps\_install\{lib,include}`.

Notes:
- **BoringSSL** is the TLS backend (`ROOT\boringssl-new`): clone at the pin, apply
  `patches/deps/boringssl.patch`, build with `scripts/configure-boringssl-new.bat` +
  `scripts/build-boringssl-new.bat`. curl links against it (`curl-cfg-boringssl-h2.bat`).
- **curl** must be built with `USE_NGHTTP2=ON` (HTTP/2) — use `deps/curl-cfg-boringssl-h2.bat`.
  The h2 fingerprint patch is what lets Cloudflare Turnstile pass.
- **pixman**: the patch adds MSVC ARM32-UWP NEON intrinsics; the GNU-asm SIMD paths stay disabled
  (they are ARM-mode A32 asm and can't build under Thumb-2). See `deps/pixman-build.bat`.
- **ICU** is prebuilt static via `deps/icu-build-static.bat` (uses the v141 toolset).
- Dependencies with no patch are vendored unchanged — just build them at the pinned commit.

## 5. Configure + build the shell

```
scripts\configure-wincairo.bat     REM CMake configure (WinCairo port, ARM, WindowsStore)
scripts\run-build.bat              REM builds WebCore + WebCoreRenderShell, patches the manifest, repackages
```

`run-build.bat` calls `build-shell.bat`, which builds the appx, runs `patch-manifest.ps1`
(rebrands to "Revenant", sets the version, injects `internetClient` + `codeGeneration` JIT
capability), and rebuilds so the patched manifest survives.

**Success signal:** the log ends with `SHELL_EXIT=0` and an appx appears at
`ROOT\build-wincairo\Source\WebCore\AppPackages\WebCoreRenderShell\WebCoreRenderShell_*_ARM_Test\WebCoreRenderShell_*_ARM.appx`.

## 6. Version

The release version lives in `scripts/patch-manifest.ps1` (`$REVENANT_VERSION`). It rewrites the
appx **Identity Version** (numeric `x.x.x.x`). Bump it per release; the human-facing tag adds the
`-alpha` suffix (e.g. Identity `0.0.1.1` ↔ release `v0.0.1.1-alpha`).

## 7. Deploy to a phone (optional, for testing)

Install the appx via Windows Device Portal (`/api/app/packagemanager/package`) or the
`Add-AppxPackage` / Device Portal UI: uninstall the old package, install the new appx + its
dependencies, launch. Because the Identity Version was bumped, installing over an older build is an
**upgrade** (no manual uninstall needed).

## 8. If something fails

- Read the build log fully; `curl_config.h` / `CMakeCache.txt` are ground truth for whether a flag
  actually took (a stale build dir silently keeps old settings — use a fresh `build-*` dir).
- Confirm the SDK is exactly `10.0.16299.0` and the min-version is `10.0.15063.0`.
- Confirm each dependency is at the **pinned** commit — a newer upstream may not apply the patch or
  may change APIs.
- `env.bat` still has example paths if you see `[env] WARNING` — fill it in.
