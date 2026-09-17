// SPDX-License-Identifier: Unlicense
//
// Shared value types for the scheduling kernel.
//
// Everything here is double precision, on purpose. Godot's real_t / Vector3 is float32
// by default, which resolves only ~0.5 m at ECEF magnitudes (6.4e6 m) and visibly
// jitters photogrammetry. The kernel therefore works in double and converts to
// Godot's float types only at the boundary. See docs/REFACTOR_PLAN.md D1, and
// tests/test_mat4.cpp for the measured residual that backs this up.

#ifndef TILES3D_CORE_MATH_TYPES_H
#define TILES3D_CORE_MATH_TYPES_H

#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace tiles3d::math
{
    using Vec3 = glm::dvec3;
    using Vec4 = glm::dvec4;
    using Mat3 = glm::dmat3;
    using Mat4 = glm::dmat4;

} // namespace tiles3d::math

#endif
