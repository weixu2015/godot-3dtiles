// SPDX-License-Identifier: Unlicense
//
// Port of the reference helpers `mat4Mul` / `identityMatrix4` / `getMatrixMaxScale` /
// `mat4Inverse` (threeDTiles/index.ts lines 103-126) to double-precision glm.

// GLM_GTX_matrix_decompose is an experimental extension in GLM 1.0.x and must be
// enabled before any GLM header that pulls it in is included.
#define GLM_ENABLE_EXPERIMENTAL

#include "math/Mat4.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>

namespace tiles3d::math
{
    Mat4 identity()
    {
        return Mat4( 1.0 );
    }

    Mat4 multiply( const Mat4 &a, const Mat4 &b )
    {
        // The reference stores matrices column-major and computes `a.multiply(b)`,
        // i.e. the product a * b. glm uses the same convention, so this is a direct
        // translation.
        return a * b;
    }

    Mat4 invert( const Mat4 &m )
    {
        return glm::inverse( m );
    }

    double maxScale( const Mat4 &m )
    {
        // The reference decomposes into translate/rotate/scale and returns the largest
        // scale component (three.js Matrix4.decompose). glm::decompose is the direct
        // equivalent.
        glm::dvec3 scale( 0.0 );
        glm::dquat orientation;
        glm::dvec3 translation;
        glm::dvec3 skew;
        glm::dvec4 perspective;

        if ( glm::decompose( m, scale, orientation, translation, skew, perspective ) )
        {
            return std::max( std::max( std::abs( scale.x ), std::abs( scale.y ) ),
                             std::abs( scale.z ) );
        }

        // glm::decompose fails on singular matrices (zero determinant). Fall back to the
        // lengths of the upper 3x3 column vectors, which is what three.js effectively
        // derives its scale from for an affine matrix.
        const glm::dvec3 c0( m[0] );
        const glm::dvec3 c1( m[1] );
        const glm::dvec3 c2( m[2] );
        return std::max( std::max( glm::length( c0 ), glm::length( c1 ) ),
                         glm::length( c2 ) );
    }

    Mat4 fromColumnMajor( const double *elements )
    {
        Mat4 result;
        for ( int column = 0; column < 4; ++column )
        {
            for ( int row = 0; row < 4; ++row )
            {
                result[column][row] = elements[column * 4 + row];
            }
        }
        return result;
    }

    void toColumnMajor( const Mat4 &m, double *out_elements )
    {
        for ( int column = 0; column < 4; ++column )
        {
            for ( int row = 0; row < 4; ++row )
            {
                out_elements[column * 4 + row] = m[column][row];
            }
        }
    }

    bool isFinite( const Mat4 &m )
    {
        for ( int column = 0; column < 4; ++column )
        {
            for ( int row = 0; row < 4; ++row )
            {
                if ( !std::isfinite( m[column][row] ) )
                {
                    return false;
                }
            }
        }
        return true;
    }

    Vec3 transformPoint( const Mat4 &m, const Vec3 &v )
    {
        const Vec4 result = m * Vec4( v, 1.0 );
        return Vec3( result.x, result.y, result.z );
    }

    Vec3 transformDirection( const Mat4 &m, const Vec3 &v )
    {
        const Vec4 result = m * Vec4( v, 0.0 );
        return Vec3( result.x, result.y, result.z );
    }

    Mat3 linearPart( const Mat4 &m )
    {
        return Mat3( m );
    }

    Vec3 transformLinear( const Mat3 &m, const Vec3 &v )
    {
        return m * v;
    }

} // namespace tiles3d::math
