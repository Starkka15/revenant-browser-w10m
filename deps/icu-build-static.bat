@echo off
REM Build ICU common(icuuc)+i18n(icuin) as STATIC libs for ARM32 UWP, with the data linked in
REM statically. U_STATIC_IMPLEMENTATION (via CL env) makes ICU's U_EXPORT empty (no dllexport),
REM so consumers link statically (no icu*.dll). ConfigurationType overridden to StaticLibrary.
set MSB=F:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe
set LIBEXE=F:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.16.27023\bin\HostX64\x64\lib.exe
set SRC=Z:\w10m-webengine\icu\source
set OUT=Z:\w10m-webengine\icu\libARMuwp-static
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
