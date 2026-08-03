@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
REM PIXMAN. Was: buildtype defaulted (asserts on) and every SIMD path disabled.
REM
REM The SIMD flags are NOT an oversight and must stay off: pixman's ARM SIMD/NEON fast paths are
REM ~4000 lines of ARM-mode (A32) GNU assembly, and Windows on ARM32 mandates THUMB-2 -- clang
REM rejects the files outright ("target does not support ARM mode"), and forcing .thumb produces
REM 86,000 assembler errors. They are unusable here, full stop. (The route to NEON on this platform
REM is MSVC <arm_neon.h> intrinsics, which do compile to Thumb-2+NEON -- a separate job.)
REM
REM What WAS free: buildtype=release + NDEBUG. pixman ships with assertions and sanity checks that
REM are compiled in unless b_ndebug is set, and it is the inner loop of every blend/composite/scale
REM the browser does.
call "%VCVARSALL%" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul
cd /d %ROOT%\deps\pixman
rmdir /s /q build-armuwp 2>nul
meson setup build-armuwp --cross-file %ROOT%\deps\cross-armuwp.txt ^
  --buildtype=release -Doptimization=2 -Db_ndebug=true ^
  -Dgtk=disabled -Dlibpng=disabled -Dtests=disabled ^
  -Dmmx=disabled -Dsse2=disabled -Dssse3=disabled -Dvmx=disabled -Dloongson-mmi=disabled -Dmips-dspr2=disabled -Darm-simd=disabled -Dneon=disabled -Da64-neon=disabled -Diwmmxt=disabled -Dgnu-inline-asm=disabled 2>&1
echo PIXMAN_CFG=%ERRORLEVEL%
meson compile -C build-armuwp 2>&1
echo PIXMAN_BUILD=%ERRORLEVEL%
copy /y build-armuwp\pixman\libpixman-1.a %ROOT%\deps\_install\lib\pixman-1.lib 2>&1
echo PIXMAN_INSTALL=%ERRORLEVEL%
