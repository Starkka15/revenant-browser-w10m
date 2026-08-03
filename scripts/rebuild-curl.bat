@echo off
call "F:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul 2>&1
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
"%CM%" --build Z:\w10m-webengine\deps\curl\build-armuwp-bssl3 --config Release --target libcurl_static
if %ERRORLEVEL% NEQ 0 (echo CURL_BUILD_FAIL & exit /b 1)
copy /Y Z:\w10m-webengine\deps\curl\build-armuwp-bssl3\lib\Release\libcurl.lib Z:\w10m-webengine\deps\_install\lib\libcurl.lib >nul && echo COPIED_TO_INSTALL
echo CURL_EXIT=%ERRORLEVEL%
