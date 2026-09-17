# SPDX-License-Identifier: Unlicense
#
# Build dependencies. The root CMakeLists.txt adds this directory *before* src/ so that
# both tiles3d_core and the extension target can link the libraries defined here.

# ---------------------------------------------------------------------------
# godot-cpp
# ---------------------------------------------------------------------------
# From here: https://github.com/godotengine/godot-cpp
#
# Target engine: Godot 4.7.x, using godot-cpp v10.
#
# v10 is versioned independently from the engine and *requires* an explicit API
# version (see GODOTCPP_API_VERSION below and the CMake presets). Without it godot-cpp
# aborts configuration with: 'GODOTCPP_API_VERSION' must be provided.
#
# To bind against the exact engine build in use rather than the bundled API JSON,
# dump it from the engine and point godot-cpp at the file:
#
#     Godot_v4.7.2-stable_win64_console.exe --headless --dump-extension-api
#     cmake --preset windows-release -DGODOTCPP_CUSTOM_API_FILE=<abs>/extension_api.json
#
# GODOTCPP_CUSTOM_API_FILE takes precedence over GODOTCPP_API_VERSION and
# GODOTCPP_GDEXTENSION_DIR.
if ( NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/godot-cpp/Makefile" )
    message(
        FATAL_ERROR
            "[${PROJECT_NAME}] The godot-cpp submodule was not downloaded. Run: git submodule update --init extern/godot-cpp"
    )
endif()

# godot-cpp v10 renamed every cache option it reads (see cmake/godotcpp.cmake in the
# godot-cpp repository). The names this project used before the refactor
# (GODOT_ENABLE_HOT_RELOAD, GODOT_CPP_SYSTEM_HEADERS) no longer exist and silently did
# nothing, so hot reload was only ever switching on because the build set the
# HOT_RELOAD_ENABLED define by hand. That define is now gone: the option below is the
# single source of truth.
#
# Plain `set(... CACHE ...)` (no FORCE) so a command line -D still wins.
set( GODOTCPP_USE_HOT_RELOAD OFF CACHE BOOL "Enable the extra accounting required to support hot reload." )
set( GODOTCPP_SYSTEM_HEADERS ON CACHE BOOL "Expose godot-cpp headers as SYSTEM." )
set( GODOTCPP_ENABLE_TESTING OFF CACHE BOOL "Enable the godot-cpp integration testing targets." )
set( GODOTCPP_WARNING_AS_ERROR OFF CACHE BOOL "Treat godot-cpp warnings as errors." )

add_subdirectory( godot-cpp EXCLUDE_FROM_ALL )

# ---------------------------------------------------------------------------
# Header-only third-party dependencies
# ---------------------------------------------------------------------------
# glm (double precision math), nlohmann/json (worker-thread JSON parsing),
# doctest (unit tests). See extern/third_party/CMakeLists.txt.
add_subdirectory( third_party )
