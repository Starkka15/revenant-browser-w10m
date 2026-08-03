@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
set CM=%CMAKE%
set TC=%ROOT%\cmake\Toolchain-W10M-ARM32-UWP.cmake
set COMMON=-G "%VSGEN%" -A ARM -T v142,host=x64 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 -DCMAKE_TOOLCHAIN_FILE=%TC% -DBUILD_SHARED_LIBS=OFF

echo ===== LIBWEBP =====
"%CM%" -S %ROOT%\deps\libwebp -B %ROOT%\deps\libwebp\build-armuwp %COMMON% ^
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF ^
  -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF ^
  -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_WEBP_JS=OFF -DWEBP_USE_THREAD=ON
echo WEBP_CFG=%ERRORLEVEL%
"%CM%" --build %ROOT%\deps\libwebp\build-armuwp --config Release --target webp --target webpdemux
echo WEBP_BUILD=%ERRORLEVEL%

echo ===== BROTLI =====
"%CM%" -S %ROOT%\deps\brotli -B %ROOT%\deps\brotli\build-armuwp %COMMON% -DBROTLI_DISABLE_TESTS=ON
echo BROTLI_CFG=%ERRORLEVEL%
"%CM%" --build %ROOT%\deps\brotli\build-armuwp --config Release
echo BROTLI_BUILD=%ERRORLEVEL%
echo ALL_DONE
