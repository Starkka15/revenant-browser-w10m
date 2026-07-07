@echo off
call "F:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul
cd /d Z:\w10m-webengine\deps\openssl
nmake clean >nul 2>&1
nmake build_libs
echo NMAKE_EXIT=%ERRORLEVEL%
