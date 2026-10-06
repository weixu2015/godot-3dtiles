@echo off
REM Build and install the editor-target + Release configuration into demo/addons.
REM
REM The result is the one to use for day-to-day work in the editor: the editor viewport, the
REM automatic framing and the editor clip planes all compile in (GODOTCPP_TARGET=editor), and
REM the code is optimised (/O2) so frame timings mean something. scripts\release_build_install.bat
REM remains the one to use for a shipping-runtime measurement.
REM
REM Installing this rewrites demo\addons\godot-3dtiles.gdextension to match (a generated file
REM that IS tracked in git, so check it before committing), and replaces whatever library is
REM there now - close Godot first, or the install fails with "Permission denied" and leaves a
REM "~" temp file behind while the demo keeps loading the previous library.
call "%~dp0msvc_env.bat"
if errorlevel 1 exit /b 1

echo Building (windows-editor-release)...
cmake --build --preset windows-editor-release --parallel
if errorlevel 1 (
    echo Failed to build the project.
    exit /b 1
)

echo Installing...
cmake --install build\windows-editor-release
if errorlevel 1 (
    echo Failed to install the project.
    exit /b 1
)

echo Installation successful. Library and .gdextension file are in demo\addons.
