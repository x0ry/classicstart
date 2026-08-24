@echo off
setlocal EnableExtensions

echo.
echo ========================================
echo             ClassicShell Build
echo ========================================
echo.

set "ROOT=%~dp0"

if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    echo ERROR: vswhere.exe not found.
    pause
    exit /b 2
)

for /f "usebackq delims=" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%V"

if not defined VS (
    echo ERROR: MSVC Build Tools not found.
    pause
    exit /b 3
)

echo Visual Studio:
echo %VS%
echo.

call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64

if errorlevel 1 (
    echo ERROR: Could not initialize MSVC.
    pause
    exit /b 4
)

echo.
echo Building ClassicShell...
echo.

cl.exe ^
 /nologo ^
 /std:c++17 ^
 /O2 ^
 /EHsc ^
 /DUNICODE ^
 /D_UNICODE ^
 "%ROOT%classicshell.cpp" ^
 /link ^
 /SUBSYSTEM:WINDOWS ^
 /OUT:"%ROOT%classicshell.exe" ^
 user32.lib ^
 gdi32.lib ^
 shell32.lib ^
 ole32.lib ^
 advapi32.lib

if errorlevel 1 (
    echo.
    echo ========================================
    echo              BUILD FAILED
    echo ========================================
    echo.
    pause
    exit /b 5
)

echo.
echo ========================================
echo              BUILD SUCCESS
echo ========================================
echo.
echo %ROOT%classicshell.exe
echo.

pause
