@echo off
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
"%CM%" --build Z:\w10m-webengine\boringssl-new-build\build --config Release -- /p:CL_MPCount=6 /nologo > Z:\w10m-webengine\bsslnew-build.log 2>&1
echo BSSLNEW_BUILD=%ERRORLEVEL%>> Z:\w10m-webengine\bsslnew-build.log
