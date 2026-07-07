@echo off
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
set TC=Z:\w10m-webengine\cmake\Toolchain-W10M-ARM32-UWP.cmake
set COMMON=-G "Visual Studio 17 2022" -A ARM -T v142,host=x64 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 -DCMAKE_TOOLCHAIN_FILE=%TC% -DBUILD_SHARED_LIBS=OFF

echo ===== SQLITE =====
"%CM%" -S Z:\w10m-webengine\deps\sqlite-amalg -B Z:\w10m-webengine\deps\sqlite-amalg\build-armuwp %COMMON%
echo SQLITE_CFG=%ERRORLEVEL%
"%CM%" --build Z:\w10m-webengine\deps\sqlite-amalg\build-armuwp --config Release
echo SQLITE_BUILD=%ERRORLEVEL%

echo ===== LIBPNG =====
"%CM%" -S Z:\w10m-webengine\deps\libpng -B Z:\w10m-webengine\deps\libpng\build-armuwp %COMMON% ^
  -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_ARM_NEON=off ^
  -DZLIB_ROOT=Z:\w10m-webengine\deps\_install -DZLIB_LIBRARY=Z:\w10m-webengine\deps\_install\lib\zlib.lib -DZLIB_INCLUDE_DIR=Z:\w10m-webengine\deps\_install\include
echo PNG_CFG=%ERRORLEVEL%
"%CM%" --build Z:\w10m-webengine\deps\libpng\build-armuwp --config Release --target png_static
echo PNG_BUILD=%ERRORLEVEL%

echo ===== LIBJPEG-TURBO =====
"%CM%" -S Z:\w10m-webengine\deps\libjpeg-turbo -B Z:\w10m-webengine\deps\libjpeg-turbo\build-armuwp %COMMON% ^
  -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF
echo JPEG_CFG=%ERRORLEVEL%
"%CM%" --build Z:\w10m-webengine\deps\libjpeg-turbo\build-armuwp --config Release --target jpeg-static
echo JPEG_BUILD=%ERRORLEVEL%
echo ALL_DONE
