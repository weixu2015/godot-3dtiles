// SPDX-License-Identifier: Unlicense
//
// Screen space error and the distance terms that feed it. These are the numbers that
// decide every LOD refinement in the scheduler, so they are kept in their own module and
// unit tested against the reference implementation's expected values.
//
// Ported from threeDTiles/index.ts: computeScreenSpaceError, computeSurfaceDistance,
// computeBvSurfaceDistance, fog.

#ifndef TILES3D_CORE_MATH_SCREENSPACEERROR_H
#define TILES3D_CORE_MATH_SCREENSPACEERROR_H

#include "math/BoundingVolume.h"
#include "math/Types.h"

#include <array>

namespace tiles3d::math
{
    /// Screen space error that drives level of detail refinement:
    ///
    ///   sse = geometricError * viewportHeight / (max(distance, 1e-7) * 2 tan(fov / 2))
    ///
    /// @param geometricError tile geometric error, metres.
    /// @param distance camera to bounding volume surface distance, world units.
    /// @param viewportHeight viewport height in pixels.
    /// @param fovDegrees vertical field of view in degrees, i.e. Godot's
    ///        Camera3D::get_fov(). The clamp on `distance` mirrors the reference and is
    ///        what keeps a camera exactly on the volume from dividing by zero.
    double computeScreenSpaceError( double geometricError, double distance,
                                    double viewportHeight, double fovDegrees );

    /// Distance from the camera to the surface of a bounding sphere, clamped at zero so a
    /// camera inside the volume yields a large SSE and forces refinement.
    double computeSurfaceDistance( double distanceToCenter, double radius );

    /// Distance from a point to an oriented box, measured in the box's own frame where the
    /// box can be treated as axis aligned: clamp the offset to the half extents, then
    /// measure. Port of the reference box branch, which mirrors NASA's
    /// `OrientedBoundingBox.distanceToPoint`.
    ///
    /// Half extents are taken as the *lengths* of the half-axis vectors, matching the
    /// reference (and assuming the axes are mutually orthogonal, which the 3D Tiles
    /// specification guarantees for `box`).
    double orientedBoxDistanceToPoint( const OrientedBoundingBox &box, const Vec3 &point );

    /// Camera to bounding volume surface distance.
    ///
    /// Boxes use the true oriented box distance; spheres and regions fall back to
    /// `sphereSurfaceDistance`, which the caller derives from the enclosing sphere.
    ///
    /// The distinction is not cosmetic. A flat photogrammetry tile seen from above has a
    /// bounding sphere far larger than its thickness, so using the sphere distance would
    /// overestimate the surface distance, underestimate the SSE and leave the tile stuck at
    /// a low level of detail.
    ///
    /// @param volume may be null, in which case `sphereSurfaceDistance` is returned as is.
    /// @param worldMatrix tile world matrix; the camera is brought into the box's local
    ///        frame with its inverse.
    double computeBvSurfaceDistance( const BoundingVolume *volume, const Mat4 &worldMatrix,
                                     const Vec3 &cameraPosition, double sphereSurfaceDistance );

    /// Fog factor matching CesiumMath.fog: `1 - exp(-(distance * density)^2)`.
    /// Used to damp the dynamic screen space error with distance.
    double fog( double distance, double density );

} // namespace tiles3d::math

#endif
