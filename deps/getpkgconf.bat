@echo off
pip install --quiet pkgconf 2>&1 | findstr /C:"Successfully" /C:"already"
where pkgconf 2>nul
python -c "import pkgconf, os; print('PKGCONF_EXE=' + pkgconf.get_executable())" 2>nul
