@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
pip install --quiet pkgconf 2>&1 | findstr /C:"Successfully" /C:"already"
where pkgconf 2>nul
python -c "import pkgconf, os; print('PKGCONF_EXE=' + pkgconf.get_executable())" 2>nul
