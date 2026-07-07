@echo off
call "F:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul
set PKG_CONFIG_PATH=Z:\w10m-webengine\deps\_install\lib\pkgconfig
cd /d Z:\w10m-webengine\deps\cairo
rmdir /s /q build-armuwp 2>nul
meson setup build-armuwp --cross-file Z:\w10m-webengine\deps\cross-armuwp.txt -Ddwrite=enabled -Dfreetype=disabled -Dfontconfig=disabled -Dquartz=disabled -Dxlib=disabled -Dxcb=disabled -Dpng=enabled -Dzlib=enabled -Dtee=disabled -Dglib=disabled -Dtests=disabled -Dspectre=disabled -Dsymbol-lookup=disabled >setup.log 2>&1
echo CAIRO_CFG=%ERRORLEVEL%
meson compile -C build-armuwp 2>&1 | findstr /C:".c" /C:".cpp" /C:"oaidl" /C:"error C" /C:"FAILED" /C:"propidl"
echo CAIRO_BUILD=%ERRORLEVEL%
