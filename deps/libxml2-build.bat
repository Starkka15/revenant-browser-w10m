@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
set CM=%CMAKE%
set SRC=%ROOT%\deps\libxml2
set BLD=%ROOT%\deps\libxml2\build-armuwp
"%CM%" -S %SRC% -B %BLD% -G "%VSGEN%" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=%ROOT%\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DBUILD_SHARED_LIBS=OFF ^
  -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_LZMA=OFF ^
  -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_PROGRAMS=OFF ^
  -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_THREADS=ON -DLIBXML2_WITH_MODULES=OFF
echo CFG_EXIT=%ERRORLEVEL%
"%CM%" --build %BLD% --config Release
echo BUILD_EXIT=%ERRORLEVEL%
