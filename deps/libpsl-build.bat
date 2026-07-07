@echo off
call "F:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul
where meson >nul 2>nul || (echo installing meson & pip install --quiet meson)
cd /d Z:\w10m-webengine\deps\libpsl
rmdir /s /q build-armuwp 2>nul
meson setup build-armuwp --cross-file Z:\w10m-webengine\deps\cross-armuwp.txt -Druntime=no -Dbuiltin=true -Dtests=false 2>&1
echo MESON_CFG=%ERRORLEVEL%
meson compile -C build-armuwp 2>&1
echo MESON_BUILD=%ERRORLEVEL%
