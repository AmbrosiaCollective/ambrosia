@echo off
setlocal
where py >nul 2>nul
if not errorlevel 1 goto python_launcher
where python >nul 2>nul
if not errorlevel 1 goto python_command
echo Ambrosia needs Python 3.12 or newer on PATH. 1>&2
exit /b 1

:python_launcher
if "%~1"=="" (py -3 -I "%~dp0ambrosia.py" menu) else (py -3 -I "%~dp0ambrosia.py" %*)
exit /b %errorlevel%

:python_command
if "%~1"=="" (python -I "%~dp0ambrosia.py" menu) else (python -I "%~dp0ambrosia.py" %*)
exit /b %errorlevel%
