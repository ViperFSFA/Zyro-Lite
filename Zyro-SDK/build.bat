@echo off
REM Convenience wrapper: build.bat MyApp.cpp
setlocal
set DIR=%~dp0
python "%DIR%tools\pack_zapp.py" %*
