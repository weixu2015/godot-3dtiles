# Header-only third-party dependencies for the 3D Tiles scheduler.
#
# These are fetched at configure time. If the build machine has no network access,
# point FetchContent at a local checkout instead of downloading:
#
#   cmake -DFETCHCONTENT_SOURCE_DIR_GLM=D:/deps/glm \
#         -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=D:/deps/json \
#         -DFETCHCONTENT_SOURCE_DIR_DOCTEST=D:/deps/doctest ...
#
# (FETCHCONTENT_SOURCE_DIR_<UPPERCASED_NAME> is a built-in FetchContent override and
#  makes the corresponding GIT_REPOSITORY / URL unnecessary.)

include( FetchContent )

set( TILES3D_GLM_TAG "1.0.1" CACHE STRING "glm release tag" )
set( TILES3D_NLOHMANN_JSON_TAG "v3.11.3" CACHE STRING "nlohmann/json release tag" )
set( TILES3D_DOCTEST_TAG "v2.4.11" CACHE STRING "doctest release tag" )

# Keep the sub-projects from building their own test suites.
set( GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE )
set( JSON_BuildTests OFF CACHE BOOL "" FORCE )
set( DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE )
set( DOCTEST_WITH_TESTS_EXPORT OFF CACHE BOOL "" FORCE )

# Keep the sub-projects from registering install rules into our top-level install: they
# are header-only / test-only deps, so the extension must not ship their headers, and we
# must not require administrator rights when installing into a system prefix.
set( GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE )
set( JSON_Install OFF CACHE BOOL "" FORCE )
set( DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE )

FetchContent_Declare( glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG ${TILES3D_GLM_TAG}
    GIT_SHALLOW TRUE
)
FetchContent_Declare( nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG ${TILES3D_NLOHMANN_JSON_TAG}
    GIT_SHALLOW TRUE
)
FetchContent_Declare( doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG ${TILES3D_DOCTEST_TAG}
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable( glm nlohmann_json doctest )

# Single interface target carrying everything the scheduler kernel needs.
# Doctest is intentionally not part of it: it is only linked by the test executables.
add_library( tiles3d_third_party INTERFACE )

target_link_libraries( tiles3d_third_party
    INTERFACE
        glm::glm
        nlohmann_json::nlohmann_json
)
