@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
REM Rebuild jsc.exe after the setEntryAddressCommon ARMv7 dispatch-table fix (SPEC B4).
REM Only LowLevelInterpreter.asm changed -> offlineasm regenerates LowLevelInterpreterWin.asm
REM -> armasm reassembles the LLInt obj -> relink JavaScriptCore.lib + jsc.exe.
set CM=%CMAKE%
"%CM%" --build %ROOT%\build-jsc2 --target jsc --config Release
echo BUILD_EXIT=%ERRORLEVEL%
