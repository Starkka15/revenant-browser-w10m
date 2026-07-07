@echo off
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
set INST=Z:\w10m-webengine\deps\_install
"%CM%" -S Z:\w10m-webengine\deps\woff2 -B Z:\w10m-webengine\deps\woff2\build-armuwp ^
  -G "Visual Studio 17 2022" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=Z:\w10m-webengine\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DCMAKE_PREFIX_PATH=%INST% ^
  -DBROTLIDEC_INCLUDE_DIRS=%INST%\include -DBROTLIDEC_LIBRARIES=%INST%\lib\brotlidec.lib ^
  -DBROTLIENC_INCLUDE_DIRS=%INST%\include -DBROTLIENC_LIBRARIES=%INST%\lib\brotlienc.lib ^
  -DBROTLIDEC_FOUND=ON -DBROTLIENC_FOUND=ON
echo WOFF2_CFG=%ERRORLEVEL%
"%CM%" --build Z:\w10m-webengine\deps\woff2\build-armuwp --config Release --target woff2dec --target woff2common
echo WOFF2_BUILD=%ERRORLEVEL%
