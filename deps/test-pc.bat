@echo off
set PKG_CONFIG_LIBDIR=Z:\w10m-webengine\deps\_install\lib\pkgconfig
"C:\Users\Starkka15\AppData\Local\Programs\Python\Python314\Lib\site-packages\pkgconf\.bin\pkgconf.exe" --cflags icu-uc
echo ---LIBS---
"C:\Users\Starkka15\AppData\Local\Programs\Python\Python314\Lib\site-packages\pkgconf\.bin\pkgconf.exe" --libs icu-uc
