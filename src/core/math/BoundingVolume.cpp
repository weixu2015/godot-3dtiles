// SPDX-License-Identifier: Unlicense

#include "math/BoundingVolume.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>

namespace tiles3d::math
{
    BoundingVolume BoundingVolume::fromBox( const Vec3 &center, const Vec3 &halfAxisX,
                                            const Vec3 &halfAxisY, const Vec3 &halfAxisZ )
    {
        BoundingVolume volume;
        volume.type = Type::Box;
        volume.data = { center.x,      center.y,      center.z,      halfAxisX.x,
                        halfAxisX.y,   halfAxisX.z,   halfAxisY.x,   halfAxisY.y,
                        halfAxisY.z,   halfAxisZ.x,   halfAxisZ.y,   halfAxisZ.z };
        return volume;
    }

    BoundingVolume BoundingVolume::fromSphere( const Vec3 &center, double radius )
    {
        BoundingVolume volume;
        volume.type = Type::Sphere;
        volume.data = { center.x, center.y, center.z, radius };
        return volume;
    }

    BoundingVolume BoundingVolume::fromRegion( const Region &region )
    {
        BoundingVolume volume;
        volume.type = Type::Region;
        volume.data = { region.west,  region.south, region.east,
                        region.north, region.minHeight, region.maxHeight };
        return volume;
    }

    Region BoundingVolume::asRegion() const
    {
        // Precondition, not a runtime error: every caller branches on `type` first. See
        // the header for why this layer does not throw.
        assert( type == Type::Region );

        Region region;
        region.west = data[0];
        region.south = data[1];
        region.east = data[2];
        region.north = data[3];
        region.minHeight = data[4];
        region.maxHeight = data[5];
        return region;
    }

    Vec3 BoundingVolume::boxHalfAxis( int index ) const
    {
        if ( type != Type::Box || index < 0 || index > 2 )
        {
            return Vec3( 0.0 );
        }

        const int base = 3 + index * 3;
        return Vec3( data[base], data[base + 1], data[base + 2] );
    }

    Vec3 boundingVolumeCenter( const BoundingVolume &volume )
    {
        switch ( volume.type )
        {
            case BoundingVolume::Type::Box:
            case BoundingVolume::Type::Sphere:
                return Vec3( volume.data[0], volume.data[1], volume.data[2] );

            case BoundingVolume::Type::Region:
            {
                const Region region = volume.asRegion();
                return wgs84ToCartesian( ( region.west + region.east ) / 2.0,
                                         ( region.south + region.north ) / 2.0,
                                         ( region.minHeight + region.maxHeight ) / 2.0 );
            }
        }

        return Vec3( 0.0 );
    }

    double boundingVolumeRadius( const BoundingVolume &volume )
    {
        switch ( volume.type )
        {
            case BoundingVolume::Type::Box:
            {
                const double lengthX = glm::length( volume.boxHalfAxis( 0 ) );
                const double lengthY = glm::length( volume.boxHalfAxis( 1 ) );
                const double lengthZ = glm::length( volume.boxHalfAxis( 2 ) );
                return std::sqrt( lengthX * lengthX + lengthY * lengthY + lengthZ * lengthZ );
            }

            case BoundingVolume::Type::Sphere:
                return volume.data[3];

            case BoundingVolume::Type::Region:
            {
                const Vec3 center = boundingVolumeCenter( volume );
                const std::array<Vec3, 8> corners = regionCorners( volume.asRegion() );

                double maximum = 0.0;
                for ( const Vec3 &corner : corners )
                {
                    maximum = std::max( maximum, glm::length( corner - center ) );
                }
                return maximum;
            }
        }

        return 0.0;
    }

    BoundingVolume subdivideBox( const BoundingVolume &parent, int childIndex, bool isOctree )
    {
        // Precondition, guaranteed by the specification: implicit tiling only subdivides
        // boxes (regions are converted first). See the header for why this layer does not
        // throw; misuse is a caller bug and trips the assert in debug builds.
        assert( parent.type == BoundingVolume::Type::Box );
        if ( parent.type != BoundingVolume::Type::Box )
        {
            return BoundingVolume::fromBox( Vec3( 0.0 ), Vec3( 0.0 ), Vec3( 0.0 ), Vec3( 0.0 ) );
        }

        const Vec3 center = boundingVolumeCenter( parent );
        const Vec3 axisX = parent.boxHalfAxis( 0 );
        const Vec3 axisY = parent.boxHalfAxis( 1 );
        const Vec3 axisZ = parent.boxHalfAxis( 2 );

        const int indexX = childIndex & 1;
        const int indexY = ( childIndex >> 1 ) & 1;
        const int indexZ = isOctree ? ( ( childIndex >> 2 ) & 1 ) : 0;

        // A quadtree child keeps the parent's z centre, so the z term is skipped rather
        // than evaluated with indexZ == 0.
        Vec3 newCenter = center + axisX * ( indexX - 0.5 ) + axisY * ( indexY - 0.5 );
        if ( isOctree )
        {
            newCenter += axisZ * ( indexZ - 0.5 );
        }

        const Vec3 newAxisX = axisX * 0.5;
        const Vec3 newAxisY = axisY * 0.5;
        const Vec3 newAxisZ = isOctree ? axisZ * 0.5 : axisZ;

        return BoundingVolume::fromBox( newCenter, newAxisX, newAxisY, newAxisZ );
    }

} // namespace tiles3d::math
