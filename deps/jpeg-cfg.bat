@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
set CM=%CMAKE%
"%CM%" -S %ROOT%\deps\libjpeg-turbo -B %ROOT%\deps\libjpeg-turbo\build-armuwp ^
  -G "%VSGEN%" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=%ROOT%\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF
echo JPEG_CFG=%ERRORLEVEL%
