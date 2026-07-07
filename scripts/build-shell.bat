@echo off
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
rem First build. If CMakeLists changed since last configure, msbuild's ZERO_CHECK
rem auto-reconfigures here and regenerates the appx manifest WITHOUT our network
rem capabilities (bypassing configure-wincairo.bat's patch).
"%CM%" --build Z:\w10m-webengine\build-wincairo --config Release --target WebCoreRenderShell
if %ERRORLEVEL% NEQ 0 goto done
rem Re-add the network capabilities, then rebuild — the second pass does NOT reconfigure
rem (CMake is now up to date) so the patched manifest survives and the appx is repackaged
rem with internetClient. Repackage-only, so it's fast.
powershell -NoProfile -ExecutionPolicy Bypass -File Z:\w10m-webengine\patch-manifest.ps1
"%CM%" --build Z:\w10m-webengine\build-wincairo --config Release --target WebCoreRenderShell
if %ERRORLEVEL% NEQ 0 goto done
copy /Y Z:\w10m-webengine\build-wincairo\Source\WebCore\AppPackages\WebCoreRenderShell\WebCoreRenderShell_1.0.0.0_ARM_Test\WebCoreRenderShell_1.0.0.0_ARM.appx Z:\w10m-webengine\WebCoreRenderShell_ARM.appx > nul && echo staged
:done
echo SHELL_EXIT=%ERRORLEVEL%
