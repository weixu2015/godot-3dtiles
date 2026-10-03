// SPDX-License-Identifier: Unlicense
//
// GlobeAtmosphereShading: the single-scattering integral, in one place.
//
// The reference (web-spatial-examples `renderers/globe3d/atmosphere.ts`, itself a port of
// Cesium's `GlobeFS.glsl`) defines the integral exactly once, in `GLSL_ATMO_COMMON`, and
// then *includes that same string in two shaders*:
//
//   1. ATMO_FS                  - the shell outside the silhouette.
//   2. applyDayNightShading     - injected into the **tile** material via onBeforeCompile,
//                                 which is where the blue veil across the whole disc comes
//                                 from (`atmosphere.ts:390`, `GlobeFS.glsl:516`).
//
// Splitting those into two shaders with two copies of the integral is not a style choice.
// The first version of this port put the ground pass on Globe3D's own `Surface` mesh, and
// that mesh is *never visible*: GlobeTileLayer streams the imagery as its own per-tile
// meshes, drawn on top of it. The pass ran, reported a live material and correct uniforms,
// and contributed nothing to any pixel. The veil has to be on the tile material.
//
// The per-frame inputs (viewer position, sun direction, camera height, fade) reach both
// consumers through **global shader parameters** rather than per-material uniforms. A globe
// view has hundreds of visible tiles; pushing seven uniforms into each of their materials
// every frame is pure overhead, and it is also how the reference does it - one shared
// uniform object (`SHARED`) referenced by every tile program.
//
// Scope: one globe, one georeference per scene, which is what GlobeFrame already assumes.
// Two Globe3D nodes in one scene would share these globals and fight over them.

#ifndef GLOBE_ATMOSPHERE_SHADING_H
#define GLOBE_ATMOSPHERE_SHADING_H

#include "GlobeFrame.h"

#include "godot_cpp/classes/shader.hpp"
#include "godot_cpp/variant/transform3d.hpp"
#include "godot_cpp/variant/vector3.hpp"

namespace tiles3d
{
    namespace GlobeAtmosphereShading
    {
        /// Registers the global shader parameters. Idempotent, and must have run before any
        /// material using either shader is created - a shader that declares a `global uniform`
        /// which does not exist fails to compile, silently taking the whole material with it.
        void ensure_globals();

        /// The shell: the glow outside the planet's silhouette.
        const godot::Ref<godot::Shader> &shell_shader();

        /// The ground pass: the veil across the disc, plus the day/night terminator. Meant to
        /// be applied to whatever actually draws the visible surface - the streamed tiles -
        /// and to Globe3D's own ellipsoid as the no-imagery fallback.
        const godot::Ref<godot::Shader> &ground_shader();

        /// Per-frame values shared by both passes. `p_viewer_ecef_y_up` and `p_sun_y_up` are
        /// Y-up ECEF metres, the space the integral's length() calls are calibrated for.
        void publish( const godot::Vector3 &p_viewer_ecef_y_up, double p_camera_height,
                      double p_camera_distance, const godot::Vector3 &p_sun_y_up,
                      double p_ground_light_intensity, double p_shell_light_intensity,
                      double p_lighting_fade, bool p_ground_atmosphere_on,
                      bool p_enable_lighting );

        /// The global that carries the frame->ECEF matrix, named so a caller can publish it
        /// without duplicating the string.
        extern const char *const kLocalToEcefYUpParam;

        /// A frame's local space -> Y-up ECEF, as a shader mat4.
        ///
        /// Meshes are authored in the shared frame's local space (ENU, Z-up, anchored on the
        /// georeference), but the integral measures distances from the Earth's *centre*. Under
        /// a Georeference3D those origins are ~6371 km apart, so a shader that treats a
        /// vertex as ECEF computes a completely different - and wrong - atmosphere. The shell
        /// avoids this by carrying Y-up ECEF vertices and letting the node transform do the
        /// mapping (`Globe3D::apply_ecef_y_up_placement`); the ground pass cannot, because it
        /// has to be bolted onto tile meshes that are built in the frame's local space.
        godot::Transform3D local_to_ecef_y_up( const GlobeFrame &p_frame );

        /// Publishes `local_to_ecef_y_up( p_frame )` to the global of the same name. Called
        /// per frame rather than once, because the frame can change under a reparented
        /// georeference and a stale matrix is a wrong atmosphere, not a missing one.
        void publish_frame( const GlobeFrame &p_frame );

    } // namespace GlobeAtmosphereShading

} // namespace tiles3d

#endif
