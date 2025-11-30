@echo off
set "SOURCE=cfg.dot"
set "OUTPUT=cfg.svg"

echo Processing %SOURCE%...

:: Check if dot command exists
where dot >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] The 'dot' command was not found.
    echo.
    echo Please ensure Graphviz is installed and the 'bin' folder 
    echo is added to your system PATH environment variable.
    echo.
    echo Alternatively, edit this script and replace 'dot' with the full path:
    echo e.g., "C:\Program Files\Graphviz\bin\dot.exe"
    echo.
    pause
    exit /b
)

:: Run the conversion
dot -Tsvg "%SOURCE%" -o "%OUTPUT%"

if %errorlevel% equ 0 (
    echo Success! Generated %OUTPUT%
) else (
    echo [ERROR] Failed to generate SVG. Check if the dot file is valid.
)

pause