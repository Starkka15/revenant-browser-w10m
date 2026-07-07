@echo off
set PATH=Z:\w10m-webengine\deps\tools\bin;%PATH%
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
"%CM%" --build Z:\w10m-webengine\build-wincairo --config Release --target WebCore
echo WEBCORE_EXIT=%ERRORLEVEL%
