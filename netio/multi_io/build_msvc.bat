@echo off
rem ===========================================================================
rem  One-click configure + build with MSVC (Windows).
rem  NOTE: keep this file pure ASCII -- cmd.exe parses .bat using the local
rem  code page (GBK here), so UTF-8 Chinese comments would corrupt the syntax.
rem  If your Visual Studio lives elsewhere, change VSROOT below.
rem ===========================================================================
setlocal

set "VSROOT=D:\C++"

if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [ERROR] not found: "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
    echo         please edit VSROOT in this script
    exit /b 1
)

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed
    exit /b 1
)

cmake -S "%~dp0." -B "%~dp0build" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1

cmake --build "%~dp0build" --config Release
if errorlevel 1 exit /b 1

echo.
echo [DONE] executables are in %~dp0build\Release\
endlocal
