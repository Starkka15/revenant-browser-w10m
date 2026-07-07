# Contributing to Revenant

A real browser is bigger than one person — that's the whole reason this is open. Forks, patches, and
PRs are all welcome. This doc explains how the repo is structured (it's unusual), how to set up a dev
loop, and where the useful work is.

See [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for a curated list of open problems with file pointers, so you
can pick something scoped instead of cold-starting on a WebKit port.

## How this repo is structured (important)

This is a **build recipe**, not a full source tree. It does not vendor the ~5 GB WebKit tree or the
prebuilt dependency binaries. Instead it contains:

- `patches/webkit-2.36.8-revenant.patch` — our changes to tracked WebKit source
- `patches/deps/*.patch` — our changes to dependency source
- `src/Source/WebCore/{port,shell,platform/graphics/win}/…` — the **new** files we authored
- scripts, cmake toolchain, docs

So your working tree is a **real WebKit 2.36.8 checkout with our patch applied + our new files
copied in** (see [BUILD.md](BUILD.md) §2). You develop there.

## Contribution workflow

Because the repo is patch-based, a PR usually updates a patch and/or adds files under `src/`:

1. Fork this repo and set up the build (see [BUILD.md](BUILD.md)).
2. Make your change in your WebKit working tree.
3. Regenerate the relevant artifact:
   - **Changed existing WebKit files** → refresh the patch:
     ```bash
     cd WebKit-2.36.8 && git diff > /path/to/revenant-browser-w10m/patches/webkit-2.36.8-revenant.patch
     ```
     (or, for a focused change, attach a smaller incremental patch and note it in the PR — maintainer
     can fold it in).
   - **Changed a dependency's source** → refresh `patches/deps/<dep>.patch` the same way.
   - **New files under `Source/WebCore/port|shell|…`** → copy them into `src/` mirroring the path.
4. Open a PR describing what it fixes and how you tested it **on-device** (the packaged `.appx`, not a
   debug drop — release and debug behave differently for native/JNI/serialization paths).

If reworking the giant single patch is painful for your change, a clearly-described incremental diff
in the PR is fine; keeping the mega-patch clean is the maintainer's job.

## Dev loop

- **Build/package:** `scripts/build-shell.bat` → `WebCoreRenderShell_ARM.appx`. It builds the shell
  target, re-applies the network/branding manifest patch, rebuilds (repackage-only), and stages the
  appx. Prints `staged` + `SHELL_EXIT=0` on success.
- **Deploy:** `WinAppDeployCmd install -file …_ARM.appx -ip <phone-ip>` (phone in Developer mode), or
  the phone's Device Portal. Updates in place (stable package identity).
- **Debug:** the engine writes a diagnostic log on-device (pull it via Device Portal / your log
  mechanism). Logging goes through `WebCorePort::portLog()` / `PortImgLog()` in
  `Source/WebCore/port/WebCoreDriver.cpp`. Lines prefixed `mse` / `mf:` / `mem:` / `gate:` and ones
  containing `EXCEPTION`/`THREW`/`FAILED` are **flushed immediately** (so a hard close still captures
  the last line); everything else is batched. Add your own probes freely while iterating.

## Guidelines

- **Test on the actual device with the release `.appx`.** The Adreno-305 / ARM32 / AppContainer
  behavior is not reproducible on desktop, and release ≠ debug.
- **Fix root causes, don't stub around them.** This engine is real WebKit; hacks tend to resurface.
- **Keep diagnostics in during development.** Don't strip logging/probes mid-change — the next
  contributor needs the visibility. Clean up only when a piece is actually finished.
- **Mind the platform constraints** documented in BUILD.md (SDK 16299 / min 15063, `/MD` not `/MT`,
  no desktop-only Win32 APIs — the UWP AppContainer blocks them).

## Licensing

Revenant derives from [WebKit](https://webkit.org/) (BSD/LGPL) and links dependencies each under their
own license. Contributions follow the license of the file they touch. By opening a PR you agree your
contribution can be distributed under those terms.
