@echo off
where py >nul 2>nul
if errorlevel 1 (
    python "%~dp0cell.py" %*
) else (
    py -3 "%~dp0cell.py" %*
)
exit /b %errorlevel%
