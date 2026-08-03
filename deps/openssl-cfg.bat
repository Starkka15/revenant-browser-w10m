@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
call "%VCVARSALL%" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul
cd /d %ROOT%\deps\openssl
perl Configure VC-WIN32-ARM-UWP no-asm no-shared no-tests no-apps no-docs --prefix=%ROOT%\deps\_install-openssl 2>&1
echo CONFIGURE_EXIT=%ERRORLEVEL%
