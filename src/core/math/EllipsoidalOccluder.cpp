// SPDX-License-Identifier: Unlicense

#include "math/EllipsoidalOccluder.h"

#include "math/GeoMath.h"

#include <algorithm>
#include <cmath>

namespace tiles3d::math
{
    namespace
    {
        /// Reference `computeMagnitude`: the scalar along the scaled-space direction at
        /// which a tangent plane to the ellipsoid touches this sample.
        ///
        /// Returns a negative value when the sample is on the opposite side of the
        /// ellipsoid from the direction, which the caller treats as "no valid culling
        /// point".
        double computeMagnitude( double longitude, double latitude, double sdx, double sdy,
                                 double sdz )
        {
            const Vec3 sample = geodeticToYUp( longitude, latitude, 0.0 );

            // Scale into unit-sphere space. Order is (A, C, B): X and Z are equatorial,
            // Y is polar.
            const double sx = sample.x / kWgs84SemiMajorAxis;
            const double sy = sample.y / kWgs84SemiMinorAxis;
            const double sz = sample.z / kWgs84SemiMajorAxis;
            const double magnitudeSquaredRaw = sx * sx + sy * sy + sz * sz;
            double magnitude = std::sqrt( magnitudeSquaredRaw );
            if ( magnitude == 0.0 )
            {
                return -1.0;
            }

            const double dx = sx / magnitude;
            const double dy = sy / magnitude;
            const double dz = sz / magnitude;

            const double clampedMagnitudeSquared = std::max( 1.0, magnitudeSquaredRaw );
            magnitude = std::max( 1.0, magnitude );

            const double cosAlpha = dx * sdx + dy * sdy + dz * sdz;
            const double cx = dy * sdz - dz * sdy;
            const double cy = dz * sdx - dx * sdz;
            const double cz = dx * sdy - dy * sdx;
            const double sinAlpha = std::sqrt( cx * cx + cy * cy + cz * cz );
            const double cosBeta = 1.0 / magnitude;
            const double sinBeta = std::sqrt( clampedMagnitudeSquared - 1.0 ) * cosBeta;

            const double denominator = cosAlpha * cosBeta - sinAlpha * sinBeta;
            if ( std::abs( denominator ) < 1e-30 )
            {
                return -1.0;
            }
            return 1.0 / denominator;
        }
    } // namespace

    HorizonCullingPoint computeHorizonCullingPointFromRectangle( double west, double south,
                                                                 double east, double north,
                                                                 const double *samples,
                                                                 int sample_count )
    {
        HorizonCullingPoint result;

        if ( samples == nullptr || sample_count <= 0 )
        {
            return result;
        }

        // Direction is the scaled-space normal at the rectangle centre, scaled to the unit
        // sphere then normalised.
        const double centerLon = ( west + east ) / 2.0;
        const double centerLat = ( north + south ) / 2.0;
        Vec3 direction = geodeticToYUp( centerLon, centerLat, 0.0 );
        direction.x /= kWgs84SemiMajorAxis;
        direction.y /= kWgs84SemiMinorAxis;
        direction.z /= kWgs84SemiMajorAxis;
        direction = normalizeSafe( direction );

        const double sdx = direction.x;
        const double sdy = direction.y;
        const double sdz = direction.z;

        double resultMagnitude = 0.0;
        for ( int i = 0; i < sample_count; ++i )
        {
            const double candidate =
                computeMagnitude( samples[i * 2], samples[i * 2 + 1], sdx, sdy, sdz );
            if ( candidate < 0.0 )
            {
                // A sample sits on the other side: no single culling point describes this
                // rectangle. Cesium returns undefined and the caller skips culling.
                return result;
            }
            resultMagnitude = std::max( resultMagnitude, candidate );
        }

        if ( resultMagnitude <= 0.0 || !std::isfinite( resultMagnitude ) )
        {
            return result;
        }

        result.position = Vec3( sdx * resultMagnitude, sdy * resultMagnitude,
                                sdz * resultMagnitude );
        result.valid = true;
        return result;
    }

    bool isScaledSpacePointVisible( const Vec3 &occludeeScaledSpacePosition,
                                    const Vec3 &cameraPosition )
    {
        const Vec3 cameraScaled( cameraPosition.x / kWgs84SemiMajorAxis,
                                 cameraPosition.y / kWgs84SemiMinorAxis,
                                 cameraPosition.z / kWgs84SemiMajorAxis );

        const Vec3 toOccludee = occludeeScaledSpacePosition - cameraScaled;

        const double vhMagnitudeSquared = glm::dot( cameraScaled, cameraScaled ) - 1.0;
        const double vtDotVc = -glm::dot( toOccludee, cameraScaled );

        // A camera inside the ellipsoid (vhMagnitudeSquared < 0) can still see the point if
        // the direction is pointing outward, hence the separate branch.
        const bool occluded =
            vhMagnitudeSquared < 0.0
                ? vtDotVc > 0.0
                : vtDotVc > vhMagnitudeSquared &&
                      ( vtDotVc * vtDotVc ) / glm::dot( toOccludee, toOccludee ) >
                          vhMagnitudeSquared;

        return !occluded;
    }

} // namespace tiles3d::math
