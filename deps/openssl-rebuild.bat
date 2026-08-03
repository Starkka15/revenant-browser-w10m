@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
call "%VCVARSALL%" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul
cd /d %ROOT%\deps\openssl
nmake clean >nul 2>&1
nmake build_libs
echo NMAKE_EXIT=%ERRORLEVEL%
