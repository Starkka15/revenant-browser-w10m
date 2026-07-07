@echo off
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
"%CM%" -S Z:\w10m-webengine\deps\libjpeg-turbo -B Z:\w10m-webengine\deps\libjpeg-turbo\build-armuwp ^
  -G "Visual Studio 17 2022" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=Z:\w10m-webengine\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF
echo JPEG_CFG=%ERRORLEVEL%
