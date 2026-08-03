@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
call "%VCVARSALL%" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul
where meson >nul 2>nul || (echo installing meson & pip install --quiet meson)
cd /d %ROOT%\deps\libpsl
rmdir /s /q build-armuwp 2>nul
meson setup build-armuwp --cross-file %ROOT%\deps\cross-armuwp.txt -Druntime=no -Dbuiltin=true -Dtests=false 2>&1
echo MESON_CFG=%ERRORLEVEL%
meson compile -C build-armuwp 2>&1
echo MESON_BUILD=%ERRORLEVEL%
