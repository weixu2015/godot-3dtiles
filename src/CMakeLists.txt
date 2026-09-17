# SPDX-License-Identifier: Unlicense

# The engine-agnostic scheduler kernel. Built as its own static library so it can be
# unit tested without Godot, and so no engine API is ever reachable from a worker thread.
add_subdirectory( core )

# Everything else under src/ is the Godot integration layer. core/ is excluded here
# because those sources are compiled into the tiles3d_core target added above.
file( GLOB_RECURSE godot_layer_src CONFIGURE_DEPENDS
        "*.h"
        "*.cpp" )
list( FILTER godot_layer_src EXCLUDE REGEX "/core/" )

target_sources( ${PROJECT_NAME}
    PRIVATE
        ${godot_layer_src}
)

# Includes are written relative to src/ (e.g. "Godot3DTiles.h", "core/math/Mat4.h"), so
# the src directory itself is the include root. The previous "src" value resolved to
# src/src and never matched anything.
target_include_directories( ${PROJECT_NAME}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries( ${PROJECT_NAME}
    PRIVATE
        godot-cpp
        tiles3d_core
)
