@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
REM Build ICU common(icuuc)+i18n(icuin) as STATIC libs for ARM32 UWP, with the data linked in
REM statically. U_STATIC_IMPLEMENTATION (via CL env) makes ICU's U_EXPORT empty (no dllexport),
REM so consumers link statically (no icu*.dll). ConfigurationType overridden to StaticLibrary.
REM MSBuild.exe and lib.exe must be on PATH: run from a "Developer Command Prompt for VS 2022"
REM or  call "%VCVARSALL%" x64_arm  first. ICU here builds with the v141 (VS2017) toolset - install it.
set MSB=msbuild.exe
set LIBEXE=lib.exe
set SRC=%ROOT%\icu\source
set OUT=%ROOT%\icu\libARMuwp-static
set CL=/DU_STATIC_IMPLEMENTATION
set OPTS=/p:Configuration=Release /p:Platform=ARM /p:PlatformToolset=v141 ^
 /p:WindowsTargetPlatformVersion=10.0.16299.0 /p:WindowsTargetPlatformMinVersion=10.0.15063.0 ^
 /p:ConfigurationType=StaticLibrary /p:OutDir=%OUT%\ /v:minimal /nologo

if not exist "%OUT%" mkdir "%OUT%"

echo === ICU common (icuuc) STATIC ARM UWP ===
"%MSB%" %SRC%\common\common_uwp.vcxproj %OPTS%
echo COMMON_EXIT=%ERRORLEVEL%

echo === ICU i18n (icuin) STATIC ARM UWP ===
"%MSB%" %SRC%\i18n\i18n_uwp.vcxproj %OPTS%
echo I18N_EXIT=%ERRORLEVEL%

echo === ICU data static lib (archive the prebuilt data object) ===
"%LIBEXE%" /MACHINE:ARM /OUT:%OUT%\icudt.lib %SRC%\data\out\tmp\icudt77l_dat.obj
echo DATA_EXIT=%ERRORLEVEL%

echo === results ===
dir "%OUT%\*.lib"
