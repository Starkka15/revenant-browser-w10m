@echo off
set PATH=Z:\w10m-webengine\deps\tools\bin;%PATH%
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
set I=Z:/w10m-webengine/deps/_install
set PKG_CONFIG_PATH=Z:\w10m-webengine\deps\_install\lib\pkgconfig
set PKGCONF=C:/Users/Starkka15/AppData/Local/Programs/Python/Python314/Lib/site-packages/pkgconf/.bin/pkgconf.exe
"%CM%" -S Z:\w10m-webengine\WebKit-2.36.8 -B Z:\w10m-webengine\build-wincairo ^
  -G "Visual Studio 17 2022" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=Z:\w10m-webengine\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DPORT=WinCairo -DENABLE_WEBGL=ON -DENABLE_WEBGL2=OFF -Dangle_is_winuwp=TRUE -Dtarget_os=winuwp -Dangle_enable_d3d9=FALSE -DGPERF_EXECUTABLE=Z:/w10m-webengine/deps/tools/bin/gperf.exe -DBISON_EXECUTABLE=Z:/w10m-webengine/deps/tools/bin/bison.exe -DFLEX_EXECUTABLE=Z:/w10m-webengine/deps/tools/bin/flex.exe -DCMAKE_PREFIX_PATH=%I% -DPKG_CONFIG_EXECUTABLE=%PKGCONF% ^
  -DICU_ROOT=Z:/w10m-webengine/icu -DICU_INCLUDE_DIR=Z:/w10m-webengine/icu/include ^
  -DENABLE_JIT=ON -DENABLE_DFG_JIT=ON -DENABLE_C_LOOP=OFF -DUSE_CAPSTONE=OFF -DENABLE_DISASSEMBLER=OFF ^
  -DENABLE_WEB_CRYPTO=ON -DENABLE_MEDIA_SOURCE=ON -DENABLE_WEBASSEMBLY=ON -DUSE_MEDIA_FOUNDATION=ON ^
  -DENABLE_WEB_AUDIO=OFF -DENABLE_OFFSCREEN_CANVAS=ON -DENABLE_SERVICE_WORKER=ON ^
  -DENABLE_TOUCH_EVENTS=ON -DENABLE_POINTER_LOCK=ON -DENABLE_DOWNLOAD_ATTRIBUTE=ON -DENABLE_AUTOCAPITALIZE=ON ^
  -DENABLE_MHTML=ON -DENABLE_SERVER_PRECONNECT=ON ^
  -DENABLE_INTELLIGENT_TRACKING_PREVENTION=ON -DENABLE_SMOOTH_SCROLLING=ON ^
  -DENABLE_VARIATION_FONTS=OFF -DENABLE_CONTENT_EXTENSIONS=OFF ^
  -DDEVELOPER_MODE=OFF -DENABLE_WEBKIT_LEGACY=OFF -DENABLE_WEBKIT=ON -DENABLE_XSLT=OFF -DUSE_LCMS=OFF -DUSE_JPEGXL=OFF -DUSE_OPENJPEG=OFF -DUSE_WOFF2=ON -DUSE_WEBP=ON ^
  -DJPEG_LIBRARY=%I%/lib/jpeg.lib -DJPEG_INCLUDE_DIR=%I%/include ^
  -DPNG_LIBRARY=%I%/lib/libpng.lib -DPNG_PNG_INCLUDE_DIR=%I%/include ^
  -DZLIB_LIBRARY=%I%/lib/zlib.lib -DZLIB_INCLUDE_DIR=%I%/include ^
  -DSQLITE3_LIBRARY=%I%/lib/sqlite3.lib -DSQLITE3_INCLUDE_DIR=%I%/include ^
  -DLIBXML2_LIBRARY=%I%/lib/libxml2.lib -DLIBXML2_INCLUDE_DIR=%I%/include/libxml2 ^
  -DLibPSL_INCLUDE_DIR=%I%/include -DLibPSL_LIBRARY=%I%/lib/psl.lib -DWEBP_INCLUDE_DIR=%I%/include -DWEBP_LIBRARY=%I%/lib/webp.lib -DWOFF2_LIBRARY=%I%/lib/woff2common.lib -DWOFF2_DEC_LIBRARY=%I%/lib/woff2dec.lib -DWOFF2_INCLUDE_DIR=%I%/include -DOPENSSL_ROOT_DIR=%I% -DOPENSSL_SSL_LIBRARY=%I%/lib/libssl.lib -DOPENSSL_CRYPTO_LIBRARY=%I%/lib/libcrypto.lib -DOPENSSL_INCLUDE_DIR=%I%/include ^
  -DCMAKE_EXE_LINKER_FLAGS="onecore.lib /FORCE:MULTIPLE" -DCMAKE_SHARED_LINKER_FLAGS="onecore.lib /FORCE:MULTIPLE"
echo WINCAIRO_CFG=%ERRORLEVEL%
rem Re-add AppContainer network capabilities that CMake strips from the manifest on every configure.
powershell -NoProfile -ExecutionPolicy Bypass -File Z:\w10m-webengine\patch-manifest.ps1
