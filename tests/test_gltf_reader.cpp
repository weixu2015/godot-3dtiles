// SPDX-License-Identifier: Unlicense
//
// GltfReader tests: synthetic minimal GLBs exercising the accessor model (typed
// components, normalization, strides), index handling, scene-graph transforms,
// material parsing and the documented failure paths.
//
// Note: only CHECK / CHECK_FALSE / SUBCASE are used, with explicit early returns
// for the "must not continue" guards. The test binaries compile with
// DOCTEST_CONFIG_NO_EXCEPTIONS (no /EHsc), under which doctest does not define
// the REQUIRE family at all. Near-zero float comparisons use std::abs with an
// absolute tolerance - doctest::Approx has no margin().

#include <doctest/doctest.h>

#include "content/GltfReader.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// `math` lives at tiles3d::math, not inside tiles3d::core. The core sources can write
// `math::Mat4` because unqualified lookup walks outward from tiles3d::core to tiles3d;
// this test sits at global scope, so tiles3d has to be reachable explicitly.
using namespace tiles3d;
using namespace tiles3d::core;

namespace
{
    void appendU32( std::vector<std::byte> &out, const std::uint32_t value )
    {
        out.push_back( static_cast<std::byte>( value & 0xFFu ) );
        out.push_back( static_cast<std::byte>( ( value >> 8 ) & 0xFFu ) );
        out.push_back( static_cast<std::byte>( ( value >> 16 ) & 0xFFu ) );
        out.push_back( static_cast<std::byte>( ( value >> 24 ) & 0xFFu ) );
    }

    std::size_t alignUp4( const std::size_t value )
    {
        return ( value + 3u ) / 4u * 4u;
    }

    /// Packs JSON and a binary chunk into a GLB container, padding both to 4 bytes
    /// (JSON with spaces, BIN with zeros) as the container format requires.
    std::vector<std::byte> buildGlb( const std::string &json,
                                     const std::vector<std::byte> &bin )
    {
        const std::size_t jsonPadded = alignUp4( json.size() );
        const std::size_t binPadded = alignUp4( bin.size() );

        std::vector<std::byte> out;
        appendU32( out, 0x46546C67u ); // "glTF"
        appendU32( out, 2u );
        appendU32( out, static_cast<std::uint32_t>( 12u + 8u + jsonPadded + 8u + binPadded ) );

        appendU32( out, static_cast<std::uint32_t>( jsonPadded ) );
        appendU32( out, 0x4E4F534Au ); // "JSON"
        const auto *jsonBytes = reinterpret_cast<const std::byte *>( json.data() );
        out.insert( out.end(), jsonBytes, jsonBytes + json.size() );
        for ( std::size_t i = json.size(); i < jsonPadded; ++i )
        {
            out.push_back( std::byte{ 0x20 } );
        }

        appendU32( out, static_cast<std::uint32_t>( binPadded ) );
        appendU32( out, 0x004E4942u ); // "BIN\0"
        out.insert( out.end(), bin.begin(), bin.end() );
        for ( std::size_t i = bin.size(); i < binPadded; ++i )
        {
            out.push_back( std::byte{ 0 } );
        }

        return out;
    }

    bool near( const float actual, const float expected, const float tolerance = 1e-5f )
    {
        return std::abs( actual - expected ) < tolerance;
    }
} // namespace

TEST_CASE( "plain primitive: float positions, u16 indices, texcoords and material" )
{
    // One triangle: positions (0,0,0) (1,0,0) (0,1,0), uv corner per vertex,
    // indices 0 1 2 as UNSIGNED_SHORT, one unlit material without texture.
    const std::string json = R"({
        "asset": {"version": "2.0"},
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 1, "TEXCOORD_0": 2},
            "indices": 0,
            "material": 0
        }]}],
        "accessors": [
            {"componentType": 5123, "count": 3, "type": "SCALAR"},
            {"componentType": 5126, "count": 3, "type": "VEC3",
             "min": [0,0,0], "max": [1,1,0]},
            {"componentType": 5126, "count": 3, "type": "VEC2"}
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0,  "byteLength": 6},
            {"buffer": 0, "byteOffset": 8,  "byteLength": 36},
            {"buffer": 0, "byteOffset": 48, "byteLength": 24}
        ],
        "buffers": [{"byteLength": 72}],
        "materials": [{
            "pbrMetallicRoughness": {"baseColorFactor": [0.25, 0.5, 0.75, 1.0]},
            "extensions": {"KHR_materials_unlit": {}}
        }]
    })";

    // Layout must match the JSON above: indices u16 at 0 (6 bytes), positions f32
    // VEC3 x3 at 8 (36 bytes), texcoords f32 VEC2 x3 at 48 (24 bytes), total 72.
    std::vector<std::byte> bin( 72u, std::byte{ 0 } );
    const std::uint16_t indices[3] = { 0, 1, 2 };
    std::memcpy( bin.data(), indices, sizeof( indices ) );
    const float positions[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    std::memcpy( bin.data() + 8u, positions, sizeof( positions ) );
    const float uvs[6] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
    std::memcpy( bin.data() + 48u, uvs, sizeof( uvs ) );

    GltfModel model;
    std::string error;
    const bool parsed = parseGltfModel( buildGlb( json, bin ), model, error );

    // The hand built binary chunk above must line up with the hand written JSON;
    // if the offsets drift, the parse must fail loudly rather than lie.
    CHECK( parsed );
    CHECK( error.empty() );
    if ( !parsed || model.primitives.size() != 1u )
    {
        return;
    }

    const GltfPrimitiveData &primitive = model.primitives[0];

    CHECK( primitive.positions.size() == 9u );
    CHECK( near( primitive.positions[0], 0.0f ) );
    CHECK( near( primitive.positions[3], 1.0f ) );
    CHECK( near( primitive.positions[7], 1.0f ) );

    CHECK( primitive.texcoords0.size() == 6u );
    CHECK( near( primitive.texcoords0[2], 1.0f ) );

    CHECK( primitive.indices.size() == 3u );
    CHECK( primitive.indices[0] == 0u );
    CHECK( primitive.indices[1] == 1u );
    CHECK( primitive.indices[2] == 2u );

    CHECK( model.materials.size() == 1u );
    CHECK( model.materials[0].unlit );
    CHECK( near( model.materials[0].baseColorFactor[1], 0.5f ) );
    CHECK( primitive.material == 0 );
}

TEST_CASE( "normalized UByte COLOR_0 is scaled to 0..1 and RGB expands to RGBA" )
{
    // Two vertices, COLOR_0 as VEC3 UNSIGNED_BYTE normalized: (255,0,128) and (0,0,0).
    const std::string json = R"({
        "asset": {"version": "2.0"},
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1}}]}],
        "accessors": [
            {"componentType": 5126, "count": 2, "type": "VEC3"},
            {"componentType": 5121, "count": 2, "type": "VEC3", "normalized": true}
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 24},
            {"buffer": 0, "byteOffset": 24, "byteLength": 6}
        ],
        "buffers": [{"byteLength": 30}]
    })";

    std::vector<std::byte> bin( 30u, std::byte{ 0 } );
    const float positions[6] = { 0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 3.0f };
    std::memcpy( bin.data(), positions, sizeof( positions ) );
    bin[24] = std::byte{ 255 };
    bin[25] = std::byte{ 0 };
    bin[26] = std::byte{ 128 };

    GltfModel model;
    std::string error;
    CHECK( parseGltfModel( buildGlb( json, bin ), model, error ) );
    if ( model.primitives.size() != 1u )
    {
        return;
    }

    const GltfPrimitiveData &primitive = model.primitives[0];

    // Non-indexed primitive: sequential indices are materialized.
    CHECK( primitive.indices.size() == 2u );
    CHECK( primitive.indices[1] == 1u );

    CHECK( primitive.colors.size() == 8u );
    CHECK( near( primitive.colors[0], 1.0f ) );
    CHECK( near( primitive.colors[1], 0.0f ) );
    CHECK( near( primitive.colors[2], 128.0f / 255.0f ) );
    CHECK( near( primitive.colors[3], 1.0f ) ); // alpha defaulted
}

TEST_CASE( "node transforms: matrix passthrough and TRS composition" )
{
    const std::string json = R"({
        "asset": {"version": "2.0"},
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"mesh": 0, "translation": [10, 20, 30],
             "rotation": [0, 0, 0, 1], "scale": [2, 2, 2]},
            {"mesh": 0, "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 5,6,7,1]}
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        "accessors": [{"componentType": 5126, "count": 1, "type": "VEC3"}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 12}],
        "buffers": [{"byteLength": 12}]
    })";

    std::vector<std::byte> bin( 12u, std::byte{ 0 } );
    const float position[3] = { 1.0f, 0.0f, 0.0f };
    std::memcpy( bin.data(), position, sizeof( position ) );

    GltfModel model;
    std::string error;
    CHECK( parseGltfModel( buildGlb( json, bin ), model, error ) );
    if ( model.nodes.size() != 2u || model.sceneNodes.size() != 2u )
    {
        return;
    }

    // TRS node: translation column, uniform scale 2 on the diagonal.
    const math::Mat4 &trs = model.nodes[0].transform;
    CHECK( trs[3][0] == 10.0 );
    CHECK( trs[3][1] == 20.0 );
    CHECK( trs[3][2] == 30.0 );
    CHECK( near( static_cast<float>( trs[0][0] ), 2.0f ) );
    CHECK( near( static_cast<float>( trs[1][1] ), 2.0f ) );
    CHECK( near( static_cast<float>( trs[2][2] ), 2.0f ) );

    // Matrix node: stored column-major, so the translation lands in column 3.
    const math::Mat4 &mat = model.nodes[1].transform;
    CHECK( mat[3][0] == 5.0 );
    CHECK( mat[3][1] == 6.0 );
    CHECK( mat[3][2] == 7.0 );
    CHECK( mat[0][0] == 1.0 );
}

TEST_CASE( "TRIANGLE_STRIP expands to triangles with alternating winding" )
{
    const std::string json = R"({
        "asset": {"version": "2.0"},
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 0},
            "indices": 0,
            "mode": 5
        }]}],
        "accessors": [
            {"componentType": 5123, "count": 4, "type": "SCALAR"},
            {"componentType": 5126, "count": 4, "type": "VEC3"}
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 8},
            {"buffer": 0, "byteOffset": 8, "byteLength": 48}
        ],
        "buffers": [{"byteLength": 56}]
    })";

    std::vector<std::byte> bin( 56u, std::byte{ 0 } );
    const std::uint16_t strip[4] = { 0, 1, 2, 3 };
    std::memcpy( bin.data(), strip, sizeof( strip ) );

    GltfModel model;
    std::string error;
    CHECK( parseGltfModel( buildGlb( json, bin ), model, error ) );
    if ( model.primitives.size() != 1u )
    {
        return;
    }

    const GltfPrimitiveData &primitive = model.primitives[0];

    // (0,1,2) keeps the strip order; (1,3,2) flips the odd triangle's winding.
    CHECK( primitive.indices.size() == 6u );
    CHECK( primitive.indices[0] == 0u );
    CHECK( primitive.indices[1] == 1u );
    CHECK( primitive.indices[2] == 2u );
    CHECK( primitive.indices[3] == 1u );
    CHECK( primitive.indices[4] == 3u );
    CHECK( primitive.indices[5] == 2u );
}

TEST_CASE( "documented failure paths fail with a named reason" )
{
    SUBCASE( "sparse accessor" )
    {
        const std::string json = R"({
            "asset": {"version": "2.0"},
            "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
            "accessors": [{"componentType": 5126, "count": 1, "type": "VEC3",
                           "sparse": {"count": 1}}],
            "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 12}],
            "buffers": [{"byteLength": 12}]
        })";

        GltfModel model;
        std::string error;
        CHECK_FALSE( parseGltfModel( buildGlb( json, std::vector<std::byte>( 12u ) ), model,
                                     error ) );
        CHECK( error.find( "sparse" ) != std::string::npos );
    }

    SUBCASE( "external .bin buffer" )
    {
        const std::string json = R"({
            "asset": {"version": "2.0"},
            "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
            "accessors": [{"componentType": 5126, "count": 1, "type": "VEC3"}],
            "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 12}],
            "buffers": [{"uri": "geometry.bin", "byteLength": 12}]
        })";

        GltfModel model;
        std::string error;
        CHECK_FALSE( parseGltfModel( buildGlb( json, std::vector<std::byte>( 12u ) ), model,
                                     error ) );
        CHECK( error.find( "external" ) != std::string::npos );
    }

    SUBCASE( "POINTS mode" )
    {
        const std::string json = R"({
            "asset": {"version": "2.0"},
            "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "mode": 0}]}],
            "accessors": [{"componentType": 5126, "count": 1, "type": "VEC3"}],
            "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 12}],
            "buffers": [{"byteLength": 12}]
        })";

        GltfModel model;
        std::string error;
        CHECK_FALSE( parseGltfModel( buildGlb( json, std::vector<std::byte>( 12u ) ), model,
                                     error ) );
        CHECK( error.find( "mode" ) != std::string::npos );
    }

    SUBCASE( "index out of vertex range" )
    {
        const std::string json = R"({
            "asset": {"version": "2.0"},
            "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
            "accessors": [
                {"componentType": 5126, "count": 2, "type": "VEC3"},
                {"componentType": 5125, "count": 3, "type": "SCALAR"}
            ],
            "bufferViews": [
                {"buffer": 0, "byteOffset": 0, "byteLength": 24},
                {"buffer": 0, "byteOffset": 24, "byteLength": 12}
            ],
            "buffers": [{"byteLength": 36}]
        })";

        std::vector<std::byte> bin( 36u, std::byte{ 0 } );
        const std::uint32_t badIndex[3] = { 0, 1, 7 };
        std::memcpy( bin.data() + 24u, badIndex, sizeof( badIndex ) );

        GltfModel model;
        std::string error;
        CHECK_FALSE( parseGltfModel( buildGlb( json, bin ), model, error ) );
        CHECK( error.find( "index out of range" ) != std::string::npos );
    }
}

TEST_CASE( "textures and embedded images resolve through bufferViews" )
{
    const std::string json = R"({
        "asset": {"version": "2.0"},
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}],
        "accessors": [{"componentType": 5126, "count": 1, "type": "VEC3"}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 12},
            {"buffer": 0, "byteOffset": 12, "byteLength": 4}
        ],
        "buffers": [{"byteLength": 16}],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
        "textures": [{"sampler": 0, "source": 0}],
        "images": [{"mimeType": "image/jpeg", "bufferView": 1}],
        "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 10497}]
    })";

    std::vector<std::byte> bin( 16u, std::byte{ 0 } );
    bin[12] = std::byte{ 0xFF };
    bin[13] = std::byte{ 0xD8 }; // JPEG SOI marker, enough to identify the span

    GltfModel model;
    std::string error;
    CHECK( parseGltfModel( buildGlb( json, bin ), model, error ) );

    CHECK( model.images.size() == 1u );
    if ( model.images.size() == 1u )
    {
        CHECK( model.images[0].offset == 12u );
        CHECK( model.images[0].length == 4u );
        CHECK( model.images[0].mimeType == "image/jpeg" );
        CHECK( model.bin[model.images[0].offset + 1u] == std::byte{ 0xD8 } );
    }

    CHECK( model.textures.size() == 1u );
    if ( model.textures.size() == 1u )
    {
        CHECK( model.textures[0].imageIndex == 0 );
        CHECK( model.textures[0].minFilter == 9729u );
        CHECK( model.textures[0].wrapS == 33071u );
    }
}
