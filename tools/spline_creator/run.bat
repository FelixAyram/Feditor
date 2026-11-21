@echo off
setlocal
cd /d "%~dp0"
py -3 -m pip install pygame moderngl Pillow -q
py -3 app.py %*
exit /b %ERRORLEVEL%
