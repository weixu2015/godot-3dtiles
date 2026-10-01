// SPDX-License-Identifier: Unlicense
//
// Tests for src/core/math/TileScheme.h - the ECT tiling scheme for the globe quadtree.
//
// The expected values are derived from the reference implementation
// (globe/renderers/globe3d/tileScheme.ts) rather than from Cesium, because the reference
// is the authority this port follows. Where the reference's arithmetic is exact, the
// assertions are exact; where a trig identity is involved, they use a relative epsilon.

#include <doctest/doctest.h>

#include "math/GeoMath.h"
#include "math/TileScheme.h"

#include <cmath>
#include <string>

using tiles3d::math::kPi;
using tiles3d::math::latitudeFromMercatorY;
using tiles3d::math::levelGeometricError;
using tiles3d::math::levelZeroGeometricError;
using tiles3d::math::mercatorY;
using tiles3d::math::tileGeometryRectangle;
using tiles3d::math::tileXYToQuadKey;
using tiles3d::math::tileXYToRectangle;

TEST_CASE( "latitudeFromMercatorY matches the reference mapping" )
{
    // t = 0.5 is the prime meridian row centre: atan(sinh(0)) == 0.
    CHECK( latitudeFromMercatorY( 0.5 ) == doctest::Approx( 0.0 ).epsilon( 1e-12 ) );

    // t = 0 is the north edge of the Web Mercator domain: atan(sinh(pi)) ~= 85.0511 deg.
    const double maxLat = latitudeFromMercatorY( 0.0 );
    CHECK( maxLat == doctest::Approx( 1.4844222297453324 ).epsilon( 1e-12 ) );

    // The mapping is odd about t = 0.5.
    CHECK( latitudeFromMercatorY( 0.0 ) ==
           doctest::Approx( -latitudeFromMercatorY( 1.0 ) ).epsilon( 1e-12 ) );
}

TEST_CASE( "tileXYToRectangle level 0 covers the full Mercator domain" )
{
    const auto rect = tileXYToRectangle( 0, 0, 0 );

    CHECK( rect.west == doctest::Approx( -kPi ).epsilon( 1e-12 ) );
    CHECK( rect.east == doctest::Approx( kPi ).epsilon( 1e-12 ) );
    CHECK( rect.north == doctest::Approx( 1.4844222297453324 ).epsilon( 1e-12 ) );
    CHECK( rect.south == doctest::Approx( -1.4844222297453324 ).epsilon( 1e-12 ) );
}

TEST_CASE( "tileXYToRectangle level 1 splits longitude evenly and latitude by Mercator" )
{
    // Longitude is equidistant, so a level 1 tile spans exactly half the world.
    const auto west = tileXYToRectangle( 0, 0, 1 );
    CHECK( west.west == doctest::Approx( -kPi ).epsilon( 1e-12 ) );
    CHECK( west.east == doctest::Approx( 0.0 ).epsilon( 1e-12 ) );

    const auto east = tileXYToRectangle( 1, 0, 1 );
    CHECK( east.west == doctest::Approx( 0.0 ).epsilon( 1e-12 ) );
    CHECK( east.east == doctest::Approx( kPi ).epsilon( 1e-12 ) );

    // The northern row of level 1 ends at the equator, not at lat 0 by coincidence: it is
    // latitudeFromMercatorY(1/2).
    CHECK( west.south == doctest::Approx( 0.0 ).epsilon( 1e-12 ) );

    // Rows tile without gaps or overlaps.
    const auto south = tileXYToRectangle( 0, 1, 1 );
    CHECK( south.north == doctest::Approx( west.south ).epsilon( 1e-12 ) );
}

TEST_CASE( "tileGeometryRectangle stretches only the polar rows" )
{
    // Level 2 has 4 rows: y = 0 (north), 1, 2, 3 (south).
    const auto northRow = tileGeometryRectangle( 1, 0, 2 );
    const auto northRowImagery = tileXYToRectangle( 1, 0, 2 );
    CHECK( northRow.north == doctest::Approx( kPi / 2.0 ).epsilon( 1e-12 ) );
    CHECK( northRowImagery.north < kPi / 2.0 );
    // Its southern edge is untouched.
    CHECK( northRow.south == doctest::Approx( northRowImagery.south ).epsilon( 1e-12 ) );

    const auto southRow = tileGeometryRectangle( 1, 3, 2 );
    CHECK( southRow.south == doctest::Approx( -kPi / 2.0 ).epsilon( 1e-12 ) );

    // An interior row is identical to its imagery rectangle: no polar stretch applies.
    const auto interior = tileGeometryRectangle( 1, 1, 2 );
    const auto interiorImagery = tileXYToRectangle( 1, 1, 2 );
    CHECK( interior.north == doctest::Approx( interiorImagery.north ).epsilon( 1e-12 ) );
    CHECK( interior.south == doctest::Approx( interiorImagery.south ).epsilon( 1e-12 ) );
}

TEST_CASE( "tileXYToQuadKey builds the standard quadtree key" )
{
    char buffer[8];

    // The reference's own worked example shape: level 1 is a single digit.
    CHECK( tileXYToQuadKey( 0, 0, 1, buffer, sizeof( buffer ) ) == 1 );
    CHECK( std::string( buffer ) == "0" );

    CHECK( tileXYToQuadKey( 1, 0, 1, buffer, sizeof( buffer ) ) == 1 );
    CHECK( std::string( buffer ) == "1" );

    CHECK( tileXYToQuadKey( 0, 1, 1, buffer, sizeof( buffer ) ) == 1 );
    CHECK( std::string( buffer ) == "2" );

    CHECK( tileXYToQuadKey( 1, 1, 1, buffer, sizeof( buffer ) ) == 1 );
    CHECK( std::string( buffer ) == "3" );

    // Level 3, tile (3, 5) = (0b011, 0b101). The reference emits the most significant bit
    // first, so the digits are MSB-to-LSB: bit 2 gives (x=0, y=1) -> 2, bit 1 gives
    // (x=1, y=0) -> 1, bit 0 gives (x=1, y=1) -> 3. Hence "213".
    CHECK( tileXYToQuadKey( 3, 5, 3, buffer, sizeof( buffer ) ) == 3 );
    CHECK( std::string( buffer ) == "213" );

    // A too-small buffer is reported rather than overrun.
    CHECK( tileXYToQuadKey( 3, 5, 3, buffer, 3 ) == 0 );
    CHECK( tileXYToQuadKey( 3, 5, 3, nullptr, 8 ) == 0 );
}

TEST_CASE( "mercatorY is the inverse of the row mapping" )
{
    // mercY and latitudeFromMercatorY describe the same relationship from both ends, so
    // composing them must return the input.
    for ( double t : { 0.05, 0.25, 0.5, 0.75, 0.95 } )
    {
        const double latitude = latitudeFromMercatorY( t );
        const double y = mercatorY( latitude );
        // y is the mercator coordinate; recover t from it and compare.
        const double recoveredT = 0.5 - y / ( 2.0 * kPi );
        CHECK( recoveredT == doctest::Approx( t ).epsilon( 1e-12 ) );
    }
}

TEST_CASE( "geometric error halves per level" )
{
    const double levelZero = levelZeroGeometricError();

    // Reference: ((max(A, C) * 2*pi) / 4) / 65. Here A == B > C, so it is A that wins.
    const double expected = ( ( 6378137.0 * 2.0 * kPi ) / 4.0 ) / 65.0;
    CHECK( levelZero == doctest::Approx( expected ).epsilon( 1e-12 ) );

    CHECK( levelGeometricError( 0 ) == doctest::Approx( levelZero ).epsilon( 1e-12 ) );
    CHECK( levelGeometricError( 1 ) == doctest::Approx( levelZero / 2.0 ).epsilon( 1e-12 ) );
    CHECK( levelGeometricError( 5 ) == doctest::Approx( levelZero / 32.0 ).epsilon( 1e-12 ) );
}
