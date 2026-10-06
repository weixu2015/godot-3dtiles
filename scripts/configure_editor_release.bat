@echo off
REM Configure the editor-target build with Release optimization (windows-editor-release).
REM
REM Why this exists, in one line: the editor viewport is not a casualty of optimisation, it is a
REM casualty of which godot-cpp TARGET the build selects.
REM
REM godot-cpp has two independent axes:
REM   target      editor | template_debug | template_release - which engine API surface the
REM               extension is built against. Export templates (what a shipped game runs on) do
REM               NOT register editor-only singletons, so anything reaching for EditorInterface
REM               has to stay out of the template build. That is what TILES3D_EDITOR_TARGET
REM               gates, and it is derived from GODOTCPP_TARGET in the top level CMakeLists.
REM   build type  Debug | Release - the /Od vs /O2 axis, which also decides the "d" suffix
REM               (DEBUG_POSTFIX) and which .gdextension template gets generated.
REM
REM The shipped presets happened to pair (editor, Debug) and (template_release, Release), which
REM reads as "optimised means no editor viewport". It does not: (editor, Release) is the
REM combination that keeps the editor code AND optimises it. Both the library name and the
REM generated .gdextension follow the BUILD TYPE, so the pair stays consistent on its own.
REM
REM Note that RelWithDebInfo would be rejected: templates/CMakeLists.txt requires
REM CMAKE_BUILD_TYPE to be exactly Debug or Release (ALLOWED_BUILDS).
call "%~dp0msvc_env.bat"
if errorlevel 1 exit /b 1

REM Same third-party reuse as configure_release.bat - FetchContent keeps a source checkout per
REM build directory, so a third configuration would otherwise try to clone glm/json/doctest
REM again, which fails outright without a working github connection.
set "DEPS=%~dp0..\build\windows-editor\_deps"
set "REUSE="
if exist "%DEPS%\glm-src" if exist "%DEPS%\nlohmann_json-src" if exist "%DEPS%\doctest-src" set "REUSE=1"

if defined REUSE (
    echo Reusing third-party sources from "%DEPS%" ...
    cmake --preset windows-editor-release ^
        -DFETCHCONTENT_SOURCE_DIR_GLM="%DEPS%\glm-src" ^
        -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="%DEPS%\nlohmann_json-src" ^
        -DFETCHCONTENT_SOURCE_DIR_DOCTEST="%DEPS%\doctest-src"
) else (
    echo Third-party sources not found under "%DEPS%" - letting FetchContent download them.
    cmake --preset windows-editor-release
)

if errorlevel 1 (
    echo Configuration failed.
    exit /b 1
)

echo Configuration complete. Build with: scripts\editor_release_build_install.bat
