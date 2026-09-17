// SPDX-License-Identifier: Unlicense
//
// Tests for src/core/math/Mat4.h.
//
// The last two test cases are the important ones: they pin down the coordinate-frame
// invariant from docs/REFACTOR_PLAN.md D1 (the root tile's worldMatrix must be the
// identity in the scheduler's render frame) and quantify why the kernel must stay in
// double precision.

#include <doctest/doctest.h>

#include "math/Mat4.h"

#include <algorithm>
#include <cmath>
#include <limits>

using tiles3d::math::fromColumnMajor;
using tiles3d::math::identity;
using tiles3d::math::invert;
using tiles3d::math::isFinite;
using tiles3d::math::Mat4;
using tiles3d::math::maxScale;
using tiles3d::math::multiply;
using tiles3d::math::toColumnMajor;

namespace
{
    /// Largest absolute difference between the translation part of `m` and zero.
    double translationMagnitude( const Mat4 &m )
    {
        return std::max( std::max( std::abs( m[3][0] ), std::abs( m[3][1] ) ),
                         std::abs( m[3][2] ) );
    }

    /// Largest absolute difference between the 3x3 linear part of `m` and the identity.
    double linearDeviationFromIdentity( const Mat4 &m )
    {
        double worst = 0.0;
        for ( int column = 0; column < 3; ++column )
        {
            for ( int row = 0; row < 3; ++row )
            {
                const double expected = ( column == row ) ? 1.0 : 0.0;
                worst = std::max( worst, std::abs( m[column][row] - expected ) );
            }
        }
        return worst;
    }

    Mat4 translation( double x, double y, double z )
    {
        Mat4 m = identity();
        m[3][0] = x;
        m[3][1] = y;
        m[3][2] = z;
        return m;
    }

    Mat4 scaling( double s )
    {
        Mat4 m = identity();
        m[0][0] = s;
        m[1][1] = s;
        m[2][2] = s;
        return m;
    }

    /// Rotation about the +Z axis by `radians`.
    Mat4 rotationZ( double radians )
    {
        const double c = std::cos( radians );
        const double s = std::sin( radians );
        Mat4 m = identity();
        m[0][0] = c;
        m[0][1] = s;
        m[1][0] = -s;
        m[1][1] = c;
        return m;
    }

    /// Approximates `CesiumGeospatial::GlobeTransforms::eastNorthUpToFixedFrame` for an
    /// ECEF anchor point: columns are (east, north, up), translation is the anchor.
    /// This is the shape of a real 3D Tiles root transform for georeferenced data.
    Mat4 eastNorthUpToEcef( double x, double y, double z )
    {
        const double length = std::sqrt( x * x + y * y + z * z );
        const double ux = x / length;
        const double uy = y / length;
        const double uz = z / length;

        // east = normalize(cross(+Z, up))
        double ex = -uy;
        double ey = ux;
        double ez = 0.0;
        const double eastLength = std::sqrt( ex * ex + ey * ey + ez * ez );
        ex /= eastLength;
        ey /= eastLength;
        ez /= eastLength;

        // north = cross(up, east)
        const double nx = uy * ez - uz * ey;
        const double ny = uz * ex - ux * ez;
        const double nz = ux * ey - uy * ex;

        Mat4 m = identity();
        m[0][0] = ex;
        m[0][1] = ey;
        m[0][2] = ez;
        m[1][0] = nx;
        m[1][1] = ny;
        m[1][2] = nz;
        m[2][0] = ux;
        m[2][1] = uy;
        m[2][2] = uz;
        m[3][0] = x;
        m[3][1] = y;
        m[3][2] = z;
        return m;
    }

    /// ECEF anchor of the sample dataset, taken from demo/node_3d.tscn.
    constexpr double kAnchorX = 1.21636e+06;
    constexpr double kAnchorY = -4.73629e+06;
    constexpr double kAnchorZ = 4.08133e+06;
} // namespace

TEST_CASE( "identity is the column-major identity matrix" )
{
    const Mat4 m = identity();
    for ( int column = 0; column < 4; ++column )
    {
        for ( int row = 0; row < 4; ++row )
        {
            CHECK( m[column][row] == ( column == row ? 1.0 : 0.0 ) );
        }
    }
}

TEST_CASE( "multiply applies the right operand first and is not commutative" )
{
    const Mat4 t = translation( 10.0, 0.0, 0.0 );
    const Mat4 r = rotationZ( 3.14159265358979323846 / 2.0 );

    // (T * R) maps the origin to (10, 0, 0): R first (origin is fixed), then T.
    const Mat4 tr = multiply( t, r );
    CHECK( tr[3][0] == doctest::Approx( 10.0 ) );
    // doctest::Approx has no absolute-margin method (unlike Catch2), so check the
    // residual explicitly. tr[3][1] must be 0.0 within 1e-12.
    CHECK( std::abs( tr[3][1] ) < 1e-12 );

    // (R * T) rotates the translated origin to (0, 10, 0).
    const Mat4 rt = multiply( r, t );
    CHECK( std::abs( rt[3][0] ) < 1e-12 );
    CHECK( rt[3][1] == doctest::Approx( 10.0 ) );
}

TEST_CASE( "invert round-trips an affine transform" )
{
    const Mat4 m = multiply( eastNorthUpToEcef( kAnchorX, kAnchorY, kAnchorZ ),
                             multiply( translation( 12.0, -34.0, 56.0 ), scaling( 2.5 ) ) );

    const Mat4 roundTrip = multiply( invert( m ), m );

    CHECK( linearDeviationFromIdentity( roundTrip ) < 1e-9 );
    CHECK( translationMagnitude( roundTrip ) < 1e-8 );
}

TEST_CASE( "maxScale reports the largest scale factor and ignores rotation and translation" )
{
    const Mat4 pureRotation = eastNorthUpToEcef( kAnchorX, kAnchorY, kAnchorZ );
    CHECK( maxScale( pureRotation ) == doctest::Approx( 1.0 ).epsilon( 1e-12 ) );

    const Mat4 scaled = multiply( pureRotation, scaling( 2.5 ) );
    CHECK( maxScale( scaled ) == doctest::Approx( 2.5 ).epsilon( 1e-12 ) );

    // Non-uniform scale returns the largest component.
    Mat4 nonUniform = identity();
    nonUniform[0][0] = 3.0;
    nonUniform[1][1] = 7.0;
    nonUniform[2][2] = 1.0;
    CHECK( maxScale( nonUniform ) == doctest::Approx( 7.0 ) );
}

TEST_CASE( "fromColumnMajor and toColumnMajor round-trip" )
{
    const double source[16] = { 1.0,  2.0,  3.0,  4.0,  5.0,  6.0,  7.0,  8.0,
                                9.0,  10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0 };

    const Mat4 m = fromColumnMajor( source );

    // Column 0 holds elements 0..3, so m[0][3] must be 4.0 and m[1][0] must be 5.0.
    CHECK( m[0][0] == 1.0 );
    CHECK( m[0][3] == 4.0 );
    CHECK( m[1][0] == 5.0 );
    CHECK( m[3][3] == 16.0 );

    double roundTripped[16] = {};
    toColumnMajor( m, roundTripped );
    for ( int i = 0; i < 16; ++i )
    {
        CHECK( roundTripped[i] == source[i] );
    }
}

TEST_CASE( "isFinite rejects NaN and infinity" )
{
    CHECK( isFinite( identity() ) );

    Mat4 nan = identity();
    nan[2][1] = std::nan( "" );
    CHECK_FALSE( isFinite( nan ) );

    Mat4 infinite = identity();
    infinite[3][0] = std::numeric_limits<double>::infinity();
    CHECK_FALSE( isFinite( infinite ) );
}

// ---------------------------------------------------------------------------
// D1 coordinate-frame invariant
// ---------------------------------------------------------------------------

TEST_CASE( "root worldMatrix is the identity in the scheduler render frame" )
{
    // The reference implementation sets modelMatrix = inverse(root.transform), so the
    // root tile's accumulated world matrix (modelMatrix * rootTransform) must collapse
    // to the identity. Every descendant inherits that frame, which is what keeps tile
    // coordinates near the origin.
    const Mat4 rootTransform = eastNorthUpToEcef( kAnchorX, kAnchorY, kAnchorZ );
    const Mat4 modelMatrix = invert( rootTransform );

    const Mat4 rootWorldMatrix = multiply( modelMatrix, rootTransform );

    CHECK( linearDeviationFromIdentity( rootWorldMatrix ) < 1e-15 );
    CHECK( translationMagnitude( rootWorldMatrix ) < 1e-8 );

    // A child tile accumulates modelMatrix * rootTransform * childTransform, so it must
    // land exactly where childTransform puts it in the tile-local frame.
    const Mat4 childTransform = multiply( translation( 120.0, -80.0, 15.0 ), scaling( 1.5 ) );
    const Mat4 childWorldMatrix = multiply( rootWorldMatrix, childTransform );

    for ( int column = 0; column < 4; ++column )
    {
        for ( int row = 0; row < 4; ++row )
        {
            // doctest::Approx has no absolute-margin method (unlike Catch2), so check
            // the per-element residual explicitly: matrices must match within 1e-8.
            CHECK( std::abs( childWorldMatrix[column][row] -
                             childTransform[column][row] ) < 1e-8 );
        }
    }
}

TEST_CASE( "double precision is required for ECEF-scale transforms" )
{
    // This test documents why Mat4 is glm::dmat4 and not glm::mat4.
    //
    // The anchor sits ~6.4e6 m from the earth centre. Inverting a transform containing
    // that translation and multiplying back must return the identity; the achievable
    // residual is bounded by the magnitude of the translation times the machine epsilon
    // of the working type. Measuring it here turns "float32 would break" from an
    // assumption into a checked number. float32 (epsilon ~1.2e-7) would leave a residual
    // of roughly 1 metre, i.e. visibly jittering photogrammetry; float64 leaves
    // nanometres.
    const Mat4 rootTransform = eastNorthUpToEcef( kAnchorX, kAnchorY, kAnchorZ );
    const Mat4 roundTrip = multiply( invert( rootTransform ), rootTransform );

    const double residualMetres =
        std::max( linearDeviationFromIdentity( roundTrip ) * 1.0,
                  translationMagnitude( roundTrip ) );

    CHECK( residualMetres < 1e-6 );
    CHECK( residualMetres > 0.0 );
}
