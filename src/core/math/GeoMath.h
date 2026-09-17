// SPDX-License-Identifier: Unlicense
//
// Ellipsoid maths and region -> oriented-bounding-box conversion.
//
// Ported from the reference implementation (threeDTiles/index.ts, the WGS84 section and
// regionToEcefObb). Vector algebra is done with glm directly except where the reference
// has non-obvious semantics, which is called out on the function.

#ifndef TILES3D_CORE_MATH_GEOMATH_H
#define TILES3D_CORE_MATH_GEOMATH_H

#include "math/Types.h"

#include <array>
#include <cstdint>

namespace tiles3d::math
{
    /// WGS84 semi-major axis, metres (consts.ts WGS84_A).
    inline constexpr double kWgs84SemiMajorAxis = 6378137.0;

    /// WGS84 first eccentricity squared (consts.ts WGS84_E2).
    inline constexpr double kWgs84EccentricitySquared = 0.00669437999013;

    inline constexpr double kPi = 3.14159265358979323846;

    /// A 3D Tiles `region` bounding volume: EPSG:4979 geodetic extents in radians and
    /// metres, ordered [west, south, east, north, minHeight, maxHeight].
    struct Region
    {
        double west = 0.0;
        double south = 0.0;
        double east = 0.0;
        double north = 0.0;
        double minHeight = 0.0;
        double maxHeight = 0.0;
    };

    /// Centre plus three half-axis vectors, in the frame the box is expressed in.
    struct OrientedBoundingBox
    {
        Vec3 center{ 0.0 };
        std::array<Vec3, 3> halfAxes{ Vec3( 0.0 ), Vec3( 0.0 ), Vec3( 0.0 ) };
    };

    /// Number of tiles preceding `level` within a subtree. `branchingFactor` is 4
    /// (quadtree) or 8 (octree); level 0 is the subtree root.
    ///
    /// Equivalent to the reference `getLevelOffset` = (bf^level - 1) / (bf - 1), but
    /// evaluated iteratively in 64-bit integers so it is exact rather than subject to
    /// pow() rounding. Representable up to level 31 (quadtree) / 21 (octree).
    ///
    /// Precondition: branchingFactor >= 2 and level >= 0. Violations are caller bugs and
    /// trip an assert in debug builds, returning 0 otherwise. This layer does not throw;
    /// see the note on BoundingVolume for why.
    std::int64_t levelOffset( int branchingFactor, int level );

    /// Geodetic (radians, metres above the ellipsoid) to ECEF cartesian.
    Vec3 wgs84ToCartesian( double longitude, double latitude, double height );

    /// Unit surface normal of the ellipsoid at (longitude, latitude).
    Vec3 wgs84SurfaceNormal( double longitude, double latitude );

    /// Divides by the length, but returns the input unchanged when the length is zero.
    /// Mirrors the reference `normalize3` (`hypot(...) || 1`), which avoids NaN where
    /// glm::normalize would produce one. Used on the region plane axes.
    Vec3 normalizeSafe( const Vec3 &v );

    /// The eight region corners in ECEF, minimum height first. Mirrors the reference
    /// `getRegionCorners` ordering so radius computations match exactly.
    std::array<Vec3, 8> regionCorners( const Region &region );

    /// Region (EPSG:4979) to a tight ECEF oriented bounding box.
    ///
    /// Line-by-line port of the reference `regionToEcefObb`, which is itself a replica of
    /// Cesium's `OrientedBoundingBox.fromRectangle`:
    ///   - width <= pi: tangent plane at the region centre. The maxHeight perimeter points
    ///     are projected onto that plane for the x/y extents; the minHeight corner
    ///     distances along the normal give minZ, and maxZ is taken as maxHeight.
    ///   - width >  pi: equatorial plane scheme (Cesium fromRectangle, the wide-region
    ///     branch), rotating around Z.
    ///
    /// Both branches finish with the same `fromPlaneExtents` step:
    ///   centre = origin + sum(axis_i * (min_i + max_i) / 2)
    ///   halfAxes_i = axis_i * (max_i - min_i) / 2
    OrientedBoundingBox regionToEcefObb( const Region &region );

    /// East-north-up frame anchored at an ECEF origin.
    ///
    /// Columns are (east, north, up) and the translation is `originEcef`, so this maps a
    /// position expressed in the local ENU frame to ECEF. The result is Z-up, which is the
    /// convention 3D Tiles uses for tile space; bringing it into Godot's Y-up world is the
    /// Godot layer's job.
    ///
    /// Degenerate at the poles: east is derived from cross(+Z, up), which vanishes when up
    /// is parallel to Z. An origin exactly on a pole yields a non-orthogonal frame; a
    /// georeference should not be placed there.
    Mat4 eastNorthUpToFixedFrame( const Vec3 &originEcef );

} // namespace tiles3d::math

#endif
