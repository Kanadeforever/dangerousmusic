@echo off
setlocal
cd /d "%~dp0"

REM --------------------------------------------------------------
REM  build_msvc.bat -- Build dsound.dll + LocalMusic.asi with MSVC
REM  Usage:  build_msvc.bat
REM --------------------------------------------------------------

set "VSROOT="

REM --- 1. Find VS via vswhere or known paths ---
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSW%" set "VSW=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSW%" (
    for /f "delims=" %%i in ('"%VSW%" -latest -property installationPath 2^>nul') do set "VSROOT=%%i"
)
if not defined VSROOT if exist "C:\Program Files\Microsoft Visual Studio\18\Community" set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VSROOT if exist "C:\Program Files\Microsoft Visual Studio\17\Community" set "VSROOT=C:\Program Files\Microsoft Visual Studio\17\Community"
if not defined VSROOT (
    echo [ERROR] Visual Studio not found. Install VS or use a VS x64 Native Tools Command Prompt.
    exit /b 1
)

REM --- 2. Set up MSVC x64 environment ---
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"

REM --- 3. Find cmake (bundled with VS) ---
where cmake >nul 2>nul
if errorlevel 1 (
    set "CMAKE_DIR=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    if exist "%CMAKE_DIR%\cmake.exe" (
        set "PATH=%CMAKE_DIR%;%PATH%"
    ) else (
        echo [ERROR] CMake not found.
        exit /b 1
    )
)

REM --- 4. Configure + build ---
if not exist build mkdir build
cmake -S . -B build -A x64
if errorlevel 1 exit /b 1
cmake --build build --config Release
if errorlevel 1 exit /b 1

REM --- 5. Verify outputs ---
if not exist "build\Release\dsound.dll" (
    echo [ERROR] build\Release\dsound.dll not found.
    exit /b 1
)
if not exist "build\Release\LocalMusic.asi" (
    echo [ERROR] build\Release\LocalMusic.asi not found.
    exit /b 1
)

REM Binary compare to confirm they are identical.
fc /b "build\Release\dsound.dll" "build\Release\LocalMusic.asi" >nul
if errorlevel 1 (
    echo [ERROR] dsound.dll and LocalMusic.asi differ.
    exit /b 1
)

for /f "skip=1 tokens=*" %%i in ('certutil -hashfile "build\Release\dsound.dll" SHA256') do set "HASH=%%i" & goto :done_hash
:done_hash
echo.
echo [OK] Built:
echo      build\Release\dsound.dll
echo      build\Release\LocalMusic.asi
echo      SHA-256: %HASH%
echo.
echo Both files are byte-identical (same binary, two names).
endlocal
