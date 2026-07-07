@echo off
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
set TC=Z:\w10m-webengine\cmake\Toolchain-W10M-ARM32-UWP.cmake
set COMMON=-G "Visual Studio 17 2022" -A ARM -T v142,host=x64 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 -DCMAKE_TOOLCHAIN_FILE=%TC% -DBUILD_SHARED_LIBS=OFF

echo ===== LIBWEBP =====
"%CM%" -S Z:\w10m-webengine\deps\libwebp -B Z:\w10m-webengine\deps\libwebp\build-armuwp %COMMON% ^
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF ^
  -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF ^
  -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_WEBP_JS=OFF -DWEBP_USE_THREAD=ON
echo WEBP_CFG=%ERRORLEVEL%
"%CM%" --build Z:\w10m-webengine\deps\libwebp\build-armuwp --config Release --target webp --target webpdemux
echo WEBP_BUILD=%ERRORLEVEL%

echo ===== BROTLI =====
"%CM%" -S Z:\w10m-webengine\deps\brotli -B Z:\w10m-webengine\deps\brotli\build-armuwp %COMMON% -DBROTLI_DISABLE_TESTS=ON
echo BROTLI_CFG=%ERRORLEVEL%
"%CM%" --build Z:\w10m-webengine\deps\brotli\build-armuwp --config Release
echo BROTLI_BUILD=%ERRORLEVEL%
echo ALL_DONE
