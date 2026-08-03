@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
set PATH=%ROOT%\deps\tools\bin;%PATH%
set CM=%CMAKE%
"%CM%" --build %ROOT%\build-wincairo --config Release --target WebCore
echo WEBCORE_EXIT=%ERRORLEVEL%
