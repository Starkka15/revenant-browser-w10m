@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
python -c "import pkgconf; print('REAL=' + str(pkgconf.get_executable()))" 2>&1
python -c "import pkgconf, os; d=os.path.dirname(pkgconf.__file__); print('PKGDIR=' + d)" 2>&1
