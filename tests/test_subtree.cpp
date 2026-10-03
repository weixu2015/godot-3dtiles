// SPDX-License-Identifier: Unlicense
//
// 3D Tiles 1.1 implicit tiling: the `.subtree` binary reader and the subtree expansion.
//
// The reader cases mirror the reference __tests__/parsers.spec.ts (BitStream / SubtreeReader
// block), so a divergence means the port drifted. The expansion cases pin the Morton row
// walk, the geometric-error halving and the bottom-row placeholder rule, none of which the
// reference had covered before.
//
// Note: only CHECK / CHECK_FALSE are used. The test binaries compile with
// DOCTEST_CONFIG_NO_EXCEPTIONS, under which doctest does not define the REQUIRE family.

#include <doctest/doctest.h>

#include "math/BoundingVolume.h"
#include "tiles/Subtree.h"
#include "tiles/Tile.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace tiles3d::core;
using namespace tiles3d::math;

namespace
{
    void appendU32( std::vector<std::uint8_t> &out, std::uint32_t value )
    {
        for ( int i = 0; i < 4; ++i )
        {
            out.push_back( static_cast<std::uint8_t>( ( value >> ( i * 8 ) ) & 0xFF ) );
        }
    }

    void appendU64( std::vector<std::uint8_t> &out, std::uint64_t value )
    {
        for ( int i = 0; i < 8; ++i )
        {
            out.push_back( static_cast<std::uint8_t>( ( value >> ( i * 8 ) ) & 0xFF ) );
        }
    }

    /// Assembles a `.subtree` file: header (24 bytes) + JSON + binary.
    std::vector<std::uint8_t> buildSubtree( const std::string &json,
                                            const std::vector<std::uint8_t> &binary )
    {
        std::vector<std::uint8_t> out;
        out.push_back( 's' );
        out.push_back( 'u' );
        out.push_back( 'b' );
        out.push_back( 't' );
        appendU32( out, 1 );
        appendU64( out, json.size() );
        appendU64( out, binary.size() );
        out.insert( out.end(), json.begin(), json.end() );
        out.insert( out.end(), binary.begin(), binary.end() );
        return out;
    }

    /// Tile is non-copyable and non-movable (children hang off unique_ptr and the scheduler
    /// holds raw pointers into the tree), so the fixture hands back a heap tile.
    std::unique_ptr<Tile> makeImplicitParent( double geometricError )
    {
        auto parent = std::make_unique<Tile>();
        parent->implicitCoordinates = ImplicitCoordinates{ 0, 0, 0, 0 };
        parent->boundingVolume = BoundingVolume::fromBox( Vec3( 0.0 ), Vec3( 100, 0, 0 ),
                                                         Vec3( 0, 100, 0 ), Vec3( 0, 0, 100 ) );
        parent->geometricError = geometricError;
        parent->refine = RefineMode::Replace;
        return parent;
    }

    ImplicitTiling makeDeclaration( ImplicitTiling::SubdivisionScheme scheme, int subtreeLevels )
    {
        ImplicitTiling decl;
        decl.subdivisionScheme = scheme;
        decl.subtreeLevels = subtreeLevels;
        decl.subtreeUriTemplate = "subtrees/0/0/0/0.subtree";
        decl.contentUriTemplate = "tiles/{level}/{x}/{y}/{z}.glb";
        return decl;
    }
} // namespace

TEST_CASE( "parseSubtree decodes the header and LSB-first availability bits" )
{
    // 0b00000101 -> tile 0 and tile 2 available, tile 1 not.
    const std::string json = R"({
        "subtreeLevels": 2,
        "tileAvailability": { "bitstream": 0 },
        "contentAvailability": [ { "bitstream": 0 } ],
        "childSubtreeAvailability": { "constant": 0 },
        "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 1 } ]
    })";
    const std::vector<std::uint8_t> buffer = buildSubtree( json, { 0b00000101 } );

    SubtreeData subtree;
    std::string error;
    CHECK( parseSubtree( buffer.data(), buffer.size(), subtree, error ) );
    CHECK( error.empty() );

    CHECK( subtree.tileAvailable( 0 ) );
    CHECK_FALSE( subtree.tileAvailable( 1 ) );
    CHECK( subtree.tileAvailable( 2 ) );
    CHECK_FALSE( subtree.tileAvailable( 3 ) );

    CHECK( subtree.contentAvailable( 0, 0 ) );
    CHECK_FALSE( subtree.contentAvailable( 1, 0 ) );

    // constant 0 -> never available, whatever the index.
    CHECK_FALSE( subtree.childSubtreeAvailable( 0 ) );
    CHECK_FALSE( subtree.childSubtreeAvailable( 5 ) );
}

TEST_CASE( "parseSubtree treats a constant child-subtree stream as all ones" )
{
    const std::string json = R"({
        "subtreeLevels": 2,
        "tileAvailability": { "bitstream": 0 },
        "contentAvailability": [ { "bitstream": 0 } ],
        "childSubtreeAvailability": { "constant": 1 },
        "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 1 } ]
    })";
    const std::vector<std::uint8_t> buffer = buildSubtree( json, { 0b00000001 } );

    SubtreeData subtree;
    std::string error;
    CHECK( parseSubtree( buffer.data(), buffer.size(), subtree, error ) );
    CHECK( subtree.childSubtreeAvailable( 0 ) );
    CHECK( subtree.childSubtreeAvailable( 5 ) );
    CHECK( subtree.childSubtreeAvailable( 1000 ) );
}

TEST_CASE( "parseSubtree rejects a bad magic and a truncated file" )
{
    SubtreeData subtree;
    std::string error;

    const std::vector<std::uint8_t> badMagic = { 'x', 'x', 'x', 'x', 1, 0, 0, 0, 0, 0, 0, 0,
                                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK_FALSE( parseSubtree( badMagic.data(), badMagic.size(), subtree, error ) );
    CHECK( error.find( "magic" ) != std::string::npos );

    error.clear();
    const std::string json = R"({ "tileAvailability": { "constant": 0 } })";
    std::vector<std::uint8_t> truncated = buildSubtree( json, {} );
    truncated.resize( truncated.size() - 1 ); // chop the closing brace of the JSON
    CHECK_FALSE( parseSubtree( truncated.data(), truncated.size(), subtree, error ) );
}

TEST_CASE( "replaceTemplate instantiates level, x, y and z" )
{
    CHECK( replaceTemplate( "tiles/{level}/{x}/{y}/{z}.glb", 2, 1, 3, 0 ) == "tiles/2/1/3/0.glb" );
    CHECK( replaceTemplate( "subtrees/{level}.subtree", 4, 0, 0, 0 ) == "subtrees/4.subtree" );
    // No placeholders -> returned unchanged.
    CHECK( replaceTemplate( "content/0.glb", 9, 9, 9, 9 ) == "content/0.glb" );
}

TEST_CASE( "expandImplicitSubtree walks an octree subtree row by row" )
{
    auto parent = makeImplicitParent( 8.0 );
    const ImplicitTiling decl = makeDeclaration( ImplicitTiling::SubdivisionScheme::Octree, 2 );

    SubtreeData subtree;
    subtree.tileAvailability.kind = Availability::Kind::Constant;
    subtree.tileAvailability.constantValue = true;
    subtree.contentAvailability.push_back( subtree.tileAvailability );
    subtree.childSubtreeAvailability.kind = Availability::Kind::Constant;
    subtree.childSubtreeAvailability.constantValue = false;

    std::size_t nextId = 0;
    expandImplicitSubtree( *parent, subtree, decl, nextId );

    // subtreeLevels = 2 -> a single child level of 8 octants.
    CHECK( parent->children.size() == 8 );

    // Child 0 is the (x-, y-, z-) octant; the box is subdivided to its centre.
    const Tile &child0 = *parent->children[0];
    CHECK( child0.implicitCoordinates->level == 1 );
    CHECK( child0.implicitCoordinates->x == 0 );
    CHECK( child0.geometricError == doctest::Approx( 4.0 ) );
    CHECK( child0.boundingVolume->data[0] == doctest::Approx( -50.0 ) );
    CHECK( child0.boundingVolume->data[1] == doctest::Approx( -50.0 ) );
    CHECK( child0.boundingVolume->data[2] == doctest::Approx( -50.0 ) );
    CHECK( child0.boundingVolume->data[3] == doctest::Approx( 50.0 ) );

    // Child 5 (binary 101) is x+, y-, z+.
    const Tile &child5 = *parent->children[5];
    CHECK( child5.implicitCoordinates->x == 1 );
    CHECK( child5.implicitCoordinates->y == 0 );
    CHECK( child5.implicitCoordinates->z == 1 );
    CHECK( child5.boundingVolume->data[0] == doctest::Approx( 50.0 ) );
    CHECK( child5.boundingVolume->data[1] == doctest::Approx( -50.0 ) );
    CHECK( child5.boundingVolume->data[2] == doctest::Approx( 50.0 ) );

    // Content URIs come from the template with the child's Morton coordinates.
    CHECK( child5.content.has_value() );
    CHECK( child5.content->uri == "tiles/1/1/0/1.glb" );

    // No child subtrees -> no placeholders, so the leaves stay childless.
    CHECK( child0.children.empty() );
}

TEST_CASE( "expandImplicitSubtree creates a placeholder per available child subtree" )
{
    auto parent = makeImplicitParent( 8.0 );
    const ImplicitTiling decl = makeDeclaration( ImplicitTiling::SubdivisionScheme::Octree, 2 );

    SubtreeData subtree;
    subtree.tileAvailability.kind = Availability::Kind::Constant;
    subtree.tileAvailability.constantValue = true;
    subtree.contentAvailability.push_back( subtree.tileAvailability );
    subtree.childSubtreeAvailability.kind = Availability::Kind::Constant;
    subtree.childSubtreeAvailability.constantValue = true;

    std::size_t nextId = 0;
    expandImplicitSubtree( *parent, subtree, decl, nextId );

    CHECK( parent->children.size() == 8 );

    // Every bottom-row tile gets 8 placeholders, each carrying the implicit declaration so the
    // traversal can request the next subtree instead of treating it as a childless leaf.
    const Tile &leaf = *parent->children[0];
    CHECK( leaf.children.size() == 8 );
    CHECK( leaf.children[1]->implicitTiling.has_value() );
    CHECK( leaf.children[1]->implicitCoordinates->level == 2 );
    CHECK( leaf.children[1]->geometricError == doctest::Approx( 2.0 ) );
    // A placeholder has no content until its own subtree says so.
    CHECK_FALSE( leaf.children[1]->content.has_value() );
}

TEST_CASE( "expandImplicitSubtree keeps the z extent for a quadtree" )
{
    auto parent = makeImplicitParent( 8.0 );
    const ImplicitTiling decl = makeDeclaration( ImplicitTiling::SubdivisionScheme::Quadtree, 2 );

    SubtreeData subtree;
    subtree.tileAvailability.kind = Availability::Kind::Constant;
    subtree.tileAvailability.constantValue = true;
    subtree.contentAvailability.push_back( subtree.tileAvailability );
    subtree.childSubtreeAvailability.kind = Availability::Kind::Constant;
    subtree.childSubtreeAvailability.constantValue = false;

    std::size_t nextId = 0;
    expandImplicitSubtree( *parent, subtree, decl, nextId );

    CHECK( parent->children.size() == 4 );
    for ( const std::unique_ptr<Tile> &child : parent->children )
    {
        // Quadtree children keep the parent's z centre (0 here) and its z half extent (100).
        CHECK( child->boundingVolume->data[2] == doctest::Approx( 0.0 ) );
        CHECK( child->boundingVolume->data[11] == doctest::Approx( 100.0 ) );
        CHECK( child->implicitCoordinates->z == 0 );
    }
}
