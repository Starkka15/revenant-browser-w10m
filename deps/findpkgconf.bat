@echo off
python -c "import pkgconf; print('REAL=' + str(pkgconf.get_executable()))" 2>&1
python -c "import pkgconf, os; d=os.path.dirname(pkgconf.__file__); print('PKGDIR=' + d)" 2>&1
