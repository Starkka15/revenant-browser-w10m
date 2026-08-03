@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
REM Build libnghttp2 (HTTP/2 state machine) as a static lib for ARM32 UWP, so curl can be
REM rebuilt with USE_NGHTTP2=ON. Library-only: no apps/tools/tests (those need extra deps).
set CM=%CMAKE%
set I=%ROOT%/deps/_install
"%CM%" -S %ROOT%\deps\nghttp2 -B %ROOT%\deps\nghttp2\build-armuwp ^
  -G "%VSGEN%" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=%ROOT%\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DENABLE_LIB_ONLY=ON -DBUILD_STATIC_LIBS=ON -DBUILD_SHARED_LIBS=OFF ^
  -DBUILD_TESTING=OFF -DENABLE_DOC=OFF -DWITH_LIBXML2=OFF -DWITH_JEMALLOC=OFF ^
  -DCMAKE_INSTALL_PREFIX=%I%
echo NGHTTP2_CFG=%ERRORLEVEL%
"%CM%" --build %ROOT%\deps\nghttp2\build-armuwp --config Release --target nghttp2_static
echo NGHTTP2_BUILD=%ERRORLEVEL%
REM Stage lib + headers into _install for curl's FindNGHTTP2. ARCHIVE_OUTPUT_NAME=nghttp2 (no suffix
REM unless both shared+static), so the static archive is nghttp2.lib.
copy /Y %ROOT%\deps\nghttp2\build-armuwp\lib\Release\nghttp2.lib %I%\lib\nghttp2.lib
xcopy /Y /E /I %ROOT%\deps\nghttp2\lib\includes\nghttp2 %I%\include\nghttp2
copy /Y %ROOT%\deps\nghttp2\build-armuwp\lib\includes\nghttp2\nghttp2ver.h %I%\include\nghttp2\nghttp2ver.h
echo NGHTTP2_STAGE=%ERRORLEVEL%
