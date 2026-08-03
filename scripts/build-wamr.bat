@echo off
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
"%CM%" -S Z:\w10m-webengine\WebKit-2.36.8\Source\ThirdParty\wamr-uwp -B Z:\w10m-webengine\build-wamr -G "Visual Studio 17 2022" -A ARM -T v142,host=x64 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 -DCMAKE_TOOLCHAIN_FILE=Z:\w10m-webengine\cmake\Toolchain-W10M-ARM32-UWP.cmake
echo CFG_EXIT=%ERRORLEVEL%
"%CM%" --build Z:\w10m-webengine\build-wamr --config Release --target wamr
echo WAMR_BUILD_EXIT=%ERRORLEVEL%
