@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
call "%VCVARSALL%" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul
set PKG_CONFIG_PATH=%ROOT%\deps\_install\lib\pkgconfig
cd /d %ROOT%\deps\cairo
rmdir /s /q build-armuwp 2>nul
meson setup build-armuwp --cross-file %ROOT%\deps\cross-armuwp.txt -Ddwrite=enabled -Dfreetype=disabled -Dfontconfig=disabled -Dquartz=disabled -Dxlib=disabled -Dxcb=disabled -Dpng=enabled -Dzlib=enabled -Dtee=disabled -Dglib=disabled -Dtests=disabled -Dspectre=disabled -Dsymbol-lookup=disabled >setup.log 2>&1
echo CAIRO_CFG=%ERRORLEVEL%
meson compile -C build-armuwp 2>&1 | findstr /C:".c" /C:".cpp" /C:"oaidl" /C:"error C" /C:"FAILED" /C:"propidl"
echo CAIRO_BUILD=%ERRORLEVEL%
