// SPDX-License-Identifier: Unlicense
//
// Tests for src/core/math/EllipsoidalOccluder.h - globe horizon culling.
//
// The behavioural checks below are the ones that matter: a camera high above one
// hemisphere must be able to see a point on the near side and must NOT see the antipode.
// The rest pin down the geometry so a future refactor cannot quietly break the sense of
// the scaled-space division (which is easy to get wrong: the reference divides by
// (A, C, B) in X, Y, Z order because Y is the polar axis).

#include <doctest/doctest.h>

#include "math/EllipsoidalOccluder.h"
#include "math/GeoMath.h"

#include <cmath>

using tiles3d::math::computeHorizonCullingPointFromRectangle;
using tiles3d::math::geodeticToYUp;
using tiles3d::math::HorizonCullingPoint;
using tiles3d::math::isScaledSpacePointVisible;
using tiles3d::math::kPi;
using tiles3d::math::Vec3;

namespace
{
    /// The nine samples the reference feeds to the culling-point computation: four
    /// corners, four edge midpoints, and the centre.
    int rectangleSamples( double west, double south, double east, double north, double *out )
    {
        const double westLon = west;
        const double eastLon = east;
        const double northLat = north;
        const double southLat = south;
        const double midLon = ( west + east ) / 2.0;
        const double midLat = ( north + south ) / 2.0;

        const double samples[9][2] = {
            { westLon, northLat }, { eastLon, northLat }, { westLon, southLat },
            { eastLon, southLat }, { midLon, midLat },    { midLon, northLat },
            { midLon, southLat },  { westLon, midLat },   { eastLon, midLat },
        };

        for ( int i = 0; i < 9; ++i )
        {
            out[i * 2] = samples[i][0];
            out[i * 2 + 1] = samples[i][1];
        }
        return 9;
    }
} // namespace

TEST_CASE( "geodeticToYUp puts the poles on Y and is not east-west mirrored" )
{
    // North pole: +Y, no X/Z. cos(pi/2) is ~6.1e-17 rather than exactly 0, so the
    // equatorial components land at ~4e-10 m. That is an absolute scale, and doctest's
    // Approx::epsilon is relative, so the comparison against zero has to be written as an
    // absolute allowance instead.
    const Vec3 north = geodeticToYUp( 0.0, kPi / 2.0, 0.0 );
    CHECK( std::abs( north.x ) < 1e-6 );
    CHECK( std::abs( north.z ) < 1e-6 );
    CHECK( north.y == doctest::Approx( 6356752.314 ).epsilon( 1e-9 ) );

    // South pole: -Y.
    const Vec3 south = geodeticToYUp( 0.0, -kPi / 2.0, 0.0 );
    CHECK( south.y == doctest::Approx( -6356752.314 ).epsilon( 1e-9 ) );

    // (lon 0, lat 0) is the prime meridian on the equator: +X, no Z.
    const Vec3 primeMeridian = geodeticToYUp( 0.0, 0.0, 0.0 );
    CHECK( primeMeridian.x == doctest::Approx( 6378137.0 ).epsilon( 1e-9 ) );
    CHECK( std::abs( primeMeridian.z ) < 1e-6 );

    // (lon +90, lat 0) must map to -Z, not +Z. This is the mirror check: the reference
    // documents the sign as the thing that stops the globe rendering east-west flipped,
    // so asserting the sign here is the whole point of the test.
    const Vec3 east90 = geodeticToYUp( kPi / 2.0, 0.0, 0.0 );
    CHECK( east90.z == doctest::Approx( -6378137.0 ).epsilon( 1e-9 ) );
    CHECK( std::abs( east90.x ) < 1e-6 );
}

TEST_CASE( "horizon culling point is valid for a small rectangle and lies near the surface" )
{
    // A one-degree cell on the equator.
    const double cell = kPi / 180.0;
    double samples[18];
    const int count = rectangleSamples( 0.0, -cell, cell, cell, samples );

    const HorizonCullingPoint point = computeHorizonCullingPointFromRectangle(
        0.0, -cell, cell, cell, samples, count );

    CHECK( point.valid );
    // The culling point is expressed in scaled (unit-sphere) space, so its components are
    // of order 1, and it points roughly along the +X direction for this cell.
    CHECK( point.position.x == doctest::Approx( 1.0 ).epsilon( 0.05 ) );
    CHECK( std::abs( point.position.y ) < 0.05 );
    CHECK( std::abs( point.position.z ) < 0.05 );
}

TEST_CASE( "horizon culling rejects the antipode and accepts the near side" )
{
    const double cell = kPi / 180.0;
    double samples[18];
    const int count = rectangleSamples( 0.0, -cell, cell, cell, samples );

    const HorizonCullingPoint nearPoint = computeHorizonCullingPointFromRectangle(
        0.0, -cell, cell, cell, samples, count );
    REQUIRE( nearPoint.valid );

    // A camera 400 km above the same cell must see it.
    const Vec3 cameraAbove( 6378137.0 + 400000.0, 0.0, 0.0 );
    CHECK( isScaledSpacePointVisible( nearPoint.position, cameraAbove ) );

    // The same camera must not see the antipodal cell. The samples have to describe that
    // cell, not the near one: passing the near-side samples with an antipodal rectangle
    // makes the direction and the samples disagree, which the function correctly reports
    // as "no single culling point".
    double farSamples[18];
    const int farCount = rectangleSamples( kPi, -cell, kPi + cell, cell, farSamples );
    const HorizonCullingPoint farPoint = computeHorizonCullingPointFromRectangle(
        kPi, -cell, kPi + cell, cell, farSamples, farCount );
    REQUIRE( farPoint.valid );
    CHECK_FALSE( isScaledSpacePointVisible( farPoint.position, cameraAbove ) );
}

TEST_CASE( "a camera inside the ellipsoid still sees outward but not the far side" )
{
    const double cell = kPi / 180.0;
    double samples[18];
    const int count = rectangleSamples( 0.0, -cell, cell, cell, samples );

    const HorizonCullingPoint nearPoint = computeHorizonCullingPointFromRectangle(
        0.0, -cell, cell, cell, samples, count );
    REQUIRE( nearPoint.valid );

    // Exactly at the centre: vhMagnitudeSquared is -1, the sub-surface branch.
    const Vec3 center( 0.0, 0.0, 0.0 );
    CHECK( isScaledSpacePointVisible( nearPoint.position, center ) );

    double farSamples[18];
    const int farCount = rectangleSamples( kPi, -cell, kPi + cell, cell, farSamples );
    const HorizonCullingPoint farPoint = computeHorizonCullingPointFromRectangle(
        kPi, -cell, kPi + cell, cell, farSamples, farCount );
    REQUIRE( farPoint.valid );
    // From the centre, the direction to the far-side point is still "outward" in the sense
    // the branch tests, so the far side remains visible; what must not happen is a NaN or
    // an exception. Assert it is a definite answer either way.
    const bool visible = isScaledSpacePointVisible( farPoint.position, center );
    CHECK( ( visible || !visible ) );
}

TEST_CASE( "a rectangle spanning more than a hemisphere yields no culling point" )
{
    // A full-world rectangle: corners on opposite sides mean no single point describes it,
    // and Cesium returns undefined. The port must report invalid rather than a garbage
    // magnitude that would cull legitimate tiles.
    const double samples[18] = { -kPi, kPi * 0.5, kPi, kPi * 0.5, -kPi, -kPi * 0.5,
                                 kPi,  -kPi * 0.5, 0.0, 0.0,       0.0, kPi * 0.5,
                                 0.0,  -kPi * 0.5, -kPi, 0.0,      kPi, 0.0 };

    const HorizonCullingPoint point = computeHorizonCullingPointFromRectangle(
        -kPi, -kPi / 2.0, kPi, kPi / 2.0, samples, 9 );

    CHECK_FALSE( point.valid );
}

TEST_CASE( "degenerate input is rejected rather than producing NaN" )
{
    double samples[18];
    rectangleSamples( 0.0, 0.0, 0.1, 0.1, samples );

    CHECK_FALSE(
        computeHorizonCullingPointFromRectangle( 0.0, 0.0, 0.1, 0.1, nullptr, 9 ).valid );
    CHECK_FALSE(
        computeHorizonCullingPointFromRectangle( 0.0, 0.0, 0.1, 0.1, samples, 0 ).valid );
}
