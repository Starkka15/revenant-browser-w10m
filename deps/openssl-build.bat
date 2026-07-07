@echo off
call "F:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64_arm -vcvars_ver=14.29 10.0.16299.0
echo VCVARS_EXIT=%ERRORLEVEL%
cd /d Z:\w10m-webengine\deps\openssl
perl Configure VC-WIN32-ARM-UWP no-asm no-shared no-tests ^
  --prefix=Z:\w10m-webengine\deps\_install-openssl
echo CONFIGURE_EXIT=%ERRORLEVEL%
nmake build_libs
echo NMAKE_EXIT=%ERRORLEVEL%
