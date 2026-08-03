@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
REM Build curl (BoringSSL + HTTP/2) and stage it into _install, keeping a revertible backup of the
REM current HTTP/1.1-only lib. See curl-cfg-boringssl-h2.bat for why HTTP/2 went missing.
set CM=%CMAKE%
set INST=%ROOT%\deps\_install
set LOG=%ROOT%\curl-h2-build.log

"%CM%" --build %ROOT%\deps\curl\build-armuwp-bssl3 --config Release --target libcurl_static -- /p:CL_MPCount=6 /nologo > %LOG% 2>&1
echo CURL_BUILD=%ERRORLEVEL%>> %LOG%

REM Back up the outgoing lib BEFORE overwriting, so a bad HTTP/2 build can be reverted by copying
REM libcurl-h1-backup.lib back over libcurl.lib.
if not exist %INST%\lib\libcurl-h1-backup.lib copy /Y %INST%\lib\libcurl.lib %INST%\lib\libcurl-h1-backup.lib >> %LOG% 2>&1

copy /Y %ROOT%\deps\curl\build-armuwp-bssl3\lib\Release\libcurl.lib %INST%\lib\libcurl.lib >> %LOG% 2>&1
echo CURL_STAGE=%ERRORLEVEL%>> %LOG%
type %LOG%
