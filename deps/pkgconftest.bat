@echo off
set PKGCONF=C:\Users\Starkka15\AppData\Local\Programs\Python\Python314\Scripts\pkgconf.exe
set PKG_CONFIG_PATH=Z:\w10m-webengine\deps\_install\lib\pkgconfig
echo --- pkgconf version ---
"%PKGCONF%" --version
echo --- list-all (does it see our pc dir?) ---
"%PKGCONF%" --list-all
echo --- zlib exists? ---
"%PKGCONF%" --exists zlib && echo ZLIB_FOUND || echo ZLIB_MISSING (errorlevel %ERRORLEVEL%)
"%PKGCONF%" --cflags --libs zlib
echo --- with forward-slash PKG_CONFIG_PATH ---
set PKG_CONFIG_PATH=Z:/w10m-webengine/deps/_install/lib/pkgconfig
"%PKGCONF%" --exists zlib && echo ZLIB_FOUND_FWD || echo ZLIB_MISSING_FWD
