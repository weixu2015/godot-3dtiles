// SPDX-License-Identifier: Unlicense
//
// Direct port of the matrix helpers used by the reference Three.js scheduler
// (web-spatial-examples/apps/main/src/views/threeDTiles/index.ts, lines 100-126).
//
// All matrices are double precision and column-major, matching both glm::dmat4 and
// the layout the reference implementation exchanges as number[16]. Keeping this
// identical is what lets tools/sched_trace diff a per-tile worldMatrix against the
// TypeScript implementation element by element.

#ifndef TILES3D_CORE_MATH_MAT4_H
#define TILES3D_CORE_MATH_MAT4_H

#include "math/Types.h"

namespace tiles3d::math
{
    /// Identity matrix. Equivalent to the reference `identityMatrix4()`.
    Mat4 identity();

    /// Returns `a * b` (column-major convention, i.e. b is applied first).
    /// Equivalent to the reference `mat4Mul(a, b)`.
    Mat4 multiply( const Mat4 &a, const Mat4 &b );

    /// Full 4x4 inverse. Equivalent to the reference `mat4Inverse(m)`.
    Mat4 invert( const Mat4 &m );

    /// Largest absolute scale factor of the matrix' linear part.
    /// Equivalent to the reference `getMatrixMaxScale(m)`; used to scale bounding
    /// volume radii by the tile transform.
    double maxScale( const Mat4 &m );

    /// Reads 16 doubles in column-major order. The reference implementation stores
    /// matrices this way, so this is the boundary used by tests and trace tooling.
    Mat4 fromColumnMajor( const double *elements );

    /// Writes 16 doubles in column-major order.
    void toColumnMajor( const Mat4 &m, double *out_elements );

    /// Rejects NaN / infinity in any of the 16 elements.
    bool isFinite( const Mat4 &m );

    /// Transforms a position: equivalent to three.js `Vector3.applyMatrix4` (w = 1).
    Vec3 transformPoint( const Mat4 &m, const Vec3 &v );

    /// Transforms a direction: w = 0, so translation is ignored.
    Vec3 transformDirection( const Mat4 &m, const Vec3 &v );

    /// Upper-left 3x3 block. Equivalent to three.js `Matrix3.setFromMatrix4`.
    Mat3 linearPart( const Mat4 &m );

    /// Equivalent to three.js `Vector3.applyMatrix3`.
    Vec3 transformLinear( const Mat3 &m, const Vec3 &v );

} // namespace tiles3d::math

#endif
