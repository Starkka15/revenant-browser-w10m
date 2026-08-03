@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
set CM=%CMAKE%
set TC=%ROOT%\cmake\Toolchain-W10M-ARM32-UWP.cmake
set COMMON=-G "%VSGEN%" -A ARM -T v142,host=x64 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 -DCMAKE_TOOLCHAIN_FILE=%TC% -DBUILD_SHARED_LIBS=OFF

echo ===== SQLITE =====
"%CM%" -S %ROOT%\deps\sqlite-amalg -B %ROOT%\deps\sqlite-amalg\build-armuwp %COMMON%
echo SQLITE_CFG=%ERRORLEVEL%
"%CM%" --build %ROOT%\deps\sqlite-amalg\build-armuwp --config Release
echo SQLITE_BUILD=%ERRORLEVEL%

echo ===== LIBPNG =====
"%CM%" -S %ROOT%\deps\libpng -B %ROOT%\deps\libpng\build-armuwp %COMMON% ^
  -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_ARM_NEON=off ^
  -DZLIB_ROOT=%ROOT%\deps\_install -DZLIB_LIBRARY=%ROOT%\deps\_install\lib\zlib.lib -DZLIB_INCLUDE_DIR=%ROOT%\deps\_install\include
echo PNG_CFG=%ERRORLEVEL%
"%CM%" --build %ROOT%\deps\libpng\build-armuwp --config Release --target png_static
echo PNG_BUILD=%ERRORLEVEL%

echo ===== LIBJPEG-TURBO =====
"%CM%" -S %ROOT%\deps\libjpeg-turbo -B %ROOT%\deps\libjpeg-turbo\build-armuwp %COMMON% ^
  -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF
echo JPEG_CFG=%ERRORLEVEL%
"%CM%" --build %ROOT%\deps\libjpeg-turbo\build-armuwp --config Release --target jpeg-static
echo JPEG_BUILD=%ERRORLEVEL%
echo ALL_DONE
