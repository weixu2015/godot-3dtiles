// SPDX-License-Identifier: Unlicense
//
// The three bounding volume representations allowed by the 3D Tiles specification, plus
// the operations the scheduler needs from them.
//
// Ported from the reference implementation (threeDTiles/index.ts: parseBoundingVolume,
// getBoundingVolumeCenter, getBoundingVolumeRadius, subdivideBox).

#ifndef TILES3D_CORE_MATH_BOUNDINGVOLUME_H
#define TILES3D_CORE_MATH_BOUNDINGVOLUME_H

#include "math/GeoMath.h"
#include "math/Types.h"

#include <array>

namespace tiles3d::math
{
    /// Stored as a fixed 12-double payload so the type stays trivially copyable and cheap
    /// to move around the tile tree, mirroring the reference's `{type, data: number[]}`:
    ///
    ///   Box    -> [centre.xyz, halfAxisX.xyz, halfAxisY.xyz, halfAxisZ.xyz]   (12)
    ///   Sphere -> [centre.xyz, radius]                                       (4)
    ///   Region -> [west, south, east, north, minHeight, maxHeight]            (6)
    ///
    /// Unused trailing slots stay zero. A region is EPSG:4979 absolute geodetic
    /// coordinates and is deliberately *not* transformed by the tile's transform chain;
    /// see docs/REFACTOR_PLAN.md D1 for how it reaches the render frame.
    ///
    /// This layer is deliberately exception free. Godot itself is built with C++
    /// exceptions disabled, and godot-cpp's default GODOTCPP_DISABLE_EXCEPTIONS=ON adds
    /// _HAS_EXCEPTIONS=0 to consumers, so a library in that link chain must not rely on
    /// throwing. The conditions that the reference implementation expresses as throws are
    /// caller programming errors here and are handled with assert plus a defined fallback.
    struct BoundingVolume
    {
        enum class Type
        {
            Box = 0,
            Region = 1,
            Sphere = 2,
        };

        Type type = Type::Box;
        std::array<double, 12> data{};

        static BoundingVolume fromBox( const Vec3 &center, const Vec3 &halfAxisX,
                                       const Vec3 &halfAxisY, const Vec3 &halfAxisZ );
        static BoundingVolume fromSphere( const Vec3 &center, double radius );
        static BoundingVolume fromRegion( const Region &region );

        /// Reinterprets the payload as a Region.
        ///
        /// Precondition: `type == Region`. Callers normally branch on `type` first, so
        /// this is only reached on the region path. Misuse trips an assert in debug
        /// builds; with asserts compiled out the payload is reinterpreted, which stays
        /// defined behaviour (there is no exception to throw, by design).
        Region asRegion() const;

        /// Half-axis `index` (0..2) of a box; a zero vector for sphere, region and
        /// out-of-range indices.
        Vec3 boxHalfAxis( int index ) const;
    };

    /// Centre of the volume. A region reduces to the geodetic midpoint, projected onto the
    /// ellipsoid at the midpoint height, exactly as the reference does.
    Vec3 boundingVolumeCenter( const BoundingVolume &volume );

    /// Radius of the volume. For a box this is the length of the half-axis triad's
    /// diagonal (sqrt(sum of squared axis lengths)); for a region it is the largest corner
    /// distance from the centre. Mirrors the reference `getBoundingVolumeRadius`.
    double boundingVolumeRadius( const BoundingVolume &volume );

    /// Subdivides a box for implicit tiling.
    ///
    /// `childIndex` encodes the quadrant/octant bitwise: bit 0 selects +x when set, bit 1
    /// selects +y, bit 2 selects +z and is only consulted for octrees. Quadtree children
    /// keep the parent's z centre and z extent, matching Cesium's subdivideBox.
    ///
    /// Precondition: `parent.type == Box`. The 3D Tiles specification only permits box
    /// (or region, which convertRegionBoundingVolumes turns into a box before subdivision
    /// ever runs) for implicit tiling, so this holds by construction. Misuse trips an
    /// assert in debug builds and returns a degenerate origin-centred box otherwise.
    BoundingVolume subdivideBox( const BoundingVolume &parent, int childIndex, bool isOctree );

} // namespace tiles3d::math

#endif
