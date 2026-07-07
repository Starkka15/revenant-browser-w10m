@echo off
call "F:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64_arm -vcvars_ver=14.29 10.0.16299.0 >nul
cd /d Z:\w10m-webengine\deps\harfbuzz
rmdir /s /q build-armuwp 2>nul
rem WinCairo WebCore (ComplexTextControllerHarfBuzz) calls hb_directwrite_face_create
rem and hb_icu_script_to_script, so harfbuzz must be built WITH directwrite + icu.
rem --buildtype release => /MD + NDEBUG (no _CrtDbgReport/_invalid_parameter debug-CRT refs).
meson setup build-armuwp --cross-file Z:\w10m-webengine\deps\cross-armuwp.txt ^
  --buildtype release -Dcpp_std=c++17 ^
  -Dglib=disabled -Dgobject=disabled -Dcairo=disabled -Dchafa=disabled ^
  -Dicu=enabled -Dfreetype=disabled -Dgdi=disabled -Ddirectwrite=enabled ^
  -Dcoretext=disabled -Dtests=disabled -Ddocs=disabled -Dintrospection=disabled 2>&1
echo HB_CFG=%ERRORLEVEL%
meson compile -C build-armuwp 2>&1
echo HB_BUILD=%ERRORLEVEL%
copy /y build-armuwp\src\libharfbuzz.a Z:\w10m-webengine\deps\_install\lib\harfbuzz.lib 2>&1
copy /y build-armuwp\src\libharfbuzz-subset.a Z:\w10m-webengine\deps\_install\lib\harfbuzz-subset.lib 2>&1
rem hb-icu compiles into a SEPARATE lib; WebCore's ComplexTextControllerHarfBuzz needs it.
copy /y build-armuwp\src\libharfbuzz-icu.a Z:\w10m-webengine\deps\_install\lib\harfbuzz-icu.lib 2>&1
echo HB_COPY=%ERRORLEVEL%
