// SPDX-License-Identifier: Unlicense

#include "math/GeoMath.h"

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>

namespace tiles3d::math
{
    std::int64_t levelOffset( int branchingFactor, int level )
    {
        // Preconditions, not runtime errors: only 4 and 8 are meaningful in 3D Tiles and
        // level is always non-negative. See the header for why this layer does not throw.
        assert( branchingFactor >= 2 );
        assert( level >= 0 );
        if ( branchingFactor < 2 || level < 0 )
        {
            return 0;
        }

        // Iterative evaluation of (bf^level - 1) / (bf - 1); exact in integer arithmetic.
        std::int64_t offset = 0;
        for ( int i = 0; i < level; ++i )
        {
            offset = offset * branchingFactor + 1;
        }
        return offset;
    }

    Vec3 wgs84ToCartesian( double longitude, double latitude, double height )
    {
        const double sinLat = std::sin( latitude );
        const double cosLat = std::cos( latitude );
        const double primeVerticalRadius =
            kWgs84SemiMajorAxis / std::sqrt( 1.0 - kWgs84EccentricitySquared * sinLat * sinLat );

        return Vec3(
            ( primeVerticalRadius + height ) * cosLat * std::cos( longitude ),
            ( primeVerticalRadius + height ) * cosLat * std::sin( longitude ),
            ( ( 1.0 - kWgs84EccentricitySquared ) * primeVerticalRadius + height ) * sinLat );
    }

    Vec3 wgs84SurfaceNormal( double longitude, double latitude )
    {
        const double cosLat = std::cos( latitude );
        return Vec3( cosLat * std::cos( longitude ), cosLat * std::sin( longitude ),
                     std::sin( latitude ) );
    }

    Vec3 normalizeSafe( const Vec3 &v )
    {
        const double length = glm::length( v );
        return length > 0.0 ? v / length : v;
    }

    std::array<Vec3, 8> regionCorners( const Region &region )
    {
        return { wgs84ToCartesian( region.west, region.south, region.minHeight ),
                 wgs84ToCartesian( region.west, region.north, region.minHeight ),
                 wgs84ToCartesian( region.east, region.south, region.minHeight ),
                 wgs84ToCartesian( region.east, region.north, region.minHeight ),
                 wgs84ToCartesian( region.west, region.south, region.maxHeight ),
                 wgs84ToCartesian( region.west, region.north, region.maxHeight ),
                 wgs84ToCartesian( region.east, region.south, region.maxHeight ),
                 wgs84ToCartesian( region.east, region.north, region.maxHeight ) };
    }

    OrientedBoundingBox regionToEcefObb( const Region &region )
    {
        const double lonCenter = ( region.west + region.east ) / 2.0;
        const double rawLatCenter = ( region.south + region.north ) / 2.0;
        // A region straddling the equator must use latitude 0 as the plane centre,
        // because that is where the east-west extent is widest.
        const double latCenter =
            ( region.south < 0.0 && region.north > 0.0 ) ? 0.0 : rawLatCenter;

        Vec3 planeOrigin( 0.0, 0.0, 0.0 );
        Vec3 planeXAxis( 1.0, 0.0, 0.0 );
        Vec3 planeYAxis( 0.0, 1.0, 0.0 );
        Vec3 planeZAxis( 0.0, 0.0, 1.0 );

        double minX = 0.0;
        double maxX = 0.0;
        double minY = 0.0;
        double maxY = 0.0;
        double minZ = 0.0;
        double maxZ = 0.0;

        if ( region.east - region.west <= kPi )
        {
            const Vec3 tangent = wgs84ToCartesian( lonCenter, rawLatCenter, 0.0 );
            const Vec3 up = wgs84SurfaceNormal( lonCenter, rawLatCenter );
            const Vec3 east =
                normalizeSafe( Vec3( -std::sin( lonCenter ), std::cos( lonCenter ), 0.0 ) );
            const Vec3 north = glm::cross( up, east );

            // Signed distances along the tangent plane axes.
            const auto projectedX = [&]( const Vec3 &p ) { return glm::dot( p - tangent, east ); };
            const auto projectedY = [&]( const Vec3 &p ) { return glm::dot( p - tangent, north ); };
            const auto projectedZ = [&]( const Vec3 &p ) { return glm::dot( p - tangent, up ); };

            // Perimeter points at maximum height: north centre, north west, west centre,
            // south west, south centre.
            const Vec3 northCenter = wgs84ToCartesian( lonCenter, region.north, region.maxHeight );
            const Vec3 northWest = wgs84ToCartesian( region.west, region.north, region.maxHeight );
            const Vec3 westCenter = wgs84ToCartesian( region.west, latCenter, region.maxHeight );
            const Vec3 southWest = wgs84ToCartesian( region.west, region.south, region.maxHeight );
            const Vec3 southCenter = wgs84ToCartesian( lonCenter, region.south, region.maxHeight );

            minX = std::min( { projectedX( northWest ), projectedX( westCenter ),
                               projectedX( southWest ) } );
            maxX = -minX; // symmetric about the centre meridian
            maxY = std::max( projectedY( northWest ), projectedY( northCenter ) );
            minY = std::min( projectedY( southWest ), projectedY( southCenter ) );

            minZ = std::min( projectedZ( wgs84ToCartesian( region.west, region.north,
                                                           region.minHeight ) ),
                             projectedZ( wgs84ToCartesian( region.west, region.south,
                                                           region.minHeight ) ) );
            maxZ = region.maxHeight;

            planeOrigin = tangent;
            planeXAxis = east;
            planeYAxis = north;
            planeZAxis = up;
        }
        else
        {
            // Wide region: rotate around Z on the equatorial plane.
            const bool fullyAboveEquator = region.south > 0.0;
            const bool fullyBelowEquator = region.north < 0.0;
            const double latitudeNearestToEquator = fullyAboveEquator ? region.south
                                              : fullyBelowEquator ? region.north
                                                                  : 0.0;

            const Vec3 flattened =
                wgs84ToCartesian( lonCenter, latitudeNearestToEquator, region.maxHeight );
            const Vec3 origin( flattened.x, flattened.y, 0.0 );

            const bool isPole = std::abs( origin.x ) < 1e-10 && std::abs( origin.y ) < 1e-10;
            const Vec3 planeNormal = !isPole ? normalizeSafe( origin ) : Vec3( 1.0, 0.0, 0.0 );

            planeYAxis = Vec3( 0.0, 0.0, 1.0 );
            planeXAxis = normalizeSafe( glm::cross( planeNormal, planeYAxis ) );
            planeZAxis = planeNormal;
            planeOrigin = origin;

            const Vec3 horizon = wgs84ToCartesian( lonCenter + kPi / 2.0, latitudeNearestToEquator,
                                                   region.maxHeight );
            maxX = glm::dot( horizon - planeOrigin, planeXAxis );
            minX = -maxX;
            maxY = wgs84ToCartesian( 0.0, region.north,
                                     fullyBelowEquator ? region.minHeight : region.maxHeight )
                       .z;
            minY = wgs84ToCartesian( 0.0, region.south,
                                     fullyAboveEquator ? region.minHeight : region.maxHeight )
                       .z;
            minZ = glm::dot( wgs84ToCartesian( region.east, latitudeNearestToEquator,
                                               region.maxHeight ) -
                                 planeOrigin,
                             planeNormal );
            maxZ = 0.0;
        }

        // fromPlaneExtents
        const Vec3 centerOffset( ( minX + maxX ) / 2.0, ( minY + maxY ) / 2.0,
                                 ( minZ + maxZ ) / 2.0 );
        const Vec3 halfScale( ( maxX - minX ) / 2.0, ( maxY - minY ) / 2.0,
                              ( maxZ - minZ ) / 2.0 );

        OrientedBoundingBox result;
        result.center = planeOrigin + planeXAxis * centerOffset.x + planeYAxis * centerOffset.y +
                        planeZAxis * centerOffset.z;
        result.halfAxes[0] = planeXAxis * halfScale.x;
        result.halfAxes[1] = planeYAxis * halfScale.y;
        result.halfAxes[2] = planeZAxis * halfScale.z;
        return result;
    }

    Mat4 eastNorthUpToFixedFrame( const Vec3 &originEcef )
    {
        const Vec3 up = normalizeSafe( originEcef );
        const Vec3 east = normalizeSafe( glm::cross( Vec3( 0.0, 0.0, 1.0 ), up ) );
        const Vec3 north = glm::cross( up, east );

        // Columns are the frame axes; column 3 is the origin. Built explicitly rather than
        // via identity() so this file does not need Mat4.h.
        Mat4 frame;
        frame[0] = Vec4( east, 0.0 );
        frame[1] = Vec4( north, 0.0 );
        frame[2] = Vec4( up, 0.0 );
        frame[3] = Vec4( originEcef, 1.0 );
        return frame;
    }

} // namespace tiles3d::math
