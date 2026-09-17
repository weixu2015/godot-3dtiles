// SPDX-License-Identifier: Unlicense
//
// Port of the getLevelOffset / wgs84ToCartesian blocks of __tests__/math.spec.ts, plus
// invariants for regionToEcefObb that the reference covers in srs-bounding.spec.ts.

#include <doctest/doctest.h>

#include "math/GeoMath.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <cstdint>

using namespace tiles3d::math;

TEST_CASE( "levelOffset matches the reference closed form" )
{
    // Quadtree: (4^level - 1) / 3
    CHECK( levelOffset( 4, 0 ) == 0 );
    CHECK( levelOffset( 4, 1 ) == 1 );
    CHECK( levelOffset( 4, 2 ) == 5 );
    CHECK( levelOffset( 4, 3 ) == 21 );

    // Octree: (8^level - 1) / 7
    CHECK( levelOffset( 8, 0 ) == 0 );
    CHECK( levelOffset( 8, 1 ) == 1 );
    CHECK( levelOffset( 8, 2 ) == 9 );
    CHECK( levelOffset( 8, 3 ) == 73 );
}

TEST_CASE( "levelOffset agrees with the reference closed form at deeper levels" )
{
    // Guards the iterative integer evaluation against the reference's pow()-based formula.
    // This is where a rounding difference would surface first.
    for ( int level = 0; level <= 8; ++level )
    {
        const double quadtree = ( std::pow( 4.0, level ) - 1.0 ) / 3.0;
        const double octree = ( std::pow( 8.0, level ) - 1.0 ) / 7.0;
        CHECK( levelOffset( 4, level ) == static_cast<std::int64_t>( quadtree ) );
        CHECK( levelOffset( 8, level ) == static_cast<std::int64_t>( octree ) );
    }
}

TEST_CASE( "wgs84ToCartesian places known points correctly" )
{
    SUBCASE( "equator at longitude 0 is (a, 0, 0)" )
    {
        const Vec3 p = wgs84ToCartesian( 0.0, 0.0, 0.0 );
        CHECK( p.x == doctest::Approx( 6378137.0 ).epsilon( 1e-9 ) );
        // doctest::Approx only offers a relative epsilon (Catch2's margin() does not
        // exist here), so near-zero comparisons use an explicit absolute bound.
        CHECK( std::abs( p.y ) < 1e-6 );
        CHECK( std::abs( p.z ) < 1e-6 );
    }

    SUBCASE( "north pole sits at the semi-minor axis" )
    {
        const Vec3 p = wgs84ToCartesian( 0.0, kPi / 2.0, 0.0 );
        CHECK( std::abs( p.x ) < 1e-6 );
        CHECK( std::abs( p.y ) < 1e-6 );
        CHECK( p.z == doctest::Approx( 6356752.3142 ).epsilon( 1e-6 ) );
    }

    SUBCASE( "height moves the point along the surface normal" )
    {
        const Vec3 surface = wgs84ToCartesian( 0.3, 0.6, 0.0 );
        const Vec3 raised = wgs84ToCartesian( 0.3, 0.6, 1500.0 );
        const Vec3 normal = wgs84SurfaceNormal( 0.3, 0.6 );

        CHECK( glm::length( raised - surface ) == doctest::Approx( 1500.0 ).epsilon( 1e-9 ) );
        // The displacement must be parallel to the normal.
        CHECK( glm::length( glm::normalize( raised - surface ) - normal ) < 1e-12 );
    }
}

TEST_CASE( "normalizeSafe does not produce NaN for a zero vector" )
{
    const Vec3 zero = normalizeSafe( Vec3( 0.0 ) );
    CHECK( zero.x == 0.0 );
    CHECK( zero.y == 0.0 );
    CHECK( zero.z == 0.0 );

    const Vec3 scaled = normalizeSafe( Vec3( 0.0, 0.0, 5.0 ) );
    CHECK( scaled.z == doctest::Approx( 1.0 ) );
}

TEST_CASE( "regionToEcefObb builds an orthogonal box that contains the region" )
{
    // ~1.1 km patch, narrow in longitude so the tangent-plane branch is taken.
    const Region region{ 0.02, 0.50, 0.03, 0.51, 120.0, 260.0 };

    const OrientedBoundingBox box = regionToEcefObb( region );

    // Non-degenerate.
    for ( const Vec3 &axis : box.halfAxes )
    {
        CHECK( glm::length( axis ) > 0.0 );
        CHECK( std::isfinite( axis.x ) );
        CHECK( std::isfinite( axis.y ) );
        CHECK( std::isfinite( axis.z ) );
    }
    CHECK( std::isfinite( box.center.x ) );

    // The three axes must be mutually orthogonal: they are built from east / north / up
    // (narrow branch) or from the equatorial plane (wide branch).
    for ( int i = 0; i < 3; ++i )
    {
        for ( int j = i + 1; j < 3; ++j )
        {
            const double alignment = std::abs( glm::dot( glm::normalize( box.halfAxes[i] ),
                                                        glm::normalize( box.halfAxes[j] ) ) );
            CHECK( alignment < 1e-12 );
        }
    }

    // Every region corner must fall inside the box. This is the invariant that catches a
    // wrong branch, a swapped axis or a sign error.
    for ( const Vec3 &corner : regionCorners( region ) )
    {
        const Vec3 offset = corner - box.center;
        for ( int axis = 0; axis < 3; ++axis )
        {
            const Vec3 direction = glm::normalize( box.halfAxes[axis] );
            const double projected = std::abs( glm::dot( offset, direction ) );
            CHECK( projected <= glm::length( box.halfAxes[axis] ) + 1.0 );
        }
    }
}

TEST_CASE( "regionToEcefObb handles a region wider than pi" )
{
    // Width 4.0 rad > pi, so the equatorial-plane branch is taken.
    const Region region{ -2.0, -0.3, 2.0, 0.3, 0.0, 1000.0 };

    const OrientedBoundingBox box = regionToEcefObb( region );

    for ( const Vec3 &axis : box.halfAxes )
    {
        CHECK( std::isfinite( axis.x ) );
        CHECK( std::isfinite( axis.y ) );
        CHECK( std::isfinite( axis.z ) );
        CHECK( glm::length( axis ) > 0.0 );
    }
    CHECK( std::isfinite( box.center.x ) );
    CHECK( std::isfinite( box.center.y ) );
    CHECK( std::isfinite( box.center.z ) );

    for ( int i = 0; i < 3; ++i )
    {
        for ( int j = i + 1; j < 3; ++j )
        {
            const double alignment = std::abs( glm::dot( glm::normalize( box.halfAxes[i] ),
                                                        glm::normalize( box.halfAxes[j] ) ) );
            CHECK( alignment < 1e-12 );
        }
    }
}

TEST_CASE( "regionToEcefObb straddling the equator uses latitude 0 as plane centre" )
{
    // A 0.1 rad x 0.1 rad patch centred on the equator. One radian of arc is one Earth
    // radius, so each half extent is 0.05 rad * a ~= 318.9 km. (Sanity check: it is easy
    // to misread 0.05 rad as 0.05 degrees - that would be ~5.6 km, 57x too small.)
    const Region region{ -0.05, -0.05, 0.05, 0.05, -100.0, 100.0 };

    const OrientedBoundingBox box = regionToEcefObb( region );

    const double expectedHalfExtent = 0.05 * kWgs84SemiMajorAxis;

    CHECK( std::isfinite( box.center.z ) );
    CHECK( glm::length( box.halfAxes[0] ) ==
           doctest::Approx( expectedHalfExtent ).epsilon( 0.02 ) );
    CHECK( glm::length( box.halfAxes[1] ) ==
           doctest::Approx( expectedHalfExtent ).epsilon( 0.02 ) );

    // The z half extent is governed by the height band plus the ellipsoid drop across the
    // patch, so it must be far smaller than the horizontal extents. Over a 637 km span the
    // surface falls ~16 km, which dominates the 200 m height band - that asymmetry is what
    // makes the order of the half axes worth asserting.
    CHECK( glm::length( box.halfAxes[2] ) < glm::length( box.halfAxes[0] ) );
    CHECK( glm::length( box.halfAxes[2] ) < 20000.0 );
}
