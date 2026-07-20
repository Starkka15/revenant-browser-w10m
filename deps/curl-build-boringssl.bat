@echo off
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
"%CM%" --build Z:\w10m-webengine\deps\curl\build-armuwp-bssl2 --config Release -- /p:CL_MPCount=6 /nologo > Z:\w10m-webengine\curl-bssl-build.log 2>&1
echo CURL_BUILD=%ERRORLEVEL%>> Z:\w10m-webengine\curl-bssl-build.log
