@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
call "%VCVARSALL%" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul 2>&1
set CM=%CMAKE%
"%CM%" --build %ROOT%\deps\curl\build-armuwp-bssl3 --config Release --target libcurl_static
if %ERRORLEVEL% NEQ 0 (echo CURL_BUILD_FAIL & exit /b 1)
copy /Y %ROOT%\deps\curl\build-armuwp-bssl3\lib\Release\libcurl.lib %ROOT%\deps\_install\lib\libcurl.lib >nul && echo COPIED_TO_INSTALL
echo CURL_EXIT=%ERRORLEVEL%
