@echo off
REM ============================================================================
REM  Revenant build environment
REM
REM  1. Copy this file to  env.bat   (same folder, repo root)
REM  2. Edit the five paths below to match YOUR machine
REM  3. Every script in scripts\ and deps\ sources this automatically
REM
REM  env.bat is git-ignored so your local paths never get committed.
REM  See BUILD.md for the full walkthrough and AI-HELPER.md for an AI-driven setup.
REM ============================================================================

REM (1) ROOT — your build tree. Make an empty folder and populate it like this
REM     (AI-HELPER.md / BUILD.md explain each item; deps\MANIFEST.md pins versions):
REM
REM       %ROOT%\WebKit-2.36.8\    WebKit @ tag webkitgtk-2.36.8, then apply
REM                                patches\webkit-2.36.8-revenant.patch and copy src\ over it
REM       %ROOT%\deps\            each dependency cloned at its pinned commit + patch applied
REM       %ROOT%\boringssl-new\   BoringSSL source + patches\deps\boringssl.patch
REM       %ROOT%\cmake\           copy this repo's  cmake\  folder here (toolchain file)
REM       %ROOT%\icu\             prebuilt ICU (see deps\icu-build-static.bat)
REM       %ROOT%\tools\           cmake, ninja, gperf, bison, flex, meson on PATH
REM       %ROOT%\build-wincairo\  created by the build; leave empty
REM
set ROOT=C:\revenant

REM (2) CMAKE — CMake 3.12 or newer. The line below assumes you unpacked CMake into
REM     %ROOT%\tools\cmake ; change it if you use a system install.
set CMAKE=%ROOT%\tools\cmake\bin\cmake.exe

REM (3) VCVARSALL — Visual Studio 2022, ANY edition. Fix the edition in the path:
REM     Community, Professional, or Enterprise. The Desktop-C++ and
REM     "Universal Windows Platform" workloads + the ARM build tools must be installed.
set VCVARSALL=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat

REM (4) PKGCONF — pkgconf.exe. Easiest:  pip install pkgconf  then point at Scripts\pkgconf.exe.
set PKGCONF=%USERPROFILE%\AppData\Local\Programs\Python\Python3XX\Scripts\pkgconf.exe

REM (5) VS CMake generator name. Leave as-is for VS 2022; change only for a different VS.
set VSGEN=Visual Studio 17 2022

REM ---------------------------------------------------------------------------
REM  Fixed platform requirements — do NOT change unless you know exactly why.
REM  The W10M SDK must be 10.0.16299.0 and the app's TargetPlatformMinVersion
REM  10.0.15063.0 (phones report OS build 15254 and reject a higher min-version).
REM ---------------------------------------------------------------------------
set WINSDK=10.0.16299.0
set VSTOOLSET=v142,host=x64
set VCVARS_VER=14.29
if "%ROOT%"=="C:\revenant" echo [env] WARNING: env.bat still has example paths - edit it first.