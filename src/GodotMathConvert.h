// SPDX-License-Identifier: Unlicense
//
// The single place where the kernel's double precision meets Godot's float32 types.
//
// Every narrowing conversion is explicit. The extension target builds with /WX, so an
// implicit double to float conversion (MSVC C4305) is a hard error rather than a warning -
// and the kernel genuinely needs double for ECEF-magnitude work, so the narrowing has to be
// deliberate and local. Keeping it here means there is exactly one file to audit.

#ifndef GODOT_MATH_CONVERT_H
#define GODOT_MATH_CONVERT_H

#include "core/math/Mat4.h"
#include "core/math/Types.h"

#include "godot_cpp/variant/basis.hpp"
#include "godot_cpp/variant/transform3d.hpp"
#include "godot_cpp/variant/vector3.hpp"

namespace tiles3d
{
    inline godot::Vector3 toGodotVector( const math::Vec3 &v )
    {
        return godot::Vector3( static_cast<float>( v.x ), static_cast<float>( v.y ),
                               static_cast<float>( v.z ) );
    }

    /// Godot to kernel. Widening, so no explicit cast is needed, but it lives here with its
    /// counterpart so the direction of travel is obvious at every call site.
    inline math::Vec3 fromGodotVector( const godot::Vector3 &v )
    {
        return math::Vec3( v.x, v.y, v.z );
    }

    /// Column-major matrix to Godot transform.
    ///
    /// Both sides are column-major, and Godot's `Basis(x_axis, y_axis, z_axis)` stores its
    /// arguments as columns (`rows[0] = {x.x, y.x, z.x}`), so no transpose is involved.
    inline godot::Transform3D toGodotTransform( const math::Mat4 &m )
    {
        const godot::Basis basis( toGodotVector( math::Vec3( m[0] ) ),
                                  toGodotVector( math::Vec3( m[1] ) ),
                                  toGodotVector( math::Vec3( m[2] ) ) );
        return godot::Transform3D( basis, toGodotVector( math::Vec3( m[3] ) ) );
    }

} // namespace tiles3d

#endif
