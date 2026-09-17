// SPDX-License-Identifier: Unlicense
//
// convertRegionBoundingVolumes tests.
//
// A region bounding volume is EPSG:4979 absolute geodetic coordinates and is *not*
// transformed by the tile's transform chain, so it has to be rewritten into the tile's
// local frame once, up front. Getting this wrong shows up as bounding volumes that drift
// away from their geometry as the camera moves, which is expensive to debug later.
//
// Only CHECK / CHECK_FALSE are used: the test binaries build with
// DOCTEST_CONFIG_NO_EXCEPTIONS, under which doctest does not define the REQUIRE family.

#include <doctest/doctest.h>

#include "math/GeoMath.h"
#include "tiles/Tile.h"

#include <cmath>
#include <memory>

using namespace tiles3d::core;
using namespace tiles3d::math;

namespace
{
    /// A ~1.1 km patch sitting inside the demo dataset's area.
    const Region kTestRegion{ 0.02, 0.50, 0.03, 0.51, 120.0, 260.0 };

    /// Well below a millimetre, so it is far tighter than any error this could mask.
    constexpr double kTolerance = 1e-6;

    double distance( const Vec3 &a, const Vec3 &b )
    {
        return std::sqrt( ( a.x - b.x ) * ( a.x - b.x ) + ( a.y - b.y ) * ( a.y - b.y ) +
                          ( a.z - b.z ) * ( a.z - b.z ) );
    }

    Vec3 centreOf( const BoundingVolume &volume )
    {
        return Vec3( volume.data[0], volume.data[1], volume.data[2] );
    }

    Mat4 translation( double x, double y, double z )
    {
        Mat4 m = identity();
        m[3][0] = x;
        m[3][1] = y;
        m[3][2] = z;
        return m;
    }

    std::unique_ptr<Tile> makeRegionTile()
    {
        auto tile = std::make_unique<Tile>();
        tile->boundingVolume = BoundingVolume::fromRegion( kTestRegion );
        return tile;
    }

    void expectBoxMatches( const BoundingVolume &volume, const Vec3 &expectedCentre,
                           const std::array<Vec3, 3> &expectedHalfAxes )
    {
        CHECK( volume.type == BoundingVolume::Type::Box );
        CHECK( distance( centreOf( volume ), expectedCentre ) < kTolerance );

        for ( int axis = 0; axis < 3; ++axis )
        {
            const Vec3 actual = volume.boxHalfAxis( axis );
            CHECK( distance( actual, expectedHalfAxes[static_cast<std::size_t>( axis )] ) <
                   kTolerance );
        }
    }
} // namespace

TEST_CASE( "with identity transforms the region becomes its raw ECEF box" )
{
    std::unique_ptr<Tile> tile = makeRegionTile();
    const OrientedBoundingBox expected = regionToEcefObb( kTestRegion );

    convertRegionBoundingVolumes( *tile, identity(), identity() );

    CHECK( tile->boundingVolume.has_value() );
    expectBoxMatches( *tile->boundingVolume, expected.center, expected.halfAxes );
}

TEST_CASE( "the model matrix places the region in the render frame" )
{
    // parentChain = identity isolates the model matrix: the local frame is the render
    // frame, so the box centre must be modelMatrix * ECEF.
    std::unique_ptr<Tile> tile = makeRegionTile();

    const Mat4 model = translation( -6378000.0, 0.0, 0.0 );
    const Vec3 expectedCentre =
        transformPoint( model, regionToEcefObb( kTestRegion ).center );

    convertRegionBoundingVolumes( *tile, model, identity() );

    CHECK( tile->boundingVolume.has_value() );
    CHECK( distance( centreOf( *tile->boundingVolume ), expectedCentre ) < kTolerance );
}

TEST_CASE( "when parentChain equals the model matrix the two cancel out" )
{
    // This is the real call shape: Tileset3DRenderer passes modelMatrix as the default
    // parentChain. The root tile's worldMatrix is then the identity, so the region must
    // land back at its raw ECEF position for the traversal to place it correctly.
    std::unique_ptr<Tile> tile = makeRegionTile();

    const Mat4 model = translation( -6378000.0, 0.0, 0.0 );
    const OrientedBoundingBox expected = regionToEcefObb( kTestRegion );

    convertRegionBoundingVolumes( *tile, model, model );

    CHECK( tile->boundingVolume.has_value() );
    expectBoxMatches( *tile->boundingVolume, expected.center, expected.halfAxes );
}

TEST_CASE( "the tile transform chain is undone" )
{
    // tile.transform moves the tile, so the region - which does not follow the chain - has
    // to be offset by the inverse to stay put in world space.
    std::unique_ptr<Tile> tile = makeRegionTile();
    tile->transform = translation( 100.0, 0.0, 0.0 );

    const Vec3 expectedCentre = regionToEcefObb( kTestRegion ).center - Vec3( 100.0, 0.0, 0.0 );

    convertRegionBoundingVolumes( *tile, identity(), identity() );

    CHECK( tile->boundingVolume.has_value() );
    CHECK( distance( centreOf( *tile->boundingVolume ), expectedCentre ) < kTolerance );
}

TEST_CASE( "the chain accumulates through children" )
{
    auto root = makeRegionTile();
    root->transform = translation( 10.0, 0.0, 0.0 );

    auto child = makeRegionTile();
    child->transform = translation( 0.0, 20.0, 0.0 );

    const Vec3 expectedRoot = regionToEcefObb( kTestRegion ).center - Vec3( 10.0, 0.0, 0.0 );
    const Vec3 expectedChild =
        regionToEcefObb( kTestRegion ).center - Vec3( 10.0, 20.0, 0.0 );

    root->children.push_back( std::move( child ) );

    convertRegionBoundingVolumes( *root, identity(), identity() );

    CHECK( root->boundingVolume.has_value() );
    CHECK( distance( centreOf( *root->boundingVolume ), expectedRoot ) < kTolerance );

    CHECK( root->children.size() == 1 );
    const Tile &convertedChild = *root->children[0];
    CHECK( convertedChild.boundingVolume.has_value() );
    CHECK( distance( centreOf( *convertedChild.boundingVolume ), expectedChild ) < kTolerance );
}

TEST_CASE( "content bounding volumes are converted as well" )
{
    std::unique_ptr<Tile> tile = makeRegionTile();

    TileContent content;
    content.uri = "content.b3dm";
    content.boundingVolume = BoundingVolume::fromRegion( kTestRegion );
    tile->content = std::move( content );

    convertRegionBoundingVolumes( *tile, identity(), identity() );

    CHECK( tile->content.has_value() );
    CHECK( tile->content->boundingVolume.has_value() );
    CHECK( tile->content->boundingVolume->type == BoundingVolume::Type::Box );

    const OrientedBoundingBox expected = regionToEcefObb( kTestRegion );
    expectBoxMatches( *tile->content->boundingVolume, expected.center, expected.halfAxes );
}

TEST_CASE( "box and sphere volumes are left untouched" )
{
    auto boxTile = std::make_unique<Tile>();
    boxTile->boundingVolume = BoundingVolume::fromBox( Vec3( 1.0, 2.0, 3.0 ), Vec3( 10.0, 0.0, 0.0 ),
                                                       Vec3( 0.0, 10.0, 0.0 ), Vec3( 0.0, 0.0, 10.0 ) );
    const std::array<double, 12> originalBox = boxTile->boundingVolume->data;

    auto sphereTile = std::make_unique<Tile>();
    sphereTile->boundingVolume = BoundingVolume::fromSphere( Vec3( 4.0, 5.0, 6.0 ), 7.5 );
    const std::array<double, 12> originalSphere = sphereTile->boundingVolume->data;

    convertRegionBoundingVolumes( *boxTile, translation( 1000.0, 0.0, 0.0 ), identity() );
    convertRegionBoundingVolumes( *sphereTile, translation( 1000.0, 0.0, 0.0 ), identity() );

    CHECK( boxTile->boundingVolume->type == BoundingVolume::Type::Box );
    CHECK( boxTile->boundingVolume->data == originalBox );

    CHECK( sphereTile->boundingVolume->type == BoundingVolume::Type::Sphere );
    CHECK( sphereTile->boundingVolume->data == originalSphere );
}

TEST_CASE( "the memoised bounding volume radius is invalidated by conversion" )
{
    // Regression guard: a Tile caches its radius because projecting eight region corners is
    // expensive. Converting the volume without dropping that cache leaves a stale radius
    // that silently skews culling and request culling.
    std::unique_ptr<Tile> tile = makeRegionTile();

    const double regionRadius = tile->boundingVolumeRadius();
    CHECK( regionRadius > 0.0 );

    convertRegionBoundingVolumes( *tile, identity(), identity() );

    const double boxRadius = tile->boundingVolumeRadius();
    const double expectedBoxRadius = boundingVolumeRadius( *tile->boundingVolume );

    CHECK( boxRadius == doctest::Approx( expectedBoxRadius ).epsilon( 1e-12 ) );

    // The two representations genuinely differ for this region, so the assertion above is
    // not vacuous.
    CHECK( std::abs( boxRadius - regionRadius ) > 1e-6 );
}

TEST_CASE( "a tile without a bounding volume is tolerated" )
{
    auto tile = std::make_unique<Tile>();
    CHECK_FALSE( tile->boundingVolume.has_value() );
    CHECK( tile->boundingVolumeRadius() == 0.0 );

    convertRegionBoundingVolumes( *tile, identity(), identity() );

    CHECK_FALSE( tile->boundingVolume.has_value() );
}

TEST_CASE( "contentStateName covers every state" )
{
    CHECK( std::string( Tile::contentStateName( ContentState::Unloaded ) ) == "unloaded" );
    CHECK( std::string( Tile::contentStateName( ContentState::Loading ) ) == "loading" );
    CHECK( std::string( Tile::contentStateName( ContentState::Processing ) ) == "processing" );
    CHECK( std::string( Tile::contentStateName( ContentState::Ready ) ) == "ready" );
    CHECK( std::string( Tile::contentStateName( ContentState::Expired ) ) == "expired" );
    CHECK( std::string( Tile::contentStateName( ContentState::Failed ) ) == "failed" );
}
