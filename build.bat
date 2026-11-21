@echo off
setlocal

set "PATH=C:\Users\Pongo\AppData\Local\Microsoft\WinGet\Links;%PATH%"
cd /d "%~dp0"

echo [1/2] Compiling Windows resource...
zig rc assets\resource.rc assets\resource.res

echo [2/2] Compiling WaveDar (Native C99 + miniaudio)...
zig cc -O3 src\main.c src\metadata.c assets\resource.res -o wavedar.exe -luser32 -lole32 -ladvapi32 -lshell32

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo  Build SUCCESSFUL! Standalone binary: wavedar.exe
    echo ========================================================
) else (
    echo.
    echo [ERROR] Build failed! Check compiler output.
)

endlocal
