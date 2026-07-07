@echo off
REM T8a: zlib 1.3.1 static for ARM32 UWP (WinCairo dep). Recipe basis for the other C libs.
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
set SRC=Z:\w10m-webengine\deps\zlib
set BLD=Z:\w10m-webengine\deps\zlib\build-armuwp
"%CM%" -S %SRC% -B %BLD% ^
  -G "Visual Studio 17 2022" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=Z:\w10m-webengine\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DBUILD_SHARED_LIBS=OFF
echo CFG_EXIT=%ERRORLEVEL%
"%CM%" --build %BLD% --config Release --target zlibstatic
echo BUILD_EXIT=%ERRORLEVEL%
