// SPDX-License-Identifier: Unlicense

#include "math/ScreenSpaceError.h"

#include "math/Mat4.h"

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace tiles3d::math
{
    namespace
    {
        /// Guards against dividing by zero when the camera sits exactly on the volume.
        constexpr double kMinimumDistance = 1e-7;
    } // namespace

    double computeScreenSpaceError( double geometricError, double distance,
                                    double viewportHeight, double fovDegrees )
    {
        const double fovRadians = fovDegrees * kPi / 180.0;
        const double denominator = 2.0 * std::tan( fovRadians / 2.0 );
        return ( geometricError * viewportHeight ) /
               ( std::max( distance, kMinimumDistance ) * denominator );
    }

    double computeSurfaceDistance( double distanceToCenter, double radius )
    {
        return std::max( distanceToCenter - radius, 0.0 );
    }

    double orientedBoxDistanceToPoint( const OrientedBoundingBox &box, const Vec3 &point )
    {
        const Vec3 halfExtents( glm::length( box.halfAxes[0] ), glm::length( box.halfAxes[1] ),
                                glm::length( box.halfAxes[2] ) );
        const Vec3 offset = point - box.center;
        const Vec3 clamped( std::clamp( offset.x, -halfExtents.x, halfExtents.x ),
                            std::clamp( offset.y, -halfExtents.y, halfExtents.y ),
                            std::clamp( offset.z, -halfExtents.z, halfExtents.z ) );
        return glm::length( offset - clamped );
    }

    double computeBvSurfaceDistance( const BoundingVolume *volume, const Mat4 &worldMatrix,
                                     const Vec3 &cameraPosition, double sphereSurfaceDistance )
    {
        if ( volume == nullptr || volume->type != BoundingVolume::Type::Box )
        {
            return sphereSurfaceDistance;
        }

        // Move the camera into the box's local frame so the box can be treated as axis
        // aligned, exactly as the reference does.
        const Vec3 localCamera = transformPoint( invert( worldMatrix ), cameraPosition );

        OrientedBoundingBox localBox;
        localBox.center = boundingVolumeCenter( *volume );
        for ( int axis = 0; axis < 3; ++axis )
        {
            localBox.halfAxes[static_cast<std::size_t>( axis )] = volume->boxHalfAxis( axis );
        }

        return orientedBoxDistanceToPoint( localBox, localCamera );
    }

    double fog( double distance, double density )
    {
        const double scalar = distance * density;
        return 1.0 - std::exp( -( scalar * scalar ) );
    }

} // namespace tiles3d::math
