@echo off
REM Configure the editor build (GODOTCPP_TARGET=editor, Debug).
call "%~dp0msvc_env.bat"
if errorlevel 1 exit /b 1

cmake --preset windows-editor
if errorlevel 1 (
    echo Configuration failed.
    exit /b 1
)

echo Configuration complete. Build with: cmake --build --preset windows-editor
