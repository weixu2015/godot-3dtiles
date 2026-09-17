# SPDX-License-Identifier: Unlicense

# tiles3d_core: the 3D Tiles scheduling kernel.
#
# Hard rule: nothing in this directory (or below it) may include a Godot header
# (godot_cpp/*) or a Cesium header (Cesium*). It must stay a plain C++20 library so
# it can be unit tested without an engine and so no engine API is ever touched from a
# worker thread. Enforced by the CI gate described in docs/REFACTOR_PLAN.md 6.4.

add_library( tiles3d_core STATIC )

file( GLOB_RECURSE tiles3d_core_src CONFIGURE_DEPENDS
        "*.h"
        "*.cpp" )

if ( NOT tiles3d_core_src )
    message( FATAL_ERROR "[${PROJECT_NAME}] src/core/ contains no sources." )
endif()

target_sources( tiles3d_core
    PRIVATE
        ${tiles3d_core_src}
)

target_compile_features( tiles3d_core PUBLIC cxx_std_20 )

target_include_directories( tiles3d_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries( tiles3d_core
    PUBLIC
        tiles3d_third_party
)

# TODO(Phase 6): unify with cmake/CompilerWarnings.cmake. That file targets
# ${PROJECT_NAME} only, so the core library carries an equivalent flag set for now.
if ( MSVC )
    target_compile_options( tiles3d_core PRIVATE
        /W4
        /utf-8
    )
else()
    target_compile_options( tiles3d_core PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wno-unused-parameter
    )
endif()
