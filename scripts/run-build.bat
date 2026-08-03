@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
echo BUILD_STARTED_MARKER> %ROOT%\shell-buildBCACHE4.log
cmd /c "%~dp0build-shell.bat" >> %ROOT%\shell-buildBCACHE4.log 2>&1
echo WRAPPER_DONE>> %ROOT%\shell-buildBCACHE4.log
