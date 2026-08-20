@echo off
setlocal EnableExtensions

echo.
echo ========================================
echo           ClassicShell Tests
echo ========================================
echo.

set "ROOT=%~dp0"

if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    echo ERROR: vswhere.exe not found.
    exit /b 2
)

for /f "usebackq delims=" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%V"

if not defined VS (
    echo ERROR: MSVC Build Tools not found.
    exit /b 3
)

call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64

if errorlevel 1 (
    echo ERROR: Could not initialize MSVC.
    exit /b 4
)

echo Building tests...
echo.

cl.exe ^
 /nologo ^
 /std:c++17 ^
 /EHsc ^
 /DUNICODE ^
 /D_UNICODE ^
 "%ROOT%test_main.cpp" ^
 "%ROOT%test_starthook.cpp" ^
 /link ^
 /SUBSYSTEM:CONSOLE ^
 /OUT:"%ROOT%tests.exe" ^
 user32.lib

if errorlevel 1 (
    echo.
    echo ========================================
    echo           TEST BUILD FAILED
    echo ========================================
    exit /b 5
)

echo.
"%ROOT%tests.exe"
set "RESULT=%ERRORLEVEL%"

echo.
if "%RESULT%"=="0" (
    echo ========================================
    echo              ALL TESTS PASSED
    echo ========================================
) else (
    echo ========================================
    echo              TESTS FAILED
    echo ========================================
)

exit /b %RESULT%
