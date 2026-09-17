// SPDX-License-Identifier: Unlicense
//
// Port of the getBoundingVolumeCenter / getBoundingVolumeRadius / subdivideBox blocks of
// __tests__/math.spec.ts. The expected values are the ones the reference asserts, so a
// divergence here means the port drifted.

#include <doctest/doctest.h>

#include "math/BoundingVolume.h"

#include <glm/geometric.hpp>

#include <cmath>

using namespace tiles3d::math;

namespace
{
    /// Same payload as the reference spec: centre (1,2,3), half axes of length 10 along
    /// each principal axis.
    BoundingVolume referenceBox()
    {
        return BoundingVolume::fromBox( Vec3( 1.0, 2.0, 3.0 ), Vec3( 10.0, 0.0, 0.0 ),
                                        Vec3( 0.0, 10.0, 0.0 ), Vec3( 0.0, 0.0, 10.0 ) );
    }

    /// Parent box used by the subdivision cases: unit-less cube centred on the origin with
    /// half extents of 10.
    BoundingVolume parentBox()
    {
        return BoundingVolume::fromBox( Vec3( 0.0 ), Vec3( 10.0, 0.0, 0.0 ),
                                       Vec3( 0.0, 10.0, 0.0 ), Vec3( 0.0, 0.0, 10.0 ) );
    }
} // namespace

TEST_CASE( "boundingVolumeCenter reads the centre for box and sphere" )
{
    const Vec3 boxCenter = boundingVolumeCenter( referenceBox() );
    CHECK( boxCenter.x == 1.0 );
    CHECK( boxCenter.y == 2.0 );
    CHECK( boxCenter.z == 3.0 );

    const BoundingVolume sphere = BoundingVolume::fromSphere( Vec3( 5.0, 6.0, 7.0 ), 4.5 );
    const Vec3 sphereCenter = boundingVolumeCenter( sphere );
    CHECK( sphereCenter.x == 5.0 );
    CHECK( sphereCenter.y == 6.0 );
    CHECK( sphereCenter.z == 7.0 );
}

TEST_CASE( "boundingVolumeRadius matches the reference formulas" )
{
    // Box: sqrt(10^2 + 10^2 + 10^2) = sqrt(300)
    CHECK( boundingVolumeRadius( referenceBox() ) == doctest::Approx( std::sqrt( 300.0 ) ).epsilon( 1e-12 ) );

    // Sphere: the radius is read straight out of the payload.
    CHECK( boundingVolumeRadius( BoundingVolume::fromSphere( Vec3( 5.0, 6.0, 7.0 ), 4.5 ) ) == 4.5 );

    // Region: the largest corner distance from the centre.
    const BoundingVolume region = BoundingVolume::fromRegion( Region{ 0.0, 0.0, 0.1, 0.1, 0.0, 100.0 } );
    CHECK( boundingVolumeRadius( region ) > 0.0 );
}

TEST_CASE( "region centre is the geodetic midpoint projected onto the ellipsoid" )
{
    const Region raw{ 0.0, 0.0, 0.1, 0.1, 0.0, 100.0 };
    const Vec3 center = boundingVolumeCenter( BoundingVolume::fromRegion( raw ) );
    const Vec3 expected = wgs84ToCartesian( 0.05, 0.05, 50.0 );

    CHECK( center.x == doctest::Approx( expected.x ).epsilon( 1e-12 ) );
    CHECK( center.y == doctest::Approx( expected.y ).epsilon( 1e-12 ) );
    CHECK( center.z == doctest::Approx( expected.z ).epsilon( 1e-12 ) );
}

TEST_CASE( "subdivideBox quadtree children keep the parent z extent" )
{
    SUBCASE( "child 0 is the lower-left quadrant" )
    {
        const BoundingVolume child = subdivideBox( parentBox(), 0, false );
        CHECK( child.type == BoundingVolume::Type::Box );
        CHECK( child.data[0] == doctest::Approx( -5.0 ) );
        CHECK( child.data[1] == doctest::Approx( -5.0 ) );
        CHECK( child.data[2] == doctest::Approx( 0.0 ) );

        // x and y half axes halve; z is untouched.
        CHECK( child.data[3] == doctest::Approx( 5.0 ) );
        CHECK( child.data[7] == doctest::Approx( 5.0 ) );
        CHECK( child.data[9] == doctest::Approx( 0.0 ) );
        CHECK( child.data[11] == doctest::Approx( 10.0 ) );
    }

    SUBCASE( "child 3 is the upper-right quadrant" )
    {
        const BoundingVolume child = subdivideBox( parentBox(), 3, false );
        CHECK( child.data[0] == doctest::Approx( 5.0 ) );
        CHECK( child.data[1] == doctest::Approx( 5.0 ) );
    }
}

TEST_CASE( "subdivideBox octree child 5 (binary 101) is the x+, y-, z+ octant" )
{
    const BoundingVolume child = subdivideBox( parentBox(), 5, true );

    CHECK( child.data[0] == doctest::Approx( 5.0 ) );
    CHECK( child.data[1] == doctest::Approx( -5.0 ) );
    CHECK( child.data[2] == doctest::Approx( 5.0 ) );

    // For an octree the z half axis halves as well.
    CHECK( child.data[9] == doctest::Approx( 0.0 ) );
    CHECK( child.data[11] == doctest::Approx( 5.0 ) );
}

// NOTE: the reference implementation throws when subdivideBox or asRegion are called on
// the wrong volume type. The core layer is exception free by design (Godot is built
// without C++ exceptions, see BoundingVolume.h), so those are preconditions enforced by
// assert instead. They are deliberately NOT exercised here: a violated assert aborts the
// test binary in Debug builds. The tests below cover the valid paths only.

TEST_CASE( "type accessors behave for the valid combinations" )
{
    // Half axes only mean something for a box.
    const BoundingVolume sphere = BoundingVolume::fromSphere( Vec3( 0.0 ), 1.0 );
    CHECK( glm::length( sphere.boxHalfAxis( 0 ) ) == 0.0 );
    CHECK( glm::length( referenceBox().boxHalfAxis( 1 ) ) == doctest::Approx( 10.0 ) );

    // Out-of-range index is not an error, it simply has no axis.
    CHECK( glm::length( referenceBox().boxHalfAxis( 7 ) ) == 0.0 );
}

TEST_CASE( "asRegion round-trips the geodetic extents" )
{
    const Region raw{ 0.1, -0.2, 0.3, -0.4, 10.0, 20.0 };
    const Region returned = BoundingVolume::fromRegion( raw ).asRegion();

    CHECK( returned.west == 0.1 );
    CHECK( returned.south == -0.2 );
    CHECK( returned.east == 0.3 );
    CHECK( returned.north == -0.4 );
    CHECK( returned.minHeight == 10.0 );
    CHECK( returned.maxHeight == 20.0 );
}
