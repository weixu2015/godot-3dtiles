@echo off
REM Build and install the release configuration into demo/addons.
call "%~dp0msvc_env.bat"
if errorlevel 1 exit /b 1

echo Building (windows-release)...
cmake --build --preset windows-release --parallel
if errorlevel 1 (
    echo Failed to build the project.
    exit /b 1
)

echo Installing...
cmake --install build\windows-release
if errorlevel 1 (
    echo Failed to install the project.
    exit /b 1
)

echo Installation successful. Library and .gdextension file are in demo\addons.
