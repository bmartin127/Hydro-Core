@echo off
REM HydroCore build script (Windows x64)
REM
REM Environment variables you can override:
REM   VCVARS_BAT    - Path to vcvarsall.bat (default: Visual Studio 2022 Community)
REM   VCVARS_VER    - MSVC toolset version to pin (default: 14.44)
REM   RUSTUP_HOME   - cargo/rustup root (default: %USERPROFILE%\.rustup)
REM   CARGO_HOME    - cargo root           (default: %USERPROFILE%\.cargo)
REM   NINJA_DIR     - Directory containing ninja.exe (optional; auto-detected)

setlocal

REM Resolve script directory so this works from any cwd
set "SCRIPT_DIR=%~dp0"
cd /D "%SCRIPT_DIR%"

REM MSVC environment
if "%VCVARS_BAT%"=="" (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" (
        set "VCVARS_BAT=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" (
        set "VCVARS_BAT=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" (
        set "VCVARS_BAT=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" (
        set "VCVARS_BAT=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    )
)

if "%VCVARS_BAT%"=="" (
    echo ERROR: Could not find vcvarsall.bat. Install Visual Studio 2022 or set VCVARS_BAT.
    exit /b 1
)

if "%VCVARS_VER%"=="" set "VCVARS_VER=14.44"

call "%VCVARS_BAT%" x64 -vcvars_ver=%VCVARS_VER%
if errorlevel 1 (
    echo ERROR: Failed to initialize MSVC environment.
    exit /b 1
)

REM Rust / Cargo
if "%RUSTUP_HOME%"=="" set "RUSTUP_HOME=%USERPROFILE%\.rustup"
if "%CARGO_HOME%"=="" set "CARGO_HOME=%USERPROFILE%\.cargo"

REM Find rustc and cargo - rely on PATH first, fall back to env vars
where rustc >nul 2>&1
if errorlevel 1 (
    if exist "%RUSTUP_HOME%\toolchains\stable-x86_64-pc-windows-msvc\bin\rustc.exe" (
        set "PATH=%RUSTUP_HOME%\toolchains\stable-x86_64-pc-windows-msvc\bin;%PATH%"
    ) else (
        echo ERROR: rustc not found. Install rustup or add rustc to PATH.
        exit /b 1
    )
)
where cargo >nul 2>&1
if errorlevel 1 (
    if exist "%CARGO_HOME%\bin\cargo.exe" (
        set "PATH=%CARGO_HOME%\bin;%PATH%"
    ) else (
        echo ERROR: cargo not found. Install rustup or add cargo to PATH.
        exit /b 1
    )
)

REM Ninja (usually installed with Visual Studio's CMake component)
where ninja >nul 2>&1
if errorlevel 1 (
    if not "%NINJA_DIR%"=="" (
        set "PATH=%NINJA_DIR%;%PATH%"
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja" (
        set "PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
    ) else (
        echo ERROR: ninja not found. Install the CMake component with Visual Studio, or set NINJA_DIR.
        exit /b 1
    )
)

echo === Configuring ===
cmake -B build -G Ninja ^
    -DCMAKE_BUILD_TYPE=Game__Shipping__Win64 ^
    -DUE4SS_VERSION_CHECK=OFF ^
    -Wno-dev
if errorlevel 1 (
    echo === Configure FAILED ===
    exit /b 1
)

echo === Building HydroCore ===
cmake --build build --target HydroCore
if errorlevel 1 (
    echo === Build FAILED ===
    exit /b 1
)

echo === SUCCESS ===
dir /s /b build\*.dll 2>nul

endlocal
