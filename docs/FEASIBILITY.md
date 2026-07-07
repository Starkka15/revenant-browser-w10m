# Modern Web Engine for Windows 10 Mobile — Feasibility Spike

> **STATUS (2026-06-13): SPIKE BANKED — go/no-go = GO.** The two scariest unknowns
> came back green: JIT works on W10M (Q1 ✅), and the ARM32-UWP toolchain is present
> and proven (Q2 prereqs ✅).
>
> **NEXT SESSION — START HERE (do not skip):**
> 1. **Grab WebKit source** onto `vm-shared` — **WPE WebKit** flavor
>    (`github.com/WebKit/WebKit`, build via WPE port). It's large (GBs); shallow
>    clone / a tagged release is fine.
> 2. **Write a proper `SPEC.md` from the get-go** (use the `/spec` workflow). The
>    prior DeepSeek attempt died from no spec + desktop-first + hand-conversion.
>    SPEC must pin: target (ARM32 UWP, WINAPI_FAMILY=APP, **TargetPlatformVersion
>    = 10.0.16299.0**, **TargetPlatformMinVersion = 10.0.15063.0** — see ⚠ below,
>    MSVC 14.16 toolset), build order (WTF → JSC → WebCore → WPE backend → embed),
>    the port layer surface (D3D11/ANGLE graphics, WinHTTP net, MF media,
>    local-folder storage), the JIT/W^X invariant, and the §V invariants.
> 3. Only then start the cross-build at the first milestone (WTF configures/compiles).
>
> ⚠ **Min-version gotcha (load-bearing):** `TargetPlatformMinVersion` MUST be
> **10.0.15063.0**. W10M phones report OS build **15254** (the mobile equivalent of
> desktop **16299**), but there is no 15254/16299 *mobile* min you can set — if
> MinVersion is 16299 (or 15254), the package **won't install/run on the phone at
> all**. Build *against* 16299 (TargetPlatformVersion), but set Min to 15063.
> PocketTavern.UWP already does exactly this. Applies to every W10M UWP app.

**Goal:** Determine whether an embeddable modern web engine (target: **WPE WebKit**,
designed for embedding) can be ported to **Windows 10 Mobile, ARM32, UWP** — so
that apps (TokTikX, FaceBookX, a real browser) become thin native shells around a
WebView and let the site's own JS handle signing / Frontier DMs / For You / etc.

**Why this is the meta-unlock:** it dissolves the TikTok/Facebook reverse-engineering
walls (no API/signing to maintain — the site runs) and gives W10M a real browser.

**Approach:** cheap spike answering the killer unknowns BEFORE committing months.
Must be incremental / SPEC-driven / build-at-every-step (NOT LLM-winged — the prior
DeepSeek WPE attempt was scrapped for exactly that reason).

---

## SPIKE Q1 — Can a sideloaded W10M UWP app get JIT?  ✅ YES (RESOLVED)

This was the make-or-break: JS engines want JIT, and UWP AppContainer enforces
W^X (no write+execute memory). If JIT were impossible we'd be stuck with an
interpreter (likely too slow for heavy SPAs on 2013 hardware).

**Findings:**
1. **`codeGeneration` is a *general* (non-restricted) capability** — it's in the
   normal UWP capability table, NOT the restricted list. (MS Learn,
   app-capability-declarations.) It enables `VirtualAllocFromApp` /
   `VirtualProtectFromApp` / `CreateFileMappingFromApp` / `MapViewOfFileFromApp`.
2. **Sideloading needs no approval** — MS docs: "you can sideload apps that declare
   restricted capabilities without needing to receive any approval." We're
   sideloading (developer-unlocked Lumia), and codeGeneration isn't even restricted.
3. **Proven on real Windows 10 Mobile hardware** — PPSSPP's JIT/dynarec runs on
   W10M phones. Henrik Rydgård (PPSSPP author), "Porting to UWP":
   > "Windows 10 lets you declare that you need the 'Code Generation' permission.
   > With that, you can VirtualAlloc a block of R/W memory and then
   > VirtualProtectFromApp it over to R/X. R/W/X is not permitted but we already
   > had full support for living with that restriction — we simply switch pages
   > back to writable when needed, then make them executable again… this worked
   > on the first try." Shipped "for both PC and Windows 10 for phones."
4. **The W^X mechanism is standard** — flip target pages W→X around codegen. This
   is the **same model JSC already uses on iOS** (hardened-runtime / fast permission
   switching). So JavaScriptCore's JIT can operate under this constraint without
   new invention. (The user's "devise something like JIT in a container" idea — we
   don't need to; this *is* it, and it's already how JSC/iOS work.)

**Verdict:** JIT is viable on W10M. JS perf will be real-engine tier (with a small
W^X page-protection overhead), **not** interpreter-crippled. The feared showstopper
is not a showstopper.

---

## Constraints confirmed along the way (from the PPSSPP UWP port)

These shape the WebKit port-layer work (all surmountable, but real):

- **Graphics: Direct3D 11 only.** No OpenGL, no Vulkan, no D3D9. → WebKit must
  render via **ANGLE (D3D11 → GLES2)**, which is the standard WPE/WebKit GL path.
- **Filesystem:** free libc/Win32 file access only inside the app's *local folder*;
  anything outside needs async `StorageFile` APIs. → keep WebKit cache/storage in
  the local folder.
- **Memory mapping quirk:** `MapViewOfFileFromApp` **cannot choose the mapping
  address** (unlike classic `MapViewOfFile`). → matters for engines that rely on
  fixed-address reservations; **JSC's Gigacage / large reserved regions need
  checking** (see Q4).
- **App model:** a thin **C++/CX** layer sets up the window/lifecycle; the engine
  then runs as a plain **Direct3D app inside**. → this is exactly the embedding
  shape we want (thin CX shell hosting native WPE).

---

## Build environment (confirmed on the VM, 2026-06-13)

Workflow: author on Linux in `/mnt/ssd-raid/vm-shared` (= `Z:\` in the VM), build
in the Windows VM. VM reachable via **`ssh -p 2222 Starkka15@localhost`** (key-based,
no password). Host: DESKTOP-RF457AJ.
- **VS**: VS 18 Community + VS 2022 Professional (on `F:\`).
- **ARM32 C++ MSVC toolset present** — incl. `14.16.27023` (VS2017 15.9 toolset,
  the W10M gold standard) plus newer (14.29 / 14.38 / 14.44).
- **Windows SDKs** (`C:\Program Files (x86)\Windows Kits\10`): 10240, **15063**,
  **16299**, 19041, 22621, 26100. **15063 and 16299 have ARM UWP libs**
  (`um\arm\WindowsApp.lib`) — the exact W10M targets.
- **CMake 4.3.3**.
- **Toolchain is proven working**: PocketTavern.UWP already builds ARM UWP packages
  on this VM via this exact workflow. So Q2 is NOT "does the toolchain exist" — it's
  the WebKit-specific cross-build below.

## Remaining spike questions (next, before committing)

- **Q2 — WebKit cross-build:** Toolchain prerequisites ✅ confirmed (above). Real work:
  a **CMake toolchain file targeting ARM32 UWP** (WINAPI_FAMILY=APP, the 16299 SDK,
  the right MSVC toolset) + getting WebKit's build system (`Source/cmake`, WTF, JSC)
  to configure and compile against it. Milestone: WTF + JSC build for UWP/ARM, even
  if nothing renders yet. This is the recurring wall — and where the prior DeepSeek
  attempt fell apart by going desktop-first then hand-converting.
- **Q3 — Graphics bring-up:** Get one page (or even one WebKit layer) rendering into
  a **SwapChainPanel via ANGLE D3D11** — the "hello world" that proves the GL path.
- **Q4 — JSC memory model:** Does JSC's Gigacage / fixed reservations work given
  `MapViewOfFileFromApp` can't pick addresses? (May need to disable Gigacage or use
  a relocatable config.)
- **Q5 — Networking:** WebKit's loader inside the UWP sandbox — port to WinHTTP /
  Windows.Web, or a sandbox-friendly curl. (TLS/sockets allowed in UWP with the
  `internetClient` capability.)
- **Q6 — Media:** `<video>`/MSE backend — Media Foundation? This is heavy; may be
  phase-2 (TikTok needs video, but a first milestone can skip it).
- **Q7 — Perf reality:** WPE + JSC-JIT on Lumia 1520 (Snapdragon 800, 2013) running
  a heavy SPA — usable or sluggish? Only answerable once something runs.

---

## Sources

- MS Learn — App capability declarations (codeGeneration is general; sideload needs
  no approval): https://learn.microsoft.com/en-us/windows/uwp/packaging/app-capability-declarations
- Henrik Rydgård, "Porting to Windows 10 UWP" (PPSSPP; JIT via codeGeneration +
  VirtualProtectFromApp works on W10M phones, "first try"): http://blog.henrikrydgard.com/porting-to-uwp/
- PPSSPP UWP PR (perneky/Rydgård), JIT + `-codeGeneration`:
  https://github.com/hrydgard/ppsspp/pull/8315
