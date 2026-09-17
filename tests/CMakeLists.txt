# SPDX-License-Identifier: Unlicense

# Unit tests for the engine-agnostic scheduling kernel (tiles3d_core).
#
# Run via:  ctest --test-dir <build dir> --output-on-failure
# or run the executable directly: <build dir>/tests/tiles3d_tests

set( tiles3d_test_src )
file( GLOB_RECURSE tiles3d_test_src CONFIGURE_DEPENDS
        "*.cpp" )

add_executable( tiles3d_tests
    ${tiles3d_test_src}
)

target_compile_features( tiles3d_tests PRIVATE cxx_std_20 )

target_link_libraries( tiles3d_tests
    PRIVATE
        tiles3d_core
        doctest::doctest
)

# doctest, when DOCTEST_CONFIG_USE_STD_HEADERS is not defined, forward-declares
# std::tuple / std::allocator / std::basic_string itself instead of including the real
# standard headers (see doctest.h around line 538) to keep compile times down. MSVC 19.51
# (VS 2026) rejects those redeclarations - "warning C5285: specializing this standard
# library template is forbidden" - and the resulting type-identity mismatch between the
# doctest translation units and the standard library is an ODR violation that crashes
# during static test registration, before main runs any output.
#
# Defining this makes doctest use the real headers, which removes the forward
# declarations entirely. It must be visible to every translation unit that includes
# doctest.h, hence a target level definition rather than something in a single source.
target_compile_definitions( tiles3d_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS )

set_target_properties( tiles3d_tests
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

add_test( NAME tiles3d_core COMMAND tiles3d_tests )
