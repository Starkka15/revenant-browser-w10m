@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
set CM=%CMAKE%
"%CM%" --build %ROOT%\boringssl-new-build\build --config Release -- /p:CL_MPCount=6 /nologo > %ROOT%\bsslnew-build.log 2>&1
echo BSSLNEW_BUILD=%ERRORLEVEL%>> %ROOT%\bsslnew-build.log
