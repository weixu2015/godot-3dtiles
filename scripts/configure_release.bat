@echo off
REM Configure the release build (GODOTCPP_TARGET=template_release, Release).
call "%~dp0msvc_env.bat"
if errorlevel 1 exit /b 1

cmake --preset windows-release
if errorlevel 1 (
    echo Configuration failed.
    exit /b 1
)

echo Configuration complete. Build with: cmake --build --preset windows-release
