@echo off
set PKG_CONFIG_PATH=Z:\w10m-webengine\deps\_install\lib\pkgconfig
echo --- pixman-1 ---
pkg-config --exists pixman-1 && echo FOUND pixman-1 || echo MISSING pixman-1
pkg-config --cflags --libs pixman-1
echo --- zlib ---
pkg-config --exists zlib && echo FOUND zlib || echo MISSING zlib
echo --- libpng ---
pkg-config --exists libpng && echo FOUND libpng || echo MISSING libpng
pkg-config --cflags --libs libpng
