@echo off
REM Rebuild jsc.exe after the setEntryAddressCommon ARMv7 dispatch-table fix (SPEC B4).
REM Only LowLevelInterpreter.asm changed -> offlineasm regenerates LowLevelInterpreterWin.asm
REM -> armasm reassembles the LLInt obj -> relink JavaScriptCore.lib + jsc.exe.
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
"%CM%" --build Z:\w10m-webengine\build-jsc2 --target jsc --config Release
echo BUILD_EXIT=%ERRORLEVEL%
