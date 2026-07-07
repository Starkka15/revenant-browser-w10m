@echo off
REM Build libnghttp2 (HTTP/2 state machine) as a static lib for ARM32 UWP, so curl can be
REM rebuilt with USE_NGHTTP2=ON. Library-only: no apps/tools/tests (those need extra deps).
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
set I=Z:/w10m-webengine/deps/_install
"%CM%" -S Z:\w10m-webengine\deps\nghttp2 -B Z:\w10m-webengine\deps\nghttp2\build-armuwp ^
  -G "Visual Studio 17 2022" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=Z:\w10m-webengine\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DENABLE_LIB_ONLY=ON -DBUILD_STATIC_LIBS=ON -DBUILD_SHARED_LIBS=OFF ^
  -DBUILD_TESTING=OFF -DENABLE_DOC=OFF -DWITH_LIBXML2=OFF -DWITH_JEMALLOC=OFF ^
  -DCMAKE_INSTALL_PREFIX=%I%
echo NGHTTP2_CFG=%ERRORLEVEL%
"%CM%" --build Z:\w10m-webengine\deps\nghttp2\build-armuwp --config Release --target nghttp2_static
echo NGHTTP2_BUILD=%ERRORLEVEL%
REM Stage lib + headers into _install for curl's FindNGHTTP2. ARCHIVE_OUTPUT_NAME=nghttp2 (no suffix
REM unless both shared+static), so the static archive is nghttp2.lib.
copy /Y Z:\w10m-webengine\deps\nghttp2\build-armuwp\lib\Release\nghttp2.lib %I%\lib\nghttp2.lib
xcopy /Y /E /I Z:\w10m-webengine\deps\nghttp2\lib\includes\nghttp2 %I%\include\nghttp2
copy /Y Z:\w10m-webengine\deps\nghttp2\build-armuwp\lib\includes\nghttp2\nghttp2ver.h %I%\include\nghttp2\nghttp2ver.h
echo NGHTTP2_STAGE=%ERRORLEVEL%
