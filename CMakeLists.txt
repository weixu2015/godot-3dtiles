# SPDX-License-Identifier: Unlicense

cmake_minimum_required( VERSION 3.22 )

message( STATUS "Using CMake ${CMAKE_VERSION}" )

# Require out-of-source builds
file( TO_CMAKE_PATH "${PROJECT_BINARY_DIR}/CMakeLists.txt" LOC_PATH )

if ( EXISTS "${LOC_PATH}" )
    message( FATAL_ERROR "You cannot build in the source directory. Please use a build subdirectory." )
endif()

# Add paths to modules
list( APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/" )

# Turn on link time optimization for everything
set( CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON )

# Output compile commands to compile_commands.json (for debugging CMake issues)
set( CMAKE_EXPORT_COMPILE_COMMANDS ON )

# Build universal lib on macOS
# Note that CMAKE_OSX_ARCHITECTURES must be set before project().
if ( APPLE )
    set( CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "" )
endif()

# Unit tests for the engine-agnostic scheduling kernel (src/core).
option( BUILD_TESTING "Build the tiles3d_core unit tests" ON )

# Main project information
project( godot-3dtiles
    LANGUAGES
        CXX
    VERSION
        0.0.1
)

# Install prefix defaults to the repository root, so `cmake --install` drops the built
# library plus the generated .gdextension file into demo/addons, which is where the demo
# project loads the extension from. This stays inside the project tree (no system path,
# no administrator rights). Override with -DCMAKE_INSTALL_PREFIX=... to install
# somewhere else (INSTALL_DIR below follows the prefix).
if ( CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT )
    set( CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}" CACHE PATH "Install prefix" FORCE )
endif()

set( INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/demo/addons" )
message( STATUS "Install directory: ${INSTALL_DIR}")

# MSVC runtime must match godot-cpp. godot-cpp (cmake/windows.cmake) forces a static
# runtime (MultiThreaded /MT) via the CMAKE_MSVC_RUNTIME_LIBRARY cache variable, but it
# only sets that variable inside add_subdirectory(extern) -- which runs AFTER this main
# target is created below. If left to CMake's default, the main target captures the
# dynamic runtime (/MDd) and linking fails with LNK2038 (RuntimeLibrary /
# _ITERATOR_DEBUG_LEVEL mismatch against libgodot-cpp). Set it up front so every target
# uses static /MT.
# NOTE: use uniform "MultiThreaded" (not "MultiThreaded$<$<CONFIG:Debug>:Debug>"); the
# latter yields /MTd in Debug, whose _ITERATOR_DEBUG_LEVEL (2) still mismatches godot-cpp's
# /MT (0).
if ( MSVC )
    set( CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" )
endif()

# Create our library
add_library( ${PROJECT_NAME} SHARED )

target_compile_features( ${PROJECT_NAME}
    PRIVATE
        cxx_std_20
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4 /WX
        /wd4100 /wd4244 /wd4267 /wd4996 /wd4828 /wd4010
        /utf-8
    )
endif()

# LIB_ARCH is the architecture being built. It is set to the build system's architecture.
# For macOS, we build a universal library (both arm64 and x86_64).
set( LIB_ARCH ${CMAKE_SYSTEM_PROCESSOR} )
if ( APPLE )
    set( LIB_ARCH "universal" )
endif()

# LIB_DIR is where the actual library ends up. This is used in both the build directory and the
# install directory and needs to be consistent with the paths in the gdextension file.
#   e.g. linux.release.x86_64 = "lib/Linux-x86_64/libGDExtensionTemplate.so"
set( LIB_DIR "lib/${CMAKE_SYSTEM_NAME}-${LIB_ARCH}" )

message( STATUS "Building ${PROJECT_NAME} for ${LIB_ARCH} on ${CMAKE_SYSTEM_NAME}")

# BUILD_OUTPUT_DIR is where we put the resulting library (in the build directory)
set( BUILD_OUTPUT_DIR "${PROJECT_BINARY_DIR}/${PROJECT_NAME}/" )

set_target_properties( ${PROJECT_NAME}
    PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN true
        RUNTIME_OUTPUT_DIRECTORY "${BUILD_OUTPUT_DIR}/${LIB_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${BUILD_OUTPUT_DIR}/${LIB_DIR}"
)

if( NOT DEFINED CMAKE_DEBUG_POSTFIX )
    set_target_properties( ${PROJECT_NAME}
        PROPERTIES
            DEBUG_POSTFIX "-d"
    )
endif()

# ccache must be included before any target is created (the root target included), so that
# CMAKE_CXX_COMPILER_LAUNCHER reaches every translation unit rather than only the
# subdirectories added after it.
include( ccache )

# Warnings
include( CompilerWarnings )

# Create and include version info file from git
include( GitVersionInfo )


# External dependencies first: src/ links tiles3d_core against tiles3d_third_party, and the
# extension library against godot-cpp, so those targets must already exist.
add_subdirectory( extern )

add_subdirectory( src )

# Install library, extension file, and support files in ${INSTALL_DIR}
install( TARGETS ${PROJECT_NAME}
    LIBRARY
        DESTINATION ${INSTALL_DIR}/${LIB_DIR}
    RUNTIME
        DESTINATION ${INSTALL_DIR}/${LIB_DIR}
)

add_subdirectory( templates )

if ( BUILD_TESTING )
    enable_testing()
    add_subdirectory( tests )
endif()

# Formatting
# Adds a custom target to format all the code at once
include( ClangFormat )
