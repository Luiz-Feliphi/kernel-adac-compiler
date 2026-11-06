@echo off
rem Builds the adaC compiler on Windows with MinGW-w64 (gcc).
rem If gcc is not in the PATH, it is looked up in GCC_DIR (edit the line below if yours is elsewhere).
rem The PATH change only lasts while this script runs: nothing in Windows is modified.
setlocal
set "GCC_DIR=C:\gccgnu\bin"

where gcc >nul 2>nul
if errorlevel 1 (
    if exist "%GCC_DIR%\gcc.exe" (
        set "PATH=%GCC_DIR%;%PATH%"
    ) else (
        echo gcc was not found in the PATH nor in "%GCC_DIR%".
        echo Edit GCC_DIR at the top of build.bat to the folder that contains gcc.exe.
        exit /b 1
    )
)

cd /d "%~dp0"

gcc -O1 -o tools\embed.exe tools\embed.c || exit /b 1
tools\embed.exe ADAC_RT_CORE runtime\core.adac ADAC_RT_GC runtime\gc.adac ADAC_RT_MANUAL runtime\manual.adac > src\prelude_data.c || exit /b 1
gcc -O2 -std=gnu11 -o adac.exe src\util.c src\lexer.c src\types.c src\parser.c src\sema.c src\x64.c src\pe.c src\codegen.c src\main.c src\prelude_data.c || exit /b 1

echo.
echo adac.exe built. Usage:  adac.exe examples\hello.adac -o hello.exe
