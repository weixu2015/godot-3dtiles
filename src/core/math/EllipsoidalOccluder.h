// SPDX-License-Identifier: Unlicense
//
// Horizon culling for a globe rendered in Godot's Y-up world.
//
// Ported from the reference globe implementation:
//   web-spatial-examples/apps/main/src/views/globe/renderers/globe3d/tileScheme.ts
//   (§6, itself a faithful port of Cesium's EllipsoidalOccluder)
//
// Why this exists: without it, a quadtree that only does frustum culling still selects
// tiles on the far side of the planet. Those tiles are off-screen but inside no frustum
// plane test the near side passes, so they get loaded and rendered. Horizon culling
// rejects them using the ellipsoid's own curvature.
//
// The technique: scale the whole problem into "scaled space" by dividing each axis by the
// corresponding ellipsoid radius. In that space the ellipsoid is the unit sphere, and the
// test reduces to a plane/point comparison. This is why the divisors below are
// (A, C, B) in that order - A and B are equatorial (X and Z), C is polar (Y).

#ifndef TILES3D_CORE_MATH_ELLIPSOIDALOCCLUDER_H
#define TILES3D_CORE_MATH_ELLIPSOIDALOCCLUDER_H

#include "math/Types.h"

namespace tiles3d::math
{
    /// Horizon culling point for an ellipsoid rectangle, or a flag saying it has none.
    struct HorizonCullingPoint
    {
        Vec3 position{ 0.0 };

        /// False when the rectangle cannot be described by a single culling point, which
        /// happens when its samples do not all lie on the same side of the ellipsoid
        /// (Cesium returns undefined here). Callers must skip the culling test.
        bool valid = false;
    };

    /// Computes the horizon culling point for a rectangle, given the geodetic sample
    /// points on its boundary (and centre).
    ///
    /// Equivalent to the reference `computeHorizonCullingPointFromRectangle`. The samples
    /// are expected as (longitude, latitude) pairs in radians; the reference passes nine
    /// of them (four corners, four edge midpoints, centre).
    ///
    /// @param west,south,east,north the rectangle, radians. Its centre drives the
    ///        direction the culling point is measured along.
    /// @param samples array of 2-element lat/lon pairs.
    /// @param sample_count number of entries in `samples`.
    HorizonCullingPoint computeHorizonCullingPointFromRectangle( double west, double south,
                                                                 double east, double north,
                                                                 const double *samples,
                                                                 int sample_count );

    /// Whether a point known to be on the ellipsoid surface is visible from the camera.
    ///
    /// Equivalent to the reference `isScaledSpacePointVisible`, including the
    /// `vhSquared < 0` branch that handles a camera *inside* the ellipsoid (sub-surface
    /// cameras must not cull everything).
    ///
    /// @param occludeeScaledSpacePosition a horizon culling point, already in scaled space.
    /// @param cameraPosition camera position in the same world units as the ellipsoid
    ///        radii (metres), before scaling.
    bool isScaledSpacePointVisible( const Vec3 &occludeeScaledSpacePosition,
                                    const Vec3 &cameraPosition );

} // namespace tiles3d::math

#endif
