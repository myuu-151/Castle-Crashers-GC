@echo off
rem CCGC Builder: builds the disc from your Steam copy of Castle Crashers.
cd /d "%~dp0"
where pyw >nul 2>nul && (start "" pyw -3 tools\builder.py & exit /b)
where pythonw >nul 2>nul && (start "" pythonw tools\builder.py & exit /b)
echo Python 3 is needed: https://www.python.org/downloads/
pause
