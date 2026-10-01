// SPDX-License-Identifier: Unlicense
//
// Equidistant cylindrical (ECT) tiling scheme for the globe's surface quadtree.
//
// Ported from the reference globe implementation:
//   web-spatial-examples/apps/main/src/views/globe/renderers/globe3d/tileScheme.ts
//
// This is NOT the Web Mercator tiling scheme. The reference uses a quadtree whose
// longitudes are evenly spaced (hence "equidistant cylindrical") while latitudes are
// distributed by the Mercator mapping. The two are easy to confuse because
// `latitudeFromMercatorY` looks like a Mercator helper; what it actually does here is
// spread the rows so that imagery rows line up linearly with mercator Y.
//
// Convention: level L has 2^L x 2^L tiles, x increases eastward, y = 0 is the
// northernmost row. All angles are radians.

#ifndef TILES3D_CORE_MATH_TILESCHEME_H
#define TILES3D_CORE_MATH_TILESCHEME_H

#include "math/Types.h"

#include <cstdint>

namespace tiles3d::math
{
    /// A geodetic rectangle in radians: west/east longitudes and south/north latitudes.
    struct TileRectangle
    {
        double west = 0.0;
        double east = 0.0;
        double south = 0.0;
        double north = 0.0;
    };

    /// Maps a normalised row coordinate to a latitude.
    ///
    /// `t` is in [0, 1] where 0 is the north edge of the tiling. Equivalent to the
    /// reference `latitudeFromMercatorY`, i.e. Cesium's
    /// `WebMercatorTilingScheme.geodeticLatitudeFromMercatorAngle`.
    double latitudeFromMercatorY( double t );

    /// Geodetic rectangle covered by tile (x, y) at `level`.
    ///
    /// Equivalent to the reference `tileXYToRectangle`. Precondition: level >= 0 and
    /// x, y in [0, 2^level).
    TileRectangle tileXYToRectangle( std::int64_t x, std::int64_t y, int level );

    /// The rectangle the tile's *geometry* should span, as opposed to the rectangle its
    /// imagery covers.
    ///
    /// Web Mercator only defines tiles up to +/-85.0511 degrees, so the northernmost row
    /// is stretched to the pole and the southernmost row likewise. Without this the polar
    /// caps are holes. Equivalent to the reference `tileGeometryRectangle`.
    TileRectangle tileGeometryRectangle( std::int64_t x, std::int64_t y, int level );

    /// Bing/quadtree quadkey for a tile. Equivalent to the reference `tileXYToQuadKey`.
    ///
    /// Writes into `out` (at least `level + 1` chars) and returns the length, so the
    /// kernel layer needs no heap. Returns 0 when the buffer is too small.
    int tileXYToQuadKey( std::int64_t x, std::int64_t y, int level, char *out, int out_size );

    /// Mercator Y for a latitude: `asinh(tan(lat))`. The imagery rows are linear in this
    /// quantity, which is what makes the UV mapping in the tile mesh work.
    double mercatorY( double latitude );

    /// Geometric error that a tile at level 0 is considered to have, metres.
    ///
    /// Reference: `GE_LEVEL0 = ((max(A, C) * 2*pi) / 4) / 65`, i.e. a quarter of the
    /// equator divided into 65 columns. Tiles deeper than 0 use `levelZero / 2^level`.
    double levelZeroGeometricError();

    /// Geometric error for a tile at `level`.
    double levelGeometricError( int level );

} // namespace tiles3d::math

#endif
