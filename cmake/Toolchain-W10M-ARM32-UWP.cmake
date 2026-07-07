# Toolchain-W10M-ARM32-UWP.cmake — WPE WebKit on Windows 10 Mobile (ARM32 UWP)
#
# Use WITH the Visual Studio generator (ARM32 UWP only builds via the v141 toolset; the
# modern VS toolset dropped 32-bit ARM — see SPEC §C). Generator/arch/toolset are passed on
# the cmake command line (CMake forbids setting them in a toolchain file):
#
#   cmake -G "Visual Studio 17 2022" -A ARM -T v141,host=x64 ^
#         -DCMAKE_TOOLCHAIN_FILE=<this file> -S <src> -B <build>
#
#   -A ARM        = 32-bit ARM (NOT ARM64)
#   -T v141       = MSVC 14.16 (VS2017 15.9), the toolset that still ships ARM32 (SPEC §V.2)
#   host=x64      = x64 host cross-compiling to ARM
#
# (Generator name depends on the installed VS — adjust to the VS18/2022 generator cmake reports.)

# ── compiler detection: build a STATIC LIB, not an exe ───────────────────────
# We build static libs (WTF/JSC/WebCore). CMake's default compiler check links a full UWP
# executable, which needs WindowsApp.lib + an app manifest on the probe's LIB path (→ LNK1104
# during detection). A static-lib probe sidesteps that. The real app exe (C++/CX shell, SPEC
# T14) links WindowsApp.lib in its own VS project.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ── target platform: UWP / AppContainer ──────────────────────────────────────
set(CMAKE_SYSTEM_NAME       WindowsStore)        # UWP app partition (WINAPI_FAMILY=APP)
set(CMAKE_SYSTEM_VERSION    10.0.16299.0)        # build AGAINST 16299 (SPEC §V.1)
# Cap auto-SDK selection at 16299 — else CMake's compiler-detection TryCompile picks the
# latest SDK (26100), which trips VS's "ARM config needs TPV ≤ 22621" deprecation gate.
# This MAX caps it everywhere (incl. the TryCompile vcxproj), unlike CMAKE_SYSTEM_VERSION alone.
set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION_MAXIMUM 10.0.16299.0)
set(CMAKE_SYSTEM_PROCESSOR  ARM)                 # 32-bit ARM

# ── min-version gotcha (LOAD-BEARING, SPEC §V.1) ─────────────────────────────
# Build against 16299 but MIN must be 15063, or the .appx won't install on the phone
# (phones report 15254; there is no 15254/16299 *mobile* min you can set).
set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION 10.0.15063.0)

# ── partition: APP (UWP) for every translation unit ──────────────────────────
# ARM Windows is UWP-only — the CRT forbids WINAPI_FAMILY_DESKTOP_APP on ARM (corecrt.h #error).
# So strict APP partition; WebKit's desktop-Win32 platform bits get real UWP reimplementations.
add_compile_definitions(
    WINAPI_FAMILY=WINAPI_FAMILY_APP
    __WRL_NO_DEFAULT_LIB__
    # Link ICU statically (our own static icuuc/icuin/icudt — the browser must ship its own ICU;
    # the OS icu.dll is not an option). U_STATIC_IMPLEMENTATION makes ICU's U_EXPORT empty so
    # headers declare plain symbols (no dllimport) matching the static libs in icu/lib.
    U_STATIC_IMPLEMENTATION
)

# ── DIAGNOSTIC: disable /GS stack cookies ────────────────────────────────────
# On-device, jsc fast-failed FAST_FAIL_STACK_COOKIE_CHECK_FAILURE in parseFunctionBody's /GS
# epilogue, yet __security_cookie was initialized AND intact copies sat on the frame — pointing
# at an ARM32 /GS codegen/frame-pointer false-positive rather than a real overrun. Disable /GS to
# test: if JS runs, it was the canary misfiring; if it AVs, there's real corruption to chase.
add_compile_options(/GS-)

# ── DeviceIoControl (and friends) for the MSVC STL <filesystem> ──────────────
# CMake auto-links the UWP umbrella WindowsApp.lib for WindowsStore targets, but it omits
# DeviceIoControl, which MSVC's std::filesystem reparse-point handling pulls in (WTF FileSystem.cpp).
# onecore.lib is the OneCore umbrella that exports it. We sideload (non-Store), so WACK doesn't
# gate us. Appended to exe/dll links (the static-lib compiler probe never links, so harmless there).
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " onecore.lib")
string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " onecore.lib")

# UWP/AppContainer apps must declare app-container; CMake handles AppContainerApplication
# via the WindowsStore system name. Keep this file generator-driven; per-target appx bits
# (manifest, capabilities incl. codeGeneration/internetClient — SPEC §C) live in the shell project.
