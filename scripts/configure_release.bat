@echo off
REM Configure the release build (GODOTCPP_TARGET=template_release, Release).
call "%~dp0msvc_env.bat"
if errorlevel 1 exit /b 1

REM FetchContent keeps a source checkout inside EACH build directory, so this second
REM configuration would download glm, nlohmann_json and doctest all over again even though the
REM editor build already has every one of them on disk. On a machine that cannot reach
REM github.com reliably that is not a wait, it is a hard failure:
REM
REM   error: RPC failed; curl 56 Recv failure: Connection was reset
REM   Failed to clone repository: 'https://github.com/g-truc/glm.git'
REM
REM So: reuse the editor build's copies when they are there, and download as before when they are
REM not - a fresh machine that has never configured anything still works unchanged. The override
REM is FETCHCONTENT_SOURCE_DIR_<UPPERCASED_NAME>, documented at the top of
REM extern/third_party/CMakeLists.txt. godot-cpp is not involved: it is a git submodule at
REM extern/godot-cpp and is never re-fetched.
REM
REM Reusing the sources does NOT make this configure quick. The bulk of it is godot-cpp's binding
REM generation (2135 files) and its own static library, and those are per build directory too.
set "DEPS=%~dp0..\build\windows-editor\_deps"
set "REUSE="
if exist "%DEPS%\glm-src" if exist "%DEPS%\nlohmann_json-src" if exist "%DEPS%\doctest-src" set "REUSE=1"

if defined REUSE (
    echo Reusing third-party sources from "%DEPS%" ...
    cmake --preset windows-release ^
        -DFETCHCONTENT_SOURCE_DIR_GLM="%DEPS%\glm-src" ^
        -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="%DEPS%\nlohmann_json-src" ^
        -DFETCHCONTENT_SOURCE_DIR_DOCTEST="%DEPS%\doctest-src"
) else (
    echo Third-party sources not found under "%DEPS%" - letting FetchContent download them.
    cmake --preset windows-release
)

if errorlevel 1 (
    echo Configuration failed.
    exit /b 1
)

echo Configuration complete. Build with: scripts\release_build_install.bat
