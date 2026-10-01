// SPDX-License-Identifier: Unlicense

#include "math/TileScheme.h"

#include "math/GeoMath.h"

#include <algorithm>
#include <cmath>

namespace tiles3d::math
{
    double latitudeFromMercatorY( double t )
    {
        return std::atan( std::sinh( kPi * ( 1.0 - 2.0 * t ) ) );
    }

    TileRectangle tileXYToRectangle( std::int64_t x, std::int64_t y, int level )
    {
        // 2^level as a double: level is bounded well below 53 by the scheduler, and the
        // double form is what the longitude arithmetic below needs.
        const double n = std::ldexp( 1.0, level );
        const double size = 2.0 * kPi / n;

        TileRectangle rectangle;
        rectangle.west = -kPi + static_cast<double>( x ) * size;
        rectangle.east = rectangle.west + size;
        rectangle.north = latitudeFromMercatorY( static_cast<double>( y ) / n );
        rectangle.south = latitudeFromMercatorY( static_cast<double>( y + 1 ) / n );
        return rectangle;
    }

    TileRectangle tileGeometryRectangle( std::int64_t x, std::int64_t y, int level )
    {
        (void)x; // The geometry rectangle only differs from the imagery rectangle in Y.

        TileRectangle rectangle = tileXYToRectangle( x, y, level );
        const std::int64_t n = static_cast<std::int64_t>( 1 ) << level;

        // Stretch the outermost rows to the poles so there is no hole at either cap.
        if ( y == 0 )
        {
            rectangle.north = kPi / 2.0;
        }
        if ( y == n - 1 )
        {
            rectangle.south = -kPi / 2.0;
        }
        return rectangle;
    }

    int tileXYToQuadKey( std::int64_t x, std::int64_t y, int level, char *out, int out_size )
    {
        if ( out == nullptr || out_size <= level )
        {
            return 0;
        }

        for ( int i = level; i > 0; --i )
        {
            const std::int64_t mask = static_cast<std::int64_t>( 1 ) << ( i - 1 );
            int digit = 0;
            if ( ( x & mask ) != 0 )
            {
                digit |= 1;
            }
            if ( ( y & mask ) != 0 )
            {
                digit |= 2;
            }
            out[level - i] = static_cast<char>( '0' + digit );
        }
        out[level] = '\0';
        return level;
    }

    double mercatorY( double latitude )
    {
        return std::asinh( std::tan( latitude ) );
    }

    double levelZeroGeometricError()
    {
        const double largestRadius = std::max( kWgs84SemiMajorAxis, kWgs84SemiMinorAxis );
        return ( ( largestRadius * 2.0 * kPi ) / 4.0 ) / 65.0;
    }

    double levelGeometricError( int level )
    {
        return levelZeroGeometricError() / std::ldexp( 1.0, level );
    }

} // namespace tiles3d::math
