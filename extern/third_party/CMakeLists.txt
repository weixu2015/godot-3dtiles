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

# ---------------------------------------------------------------------------
# Google Draco: the decoder behind KHR_draco_mesh_compression.
#
# Godot's core glTF importer does not implement that extension (godotengine/godot#73738),
# and every tileset produced by the Cesium ion tiling pipeline is Draco compressed, so the
# decoder has to ship with this extension. Vendored rather than fetched: it is a native
# library, and the build machine may have no network at all.
#
# The options below are not preferences. Leaving any of them at its default breaks the
# configure or the install:
#   DRACO_TRANSCODER_SUPPORTED  pulls Eigen / filesystem / tinygltf, whose submodule
#                               directories are not part of the trimmed checkout, and
#                               draco_die_missing_submodule aborts the configure.
#   DRACO_TESTS                 needs googletest, same story.
#   DRACO_INSTALL               would register install rules into our top-level install -
#                               exactly the leak already fixed for doctest / glm / nlohmann.
# ---------------------------------------------------------------------------
set( DRACO_TESTS OFF CACHE BOOL "" FORCE )
set( DRACO_TRANSCODER_SUPPORTED OFF CACHE BOOL "" FORCE )
set( DRACO_GLTF OFF CACHE BOOL "" FORCE )
set( DRACO_GLTF_BITSTREAM OFF CACHE BOOL "" FORCE )
set( DRACO_MAYA_PLUGIN OFF CACHE BOOL "" FORCE )
set( DRACO_UNITY_PLUGIN OFF CACHE BOOL "" FORCE )
set( DRACO_JS_GLUE OFF CACHE BOOL "" FORCE )
set( DRACO_INSTALL OFF CACHE BOOL "" FORCE )
set( DRACO_VERBOSE OFF CACHE BOOL "" FORCE )

# Out-of-tree binary directory: Draco refuses to configure when its source and binary
# directories are the same.
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/draco"
    "${CMAKE_CURRENT_BINARY_DIR}/draco"
)

# Draco does NOT export its include directories to consumers. Its add_library macro
# sets them as PRIVATE and the only PUBLIC include is an $<INSTALL_INTERFACE:include>
# generator expression that is empty at build time (see
# extern/third_party/draco/cmake/draco_targets.cmake draco_add_library). So a target
# that links draco::draco gets the library but none of the "draco/..." headers and
# fails with C1083. Re-export draco's own include path set, which is exactly
#   ${draco_root}  ${draco_root}/src  ${draco_build}
# (draco/cmake/draco_build_definitions.cmake:68-69):
#   <draco>/src        -> "draco/compression/decode.h", "draco/core/macros.h" ...
#   <draco>            -> draco repo root, part of draco's own path set
#   ${draco_build}     -> "draco/draco_features.h", generated at configure time into
#                         ${draco_build}/draco/. draco_build is set to
#                         ${CMAKE_BINARY_DIR} in draco/CMakeLists.txt:27, so this root
#                         is the TOP LEVEL build dir - not the draco binary dir.
set( TILES3D_DRACO_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/draco/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/draco"
    "${CMAKE_BINARY_DIR}"
)

add_library( tiles3d_draco INTERFACE )
target_include_directories( tiles3d_draco
    INTERFACE
        ${TILES3D_DRACO_INCLUDE_DIRS}
)
target_link_libraries( tiles3d_draco INTERFACE draco::draco )

# ---------------------------------------------------------------------------
# Basis Universal: the KTX2 transcoder behind glTF's KHR_texture_basisu.
#
# 3D Tiles 1.1 datasets tiled by the Cesium ion pipeline ship their textures as
# KTX2 (supercompressed ETC1S/UASTC), and declare KHR_texture_basisu in
# extensionsRequired. Godot's C++ bindings expose load_ktx_from_buffer (KTX1)
# but no KTX2 entry point, so the transcode to plain RGBA8 has to happen here.
#
# Vendored rather than fetched, for the same reason as Draco: it is a native
# library and the build machine may have no network at all. Only the transcoder
# half is vendored - the encoder (which pulls tinyexr / lodepng / OpenCL) is not
# needed to read tiles.
#
# Zstd is vendored as the official single-file amalgamation (built from the zstd
# 1.5.7 tree with its own build/single_file_libs/combine.py) because KTX2 UASTC
# payloads are zstd-supercompressed. basisu_transcoder.cpp only calls
# ZSTD_decompress / ZSTD_isError, so a decoder-only build is sufficient - but the
# amalgamation is used as generated rather than hand-trimmed.
#
# The transcoder is deliberately compiled as C++ here, not C: basisu_transcoder.cpp
# is C++ and pulls in basisu_containers_impl.h; zstd.c is plain C compiled by the
# C++ compiler, which zstd supports (it already guards for __cplusplus).
# ---------------------------------------------------------------------------
set( TILES3D_BASISU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/basisu" )

if ( NOT EXISTS "${TILES3D_BASISU_DIR}/transcoder/basisu_transcoder.cpp" )
    message(
        FATAL_ERROR
            "[${PROJECT_NAME}] The vendored Basis Universal transcoder is missing. "
            "Expected ${TILES3D_BASISU_DIR}/transcoder/basisu_transcoder.cpp"
    )
endif()

add_library( tiles3d_basisu STATIC
    "${TILES3D_BASISU_DIR}/transcoder/basisu_transcoder.cpp"
)

# zstd.c is vendored as the official single-file amalgamation. It is added as C
# rather than C++ because the project never enables the C language, so there is no
# C compiler rule to fall back on if this is dropped: adding a .c source without
# enable_language(C) is exactly what produced "Error required internal CMake
# variable not set: CMAKE_C_COMPILE_OBJECT" during generate.
set_source_files_properties( "${TILES3D_BASISU_DIR}/zstd/zstd.c"
    TARGET_DIRECTORY tiles3d_basisu
    PROPERTIES LANGUAGE CXX
)
target_sources( tiles3d_basisu PRIVATE "${TILES3D_BASISU_DIR}/zstd/zstd.c" )

# zstd.h is found by basisu_transcoder.cpp through this path ("#include \"zstd.h\""),
# and zstd.h itself includes zstd_errors.h from the same directory, so both the
# transcoder and zstd dirs have to be on the include path. The transcoder dir is
# PUBLIC because src/core/content/Ktx2Decoder.cpp includes "basisu_transcoder.h".
target_include_directories( tiles3d_basisu
    PUBLIC
        "${TILES3D_BASISU_DIR}/transcoder"
    PRIVATE
        "${TILES3D_BASISU_DIR}/zstd"
)

# MSVC needs this because basisu_transcoder.cpp is a very large translation unit
# (24000 lines) whose tables alone allocate a lot of read-only data, and because
# /utf-8 keeps the source's non-ASCII comments from being misinterpreted under a
# non-UTF-8 ACP. /bigobj is required outright: without it the object file exceeds
# the COFF section limit (C1128).
if ( MSVC )
    target_compile_options( tiles3d_basisu PRIVATE /bigobj /utf-8 )
endif()

# basisu ships encoder sources that assume a relaxed warning set, and the vendored
# tree is not ours to fix. Suppress just the codes that fire, not the whole analysis:
#   C4244 conversion, may lose data          - everywhere in the DCT/idct tables
#   C4267 size_t -> smaller type
#   C4838 narrowing conversion in braces
#   C4996 deprecated CRT (strcpy / fopen)    - the file-IO helper path
if ( MSVC )
    target_compile_options( tiles3d_basisu PRIVATE
        /wd4244
        /wd4267
        /wd4838
        /wd4996
    )
else()
    target_compile_options( tiles3d_basisu PRIVATE -w )
endif()

# Single interface target carrying everything the scheduler kernel needs.
# Doctest is intentionally not part of it: it is only linked by the test executables.
# Draco and Basis Universal are real static libraries, but they are routed through
# here so every consumer of the kernel links them too - a static library does not
# carry its own dependencies.
add_library( tiles3d_third_party INTERFACE )

target_link_libraries( tiles3d_third_party
    INTERFACE
        glm::glm
        nlohmann_json::nlohmann_json
        tiles3d_draco
        tiles3d_basisu
)
