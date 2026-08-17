@echo off
setlocal EnableExtensions

echo.
echo ========================================
echo        ClassicStart WebView2 Build
echo ========================================
echo.

set "ROOT=%~dp0"
set "SDK=%ROOT%webview2\build\native"
set "INC=%SDK%\include"
set "LIB=%SDK%\x64"

if not exist "%INC%\WebView2.h" (
    echo ERROR: WebView2 SDK not found.
    echo.
    echo Run the WebView2 download command first.
    echo.
    pause
    exit /b 1
)

echo WebView2 SDK:
echo %SDK%
echo.

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
echo Building ClassicStart...
echo.

cl.exe ^
 /nologo ^
 /std:c++17 ^
 /O2 ^
 /EHsc ^
 /DUNICODE ^
 /D_UNICODE ^
 /I"%INC%" ^
 "%ROOT%classicstart.cpp" ^
 /link ^
 /SUBSYSTEM:WINDOWS ^
 /OUT:"%ROOT%classicstart.exe" ^
 /LIBPATH:"%LIB%" ^
 WebView2LoaderStatic.lib ^
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
echo %ROOT%classicstart.exe
echo.

pause