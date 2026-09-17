// SPDX-License-Identifier: Unlicense
//
// Port of the computeScreenSpaceError / computeSurfaceDistance / fog blocks of
// __tests__/math.spec.ts, plus coverage for the oriented-box distance branch of
// computeBvSurfaceDistance (which the reference exercises in scheduling.spec.ts).

#include <doctest/doctest.h>

#include "math/Mat4.h"
#include "math/ScreenSpaceError.h"

#include <cmath>

using namespace tiles3d::math;

namespace
{
    /// Axis-aligned unit cube of half extent 10, centred on the local origin.
    BoundingVolume unitBox()
    {
        return BoundingVolume::fromBox( Vec3( 0.0 ), Vec3( 10.0, 0.0, 0.0 ),
                                        Vec3( 0.0, 10.0, 0.0 ), Vec3( 0.0, 0.0, 10.0 ) );
    }

    /// Translation by (x, 0, 0).
    Mat4 translationX( double x )
    {
        Mat4 m = identity();
        m[3][0] = x;
        return m;
    }
} // namespace

TEST_CASE( "computeScreenSpaceError follows GE * height / (distance * 2 tan(fov/2))" )
{
    const double geometricError = 16.0;
    const double distance = 1000.0;
    const double viewportHeight = 800.0;
    const double fovDegrees = 45.0;

    const double expected = ( geometricError * viewportHeight ) /
                            ( distance * 2.0 * std::tan( ( fovDegrees * kPi / 180.0 ) / 2.0 ) );

    CHECK( computeScreenSpaceError( geometricError, distance, viewportHeight, fovDegrees ) ==
           doctest::Approx( expected ).epsilon( 1e-12 ) );
}

TEST_CASE( "computeScreenSpaceError does not divide by zero at distance 0" )
{
    const double sse = computeScreenSpaceError( 16.0, 0.0, 800.0, 45.0 );
    CHECK( std::isfinite( sse ) );
    CHECK( sse > 0.0 );
}

TEST_CASE( "computeScreenSpaceError scales with GE and inversely with distance" )
{
    const double base = computeScreenSpaceError( 16.0, 1000.0, 800.0, 45.0 );
    const double doubledError = computeScreenSpaceError( 32.0, 1000.0, 800.0, 45.0 );
    const double doubledDistance = computeScreenSpaceError( 16.0, 2000.0, 800.0, 45.0 );

    CHECK( doubledError == doctest::Approx( base * 2.0 ).epsilon( 1e-12 ) );
    CHECK( doubledDistance == doctest::Approx( base / 2.0 ).epsilon( 1e-12 ) );
}

TEST_CASE( "computeSurfaceDistance clamps at zero inside the volume" )
{
    CHECK( computeSurfaceDistance( 500.0, 100.0 ) == 400.0 );
    CHECK( computeSurfaceDistance( 100.0, 100.0 ) == 0.0 );
    CHECK( computeSurfaceDistance( 50.0, 100.0 ) == 0.0 );
}

TEST_CASE( "fog matches 1 - exp(-(distance * density)^2)" )
{
    CHECK( fog( 0.0, 0.001 ) == doctest::Approx( 0.0 ).epsilon( 1e-12 ) );
    CHECK( fog( 1000.0, 0.001 ) == doctest::Approx( 1.0 - std::exp( -1.0 ) ).epsilon( 1e-12 ) );
    CHECK( fog( 100000.0, 0.001 ) == doctest::Approx( 1.0 ).epsilon( 1e-9 ) );

    // Higher density fogs up sooner.
    CHECK( fog( 1000.0, 0.002 ) > fog( 1000.0, 0.001 ) );
}

TEST_CASE( "orientedBoxDistanceToPoint clamps then measures" )
{
    OrientedBoundingBox box;
    box.center = Vec3( 0.0 );
    box.halfAxes[0] = Vec3( 10.0, 0.0, 0.0 );
    box.halfAxes[1] = Vec3( 0.0, 10.0, 0.0 );
    box.halfAxes[2] = Vec3( 0.0, 0.0, 10.0 );

    SUBCASE( "a point inside the box is at distance 0" )
    {
        CHECK( orientedBoxDistanceToPoint( box, Vec3( 5.0, -5.0, 5.0 ) ) == 0.0 );
        CHECK( orientedBoxDistanceToPoint( box, box.center ) == 0.0 );
    }

    SUBCASE( "a point outside along one axis measures the overshoot" )
    {
        CHECK( orientedBoxDistanceToPoint( box, Vec3( 30.0, 0.0, 0.0 ) ) ==
               doctest::Approx( 20.0 ) );
    }

    SUBCASE( "a point outside diagonally measures the euclidean overshoot" )
    {
        const double expected = std::sqrt( 20.0 * 20.0 + 20.0 * 20.0 );
        CHECK( orientedBoxDistanceToPoint( box, Vec3( 30.0, 30.0, 0.0 ) ) ==
               doctest::Approx( expected ) );
    }
}

TEST_CASE( "computeBvSurfaceDistance falls back to the sphere distance where appropriate" )
{
    const Mat4 identityWorld = identity();
    const Vec3 camera( 0.0, 0.0, 50.0 );

    SUBCASE( "a null volume passes the sphere distance through" )
    {
        CHECK( computeBvSurfaceDistance( nullptr, identityWorld, camera, 123.0 ) == 123.0 );
    }

    SUBCASE( "a sphere volume passes the sphere distance through" )
    {
        const BoundingVolume sphere = BoundingVolume::fromSphere( Vec3( 0.0 ), 10.0 );
        CHECK( computeBvSurfaceDistance( &sphere, identityWorld, camera, 123.0 ) == 123.0 );
    }

    SUBCASE( "a region volume passes the sphere distance through" )
    {
        const BoundingVolume region =
            BoundingVolume::fromRegion( Region{ 0.0, 0.0, 0.1, 0.1, 0.0, 100.0 } );
        CHECK( computeBvSurfaceDistance( &region, identityWorld, camera, 123.0 ) == 123.0 );
    }

    SUBCASE( "a box uses the true oriented box distance" )
    {
        const BoundingVolume box = unitBox();
        // Identity world matrix: the camera is already in the box's frame.
        CHECK( computeBvSurfaceDistance( &box, identityWorld, Vec3( 30.0, 0.0, 0.0 ), 123.0 ) ==
               doctest::Approx( 20.0 ) );
    }

    SUBCASE( "the world matrix brings the camera into the box frame" )
    {
        const BoundingVolume box = unitBox();
        // Box translated 100 along x in world space; camera 50 above its centre in world
        // space is therefore 50 above the local centre.
        const Mat4 world = translationX( 100.0 );

        CHECK( computeBvSurfaceDistance( &box, world, Vec3( 100.0, 50.0, 0.0 ), 123.0 ) ==
               doctest::Approx( 40.0 ) );
    }
}
