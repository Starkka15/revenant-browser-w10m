@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
set CM=%CMAKE%
"%CM%" --build %ROOT%\deps\curl\build-armuwp-bssl2 --config Release -- /p:CL_MPCount=6 /nologo > %ROOT%\curl-bssl-build.log 2>&1
echo CURL_BUILD=%ERRORLEVEL%>> %ROOT%\curl-bssl-build.log
