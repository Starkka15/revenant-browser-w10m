@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
REM T8a: zlib 1.3.1 static for ARM32 UWP (WinCairo dep). Recipe basis for the other C libs.
set CM=%CMAKE%
set SRC=%ROOT%\deps\zlib
set BLD=%ROOT%\deps\zlib\build-armuwp
"%CM%" -S %SRC% -B %BLD% ^
  -G "%VSGEN%" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=%ROOT%\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DBUILD_SHARED_LIBS=OFF
echo CFG_EXIT=%ERRORLEVEL%
"%CM%" --build %BLD% --config Release --target zlibstatic
echo BUILD_EXIT=%ERRORLEVEL%
