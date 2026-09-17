@echo off
REM Build and install the editor configuration into demo/addons.
call "%~dp0msvc_env.bat"
if errorlevel 1 exit /b 1

echo Building (windows-editor)...
cmake --build --preset windows-editor --parallel
if errorlevel 1 (
    echo Failed to build the project.
    exit /b 1
)

echo Installing...
cmake --install build\windows-editor
if errorlevel 1 (
    echo Failed to install the project.
    exit /b 1
)

echo Installation successful. Library and .gdextension file are in demo\addons.
