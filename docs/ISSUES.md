# Revenant — Known Issues Tracker

Bugs/gaps found from on-device logs. Ranked within each group. Status: 🔴 open · 🟡 in progress · 🟢 fixed (unverified) · ✅ verified on-device.

## Crashes / stability

### I-01 · YouTube video crash = GPU texture OOM at high DPR 🟡
**Device:** HP Elite X3 (Snapdragon 820, **Adreno 530**, 4GB RAM but W10M caps app at 2048MB → tier=2;
DPR **3.5**, 1440p 5.96"). **Site:** m.youtube.com/watch.
**Evidence:** `GLERR ... HRESULT: 0x8007000E (E_OUTOFMEMORY): Error allocating Texture2D` → `GL_OUT_OF_MEMORY 0x0505` → C++ `_com_error` bursts at `stage=c5:layerPaint` (caught internally, `RENDER-CRASH` never fired) → mass `TEXDEL` eviction of 1442×2000/1442×2140 tiles → failure reaches `x9:xaml-commit` (SwapChainPanel present, **UI thread — outside render-thread SEH**) → OS kills process → relaunch. System RAM was fine (31%); this is **GPU** texture memory.
**Root:** at DPR 3.5 every backing store is a 3.5× D3D texture; YouTube watch page's many big layers exhaust the app's GPU texture budget. New failure axis, orthogonal to the system-memory tiering (a tier-2 device with a huge high-DPR screen OOMs the GPU where a tier-0 low-DPR device wouldn't).
**Fix (in progress):** (a) cap the **raster/composite scale** on very-high-DPR devices — render backings + EGL surface at ≤~2.5×, let the SwapChainPanel upscale to physical; keep cssW=412 (preserve mobile layout + iOS impersonation). ~2× texture-byte cut. (b) harden the `x9:xaml-commit` present path (SEH/try-catch so a present failure drops a frame, not the app). (c) on Texture2D OOM, proactively evict the tile pool + cap the tile budget instead of retrying to alloc.

### I-02 · pornhub age-gate blur freeze (tier-0) 🟢 (Fix A shipped, survivable)
640XL: full-page `filter: blur()` software-rastered every frame → 3-15s `boxBlur` main-thread burns. Fix A (tier-0 filter-scale pixel-budget clamp) cut freeze→survivable jank. Still ~3s; could go further. See `RenderLayerFilters.cpp`.

### I-03 · MSE teardown deadlock made a hung app unkillable 🟢
`PortMseDestroy` closed the MF player synchronously on the main thread during `Document::commonTeardown` → 180s+ `IDLE(deadlock/blocked)` wedge. Fixed: close on a threadpool thread (`ShellMse.cpp`).

## Media

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
