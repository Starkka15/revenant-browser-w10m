@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
set CM=%CMAKE%
rem First build. If CMakeLists changed since last configure, msbuild's ZERO_CHECK
rem auto-reconfigures here and regenerates the appx manifest WITHOUT our network
rem capabilities (bypassing configure-wincairo.bat's patch).
"%CM%" --build %ROOT%\build-wincairo --config Release --target WebCoreRenderShell
if %ERRORLEVEL% NEQ 0 goto done
rem Re-add the network capabilities, then rebuild — the second pass does NOT reconfigure
rem (CMake is now up to date) so the patched manifest survives and the appx is repackaged
rem with internetClient. Repackage-only, so it's fast.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0patch-manifest.ps1"
"%CM%" --build %ROOT%\build-wincairo --config Release --target WebCoreRenderShell
if %ERRORLEVEL% NEQ 0 goto done
copy /Y %ROOT%\build-wincairo\Source\WebCore\AppPackages\WebCoreRenderShell\WebCoreRenderShell_1.0.0.0_ARM_Test\WebCoreRenderShell_1.0.0.0_ARM.appx %ROOT%\WebCoreRenderShell_ARM.appx > nul && echo staged
:done
echo SHELL_EXIT=%ERRORLEVEL%
