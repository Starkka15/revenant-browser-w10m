@echo off
REM CAIRO. This was being built with buildtype=debug -> optimization=0 (/Od), assertions ON.
REM
REM Cairo is the software rasterizer for the ENTIRE page: it is 58.8s of the 137s of main-thread CPU
REM measured on-device (43% of everything the browser does -- more than JS and layout combined), and
REM every byte of it was compiled with optimization switched OFF. That is a 3-10x self-inflicted
REM penalty on the hottest code in the project.
call "F:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul
set PKG_CONFIG_PATH=Z:\w10m-webengine\deps\_install\lib\pkgconfig
cd /d Z:\w10m-webengine\deps\cairo
rmdir /s /q build-armuwp 2>nul
meson setup build-armuwp --cross-file Z:\w10m-webengine\deps\cross-armuwp.txt --wrap-mode=nofallback ^
  --buildtype=release -Doptimization=2 -Db_ndebug=true ^
  -Ddwrite=enabled -Dfreetype=disabled -Dfontconfig=disabled -Dquartz=disabled ^
  -Dxlib=disabled -Dxcb=disabled -Dpng=enabled -Dzlib=enabled -Dtee=disabled ^
  -Dglib=disabled -Dtests=disabled -Dspectre=disabled -Dsymbol-lookup=disabled 2>&1
echo CAIRO_CFG=%ERRORLEVEL%
meson compile -C build-armuwp 2>&1
echo CAIRO_BUILD=%ERRORLEVEL%
copy /y build-armuwp\src\libcairo.a Z:\w10m-webengine\deps\_install\lib\cairo.lib 2>&1
echo CAIRO_INSTALL=%ERRORLEVEL%
