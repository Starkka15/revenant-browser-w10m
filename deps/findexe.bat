@echo off
for /f "delims=" %%i in ('python -c "import pkgconf,os;print(os.path.dirname(pkgconf.__file__))"') do dir /s /b "%%i\*.exe"
