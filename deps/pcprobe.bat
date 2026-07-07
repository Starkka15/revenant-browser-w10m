@echo off
where pkg-config 2>nul && pkg-config --version || echo NO_PKGCONFIG
where pkgconf 2>nul || echo NO_PKGCONF
