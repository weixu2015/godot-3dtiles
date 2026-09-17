// SPDX-License-Identifier: Unlicense
//
// Tile tree tests: parsing, refine inheritance, pre-order ids and the region bounding
// volume conversion.
//
// The parsing cases mirror the parseTilesetJson block of __tests__/parsers.spec.ts;
// refine inheritance and id assignment are additions, because both are behaviours the
// reference had to fix once and could regress silently.
//
// Note: only CHECK / CHECK_FALSE / SUBCASE are used. The test binaries currently compile
// with DOCTEST_CONFIG_NO_EXCEPTIONS (no /EHsc), under which doctest does not define the
// REQUIRE family at all.

#include <doctest/doctest.h>

#include "tiles/Tile.h"
#include "tiles/TilesetJson.h"

#include <array>
#include <cmath>
#include <string>

using namespace tiles3d::core;
using namespace tiles3d::math;

namespace
{
    /// Parses a fixture, failing the test rather than throwing if the literal is invalid.
    nlohmann::json parseFixture( const char *source )
    {
        nlohmann::json document = nlohmann::json::parse( source, nullptr, /* allow_exceptions */ false );
        CHECK_FALSE( document.is_discarded() );
        return document;
    }

    /// Column-major translation matrix, the layout tileset.json uses.
    Mat4 translation( double x, double y, double z )
    {
        Mat4 m = identity();
        m[3][0] = x;
        m[3][1] = y;
        m[3][2] = z;
        return m;
    }

    double distance( const Vec3 &a, const Vec3 &b )
    {
        return std::sqrt( ( a.x - b.x ) * ( a.x - b.x ) + ( a.y - b.y ) * ( a.y - b.y ) +
                          ( a.z - b.z ) * ( a.z - b.z ) );
    }

    std::size_t countTiles( const Tile &tile )
    {
        std::size_t total = 1;
        for ( const std::unique_ptr<Tile> &child : tile.children )
        {
            total += countTiles( *child );
        }
        return total;
    }

    const Tile &childAt( const Tile &tile, std::size_t index )
    {
        return *tile.children.at( index );
    }
} // namespace

TEST_CASE( "parses a 1.0 extension implicit tileset and propagates the content template" )
{
    nlohmann::json document = parseFixture( R"({
        "asset": { "version": "1.0" },
        "geometricError": 500,
        "root": {
            "boundingVolume": { "box": [0,0,0, 100,0,0, 0,100,0, 0,0,100] },
            "geometricError": 500,
            "refine": "REPLACE",
            "content": { "uri": "content/{level}/{x}/{y}.b3dm" },
            "extensions": {
                "3DTILES_implicit_tiling": {
                    "subdivisionScheme": "QUADTREE",
                    "subtreeLevels": 2,
                    "maximumLevel": 4,
                    "subtrees": { "uri": "subtrees/{level}/{x}/{y}.subtree" }
                }
            }
        }
    })" );

    TilesetParseResult result = parseTilesetJson( document );

    CHECK( static_cast<bool>( result ) );
    CHECK( result.error.empty() );
    CHECK( result.assetVersion == "1.0" );
    CHECK( result.geometricError == 500.0 );

    const Tile &root = *result.root;
    CHECK( root.implicitTiling.has_value() );
    CHECK( root.implicitTiling->subdivisionScheme == ImplicitTiling::SubdivisionScheme::Quadtree );
    CHECK( root.implicitTiling->subtreeLevels == 2 );
    CHECK( root.implicitTiling->maximumLevel == 4 );
    CHECK( root.implicitTiling->subtreeUriTemplate == "subtrees/{level}/{x}/{y}.subtree" );

    // The template must be moved out of Tile::content and into the implicit declaration.
    CHECK( root.implicitTiling->contentUriTemplate.has_value() );
    CHECK( *root.implicitTiling->contentUriTemplate == "content/{level}/{x}/{y}.b3dm" );
    CHECK_FALSE( root.content.has_value() );

    CHECK( root.implicitCoordinates.has_value() );
    CHECK( root.implicitCoordinates->level == 0 );
    CHECK( root.implicitCoordinates->x == 0 );
    CHECK( root.implicitCoordinates->y == 0 );
    CHECK( root.refine == RefineMode::Replace );
}

TEST_CASE( "parses a 1.1 core implicit tileset" )
{
    nlohmann::json document = parseFixture( R"({
        "asset": { "version": "1.1" },
        "geometricError": 500,
        "root": {
            "boundingVolume": { "box": [0,0,0, 100,0,0, 0,100,0, 0,0,100] },
            "geometricError": 500,
            "implicitTiling": {
                "subdivisionScheme": "OCTREE",
                "subtreeLevels": 3,
                "maximumLevel": 6,
                "subtrees": { "uri": "subtrees/{level}.subtree" },
                "content": { "uri": "content/{level}.b3dm" }
            }
        }
    })" );

    TilesetParseResult result = parseTilesetJson( document );

    CHECK( static_cast<bool>( result ) );
    CHECK( result.assetVersion == "1.1" );

    const Tile &root = *result.root;
    CHECK( root.implicitTiling.has_value() );
    CHECK( root.implicitTiling->subdivisionScheme == ImplicitTiling::SubdivisionScheme::Octree );
    CHECK( root.implicitTiling->branchingFactor() == 8 );
    CHECK( root.implicitTiling->subtreeLevels == 3 );
    CHECK( root.implicitTiling->maximumLevel == 6 );
    CHECK( root.implicitTiling->contentUriTemplate.has_value() );

    // No tile level content, but the root itself must not claim to have content.
    CHECK_FALSE( root.content.has_value() );
}

TEST_CASE( "parses an explicit tile tree with transform, refine and children" )
{
    nlohmann::json document = parseFixture( R"({
        "asset": { "version": "1.0" },
        "geometricError": 500,
        "root": {
            "boundingVolume": { "box": [0,0,0, 100,0,0, 0,100,0, 0,0,100] },
            "geometricError": 500,
            "refine": "ADD",
            "transform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,30,1],
            "content": { "uri": "root.glb" },
            "children": [
                {
                    "boundingVolume": { "sphere": [0,0,0, 50] },
                    "geometricError": 250,
                    "content": { "uri": "child.b3dm" }
                }
            ]
        }
    })" );

    TilesetParseResult result = parseTilesetJson( document );

    CHECK( static_cast<bool>( result ) );
    CHECK( result.root->children.size() == 1 );
    CHECK( countTiles( *result.root ) == 2 );

    // transform is column-major: the translation lives in column 3.
    CHECK( result.root->transform[3][0] == 10.0 );
    CHECK( result.root->transform[3][1] == 20.0 );
    CHECK( result.root->transform[3][2] == 30.0 );
    CHECK( result.root->transform[0][0] == 1.0 );
    CHECK( result.root->transform[1][1] == 1.0 );

    CHECK( result.root->content.has_value() );
    CHECK( result.root->content->uri == "root.glb" );

    CHECK( childAt( *result.root, 0 ).content.has_value() );
    CHECK( childAt( *result.root, 0 ).content->uri == "child.b3dm" );
    CHECK( childAt( *result.root, 0 ).boundingVolume.has_value() );
    CHECK( childAt( *result.root, 0 ).boundingVolume->type == BoundingVolume::Type::Sphere );
    CHECK( childAt( *result.root, 0 ).boundingVolume->data[3] == 50.0 );
    CHECK( childAt( *result.root, 0 ).depth == 1 );
}

TEST_CASE( "refine is inherited from the parent when a tile omits it" )
{
    // The shape that matters in practice: photogrammetry tilesets declare REPLACE on the
    // root only. Defaulting the children to ADD would render every level at once and let
    // coarse parents cover the fine ones.
    nlohmann::json document = parseFixture( R"({
        "asset": { "version": "1.0" },
        "geometricError": 500,
        "root": {
            "boundingVolume": { "box": [0,0,0, 100,0,0, 0,100,0, 0,0,100] },
            "geometricError": 500,
            "refine": "REPLACE",
            "children": [
                {
                    "boundingVolume": { "box": [0,0,0, 50,0,0, 0,50,0, 0,0,50] },
                    "geometricError": 250,
                    "children": [
                        {
                            "boundingVolume": { "box": [0,0,0, 25,0,0, 0,25,0, 0,0,25] },
                            "geometricError": 125
                        }
                    ]
                }
            ]
        }
    })" );

    TilesetParseResult result = parseTilesetJson( document );

    CHECK( static_cast<bool>( result ) );
    CHECK( result.root->refine == RefineMode::Replace );
    CHECK( childAt( *result.root, 0 ).refine == RefineMode::Replace );
    CHECK( childAt( childAt( *result.root, 0 ), 0 ).refine == RefineMode::Replace );
}

TEST_CASE( "an explicit child refine overrides the inherited one" )
{
    nlohmann::json document = parseFixture( R"({
        "asset": { "version": "1.0" },
        "geometricError": 500,
        "root": {
            "boundingVolume": { "box": [0,0,0, 100,0,0, 0,100,0, 0,0,100] },
            "geometricError": 500,
            "refine": "REPLACE",
            "children": [
                {
                    "boundingVolume": { "box": [0,0,0, 50,0,0, 0,50,0, 0,0,50] },
                    "geometricError": 250,
                    "refine": "ADD"
                }
            ]
        }
    })" );

    TilesetParseResult result = parseTilesetJson( document );

    CHECK( static_cast<bool>( result ) );
    CHECK( result.root->refine == RefineMode::Replace );
    CHECK( childAt( *result.root, 0 ).refine == RefineMode::Add );
}

TEST_CASE( "tile ids are assigned in pre-order" )
{
    // Deterministic ids are what lets tools/sched_trace diff a C++ traversal against the
    // TypeScript one, so the assignment order is part of the contract.
    nlohmann::json document = parseFixture( R"({
        "asset": { "version": "1.0" },
        "geometricError": 500,
        "root": {
            "boundingVolume": { "box": [0,0,0, 100,0,0, 0,100,0, 0,0,100] },
            "geometricError": 500,
            "children": [
                {
                    "boundingVolume": { "box": [0,0,0, 50,0,0, 0,50,0, 0,0,50] },
                    "geometricError": 250,
                    "children": [
                        {
                            "boundingVolume": { "box": [0,0,0, 25,0,0, 0,25,0, 0,0,25] },
                            "geometricError": 125
                        }
                    ]
                },
                {
                    "boundingVolume": { "box": [0,0,0, 50,0,0, 0,50,0, 0,0,50] },
                    "geometricError": 250
                }
            ]
        }
    })" );

    TilesetParseResult result = parseTilesetJson( document );

    CHECK( static_cast<bool>( result ) );

    const Tile &root = *result.root;
    const Tile &firstChild = childAt( root, 0 );
    const Tile &firstGrandchild = childAt( firstChild, 0 );
    const Tile &secondChild = childAt( root, 1 );

    CHECK( root.id == 0 );
    CHECK( firstChild.id == 1 );
    CHECK( firstGrandchild.id == 2 );
    CHECK( secondChild.id == 3 );
}

// ---------------------------------------------------------------------------
// Failure reporting. The core layer does not throw, so these check `error` instead.
// ---------------------------------------------------------------------------

TEST_CASE( "a tile without a bounding volume is rejected" )
{
    nlohmann::json document = parseFixture( R"({
        "asset": { "version": "1.0" },
        "geometricError": 500,
        "root": { "geometricError": 500 }
    })" );

    TilesetParseResult result = parseTilesetJson( document );

    CHECK_FALSE( static_cast<bool>( result ) );
    CHECK( result.error.find( "boundingVolume" ) != std::string::npos );
}

TEST_CASE( "a malformed bounding volume is rejected rather than silently accepted" )
{
    // The reference accepts any array length here and produces NaN geometry later.
    nlohmann::json document = parseFixture( R"({
        "asset": { "version": "1.0" },
        "geometricError": 500,
        "root": {
            "boundingVolume": { "box": [0,0,0, 100,0,0] },
            "geometricError": 500
        }
    })" );

    TilesetParseResult result = parseTilesetJson( document );

    CHECK_FALSE( static_cast<bool>( result ) );
    CHECK( result.error.find( "malformed" ) != std::string::npos );
}

TEST_CASE( "a document without asset.version is rejected" )
{
    nlohmann::json document = parseFixture( R"({
        "geometricError": 500,
        "root": { "boundingVolume": { "box": [0,0,0, 1,0,0, 0,1,0, 0,0,1] }, "geometricError": 1 }
    })" );

    TilesetParseResult result = parseTilesetJson( document );

    CHECK_FALSE( static_cast<bool>( result ) );
    CHECK( result.error.find( "asset.version" ) != std::string::npos );
}

TEST_CASE( "parseBoundingVolume maps each representation" )
{
    const nlohmann::json box = parseFixture( R"({"box":[1,2,3, 10,0,0, 0,10,0, 0,0,10]})" );
    const auto parsedBox = parseBoundingVolume( box );
    CHECK( parsedBox.has_value() );
    CHECK( parsedBox->type == BoundingVolume::Type::Box );
    CHECK( parsedBox->data[0] == 1.0 );
    CHECK( parsedBox->data[3] == 10.0 );
    CHECK( boundingVolumeRadius( *parsedBox ) == doctest::Approx( std::sqrt( 300.0 ) ).epsilon( 1e-12 ) );

    const nlohmann::json sphere = parseFixture( R"({"sphere":[5,6,7, 4.5]})" );
    const auto parsedSphere = parseBoundingVolume( sphere );
    CHECK( parsedSphere.has_value() );
    CHECK( parsedSphere->type == BoundingVolume::Type::Sphere );
    CHECK( parsedSphere->data[3] == 4.5 );

    const nlohmann::json region = parseFixture( R"({"region":[0,0,0.1,0.1, 0,100]})" );
    const auto parsedRegion = parseBoundingVolume( region );
    CHECK( parsedRegion.has_value() );
    CHECK( parsedRegion->type == BoundingVolume::Type::Region );
    CHECK( parsedRegion->asRegion().maxHeight == 100.0 );

    const nlohmann::json unknown = parseFixture( R"({"ellipsoid":[1,2,3]})" );
    CHECK_FALSE( parseBoundingVolume( unknown ).has_value() );
}
