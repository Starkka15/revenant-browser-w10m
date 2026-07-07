@echo off
call "F:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul
cd /d Z:\w10m-webengine\deps\pixman
rmdir /s /q build-armuwp 2>nul
meson setup build-armuwp --cross-file Z:\w10m-webengine\deps\cross-armuwp.txt ^
  -Dgtk=disabled -Dlibpng=disabled -Dtests=disabled ^
  -Dmmx=disabled -Dsse2=disabled -Dssse3=disabled -Dvmx=disabled -Dloongson-mmi=disabled -Dmips-dspr2=disabled -Darm-simd=disabled -Dneon=disabled -Da64-neon=disabled -Diwmmxt=disabled -Dgnu-inline-asm=disabled 2>&1
echo PIXMAN_CFG=%ERRORLEVEL%
meson compile -C build-armuwp 2>&1
echo PIXMAN_BUILD=%ERRORLEVEL%
