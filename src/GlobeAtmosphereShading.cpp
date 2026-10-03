// SPDX-License-Identifier: Unlicense

#include "GlobeAtmosphereShading.h"

#include "core/math/GeoMath.h"
#include "core/math/Mat4.h"
#include "core/math/Types.h"

#include "godot_cpp/classes/rendering_server.hpp"
#include "godot_cpp/classes/shader.hpp"
#include "godot_cpp/variant/basis.hpp"
#include "godot_cpp/variant/string.hpp"
#include "godot_cpp/variant/string_name.hpp"
#include "godot_cpp/variant/transform3d.hpp"
#include "godot_cpp/variant/typed_array.hpp"
#include "godot_cpp/variant/variant.hpp"
#include "godot_cpp/variant/vector3.hpp"

#include <glm/glm.hpp>

namespace tiles3d
{
    namespace GlobeAtmosphereShading
    {
        using godot::Ref;
        using godot::RenderingServer;
        using godot::Shader;
        using godot::String;
        using godot::Transform3D;
        using godot::Vector3;

        const char *const kLocalToEcefYUpParam = "globe_atmo_local_to_ecef_y_up";

        namespace
        {
            /// Set once the global shader parameters have actually been registered.
            ///
            /// Deliberately *not* derived from `global_shader_parameter_get_list()`: that is an
            /// editor-only query, and outside the editor it hits an ERR_FAIL and returns
            /// nothing - which reads as "none of them exist" and re-adds every global on every
            /// call. Re-adding is not harmless either: it resets the value to its default, so a
            /// per-frame publish() would be undone by the next ensure_globals() and the effect
            /// would silently stop tracking the camera.
            bool g_globals_registered = false;

            // ---- global shader parameter names ----
            //
            // One flat namespace, prefixed, so nothing here can collide with a project-level
            // global. The values are per-*frame*; anything that is per-tile (the texture, the
            // UV transform) stays a material parameter on the consumer side.
            constexpr const char *kViewerPosition = "globe_atmo_viewer_position";
            constexpr const char *kEllipsoidRadii = "globe_atmo_ellipsoid_radii";
            constexpr const char *kCameraHeight = "globe_atmo_camera_height";
            constexpr const char *kCameraDistance = "globe_atmo_camera_distance";
            constexpr const char *kSunDirection = "globe_atmo_sun_direction";
            constexpr const char *kGroundOn = "globe_atmo_ground_on";
            constexpr const char *kEnableLighting = "globe_atmo_enable_lighting";
            constexpr const char *kLightingFade = "globe_atmo_lighting_fade";
            constexpr const char *kGroundIntensity = "globe_atmo_ground_light_intensity";
            constexpr const char *kShellIntensity = "globe_atmo_shell_light_intensity";
            constexpr const char *kSunsetTint = "globe_atmo_sunset_tint";
            constexpr const char *kTint = "globe_atmo_tint";
            constexpr const char *kDebugPure = "globe_atmo_debug_pure";
            constexpr const char *kDebugAlbedo = "globe_atmo_debug_albedo";

/// The single-scattering integral, shared verbatim in structure by both consumers.
///
/// A direct port of the reference's GLSL_ATMO_COMMON (atmosphere.ts:63-245, itself
            /// a port of Cesium's AtmosphereCommon.glsl + SkyAtmosphereCommon.glsl), with four
            /// Godot-side adaptations, each of which is a real difference and each commented
            /// where it happens:
///
///  1. snake_case names, and the shared uniforms are *global* rather than per-material.
///  2. `compute_atmosphere_color` takes the light intensity as an argument instead
///     of reading a uniform. The reference gets away with one uniform name because
///     three.js compiles a separate program per material; with one global there
///     would be no way to give the shell 50.0 and the ground pass 10.0.
///  3. A sunset warm-up term. Not in the reference - a uniform sun direction cannot
///     redden on its own, because a real sunset is the light path lengthening as
///     the sun grazes the limb. Shared, so the veil and the limb cannot disagree.
///  4. PRIMARY_STEPS_MAX is 48 rather than 16, with an adaptive budget. See
///     compute_scattering.
///
/// Everything else - the step distribution, the two density exponentials, the
/// optical-depth accumulation, the phase functions, the coverage remap - is the
/// reference's arithmetic, unchanged.
///
/// COLOUR SPACE - the ground pass runs entirely in DISPLAY space. Read before changing it.
///
/// The reference's fragment is:
///     lit  = <sRGB texture value> * dayIntensity
///     fa   = lit + scatter * 1.5
///     fa   = 1 - exp(-2 * fa)                      <- applied to a DISPLAY-domain value
///     out  = three.js encodes the framebuffer
///
/// Both operands of that composite are already display-referred: the texture arrives sRGB-encoded
/// and the scattering is small enough to be treated the same way. The exposure curve is calibrated
/// against that, which is why it is `1 - exp(-2x)` and not something with a 1/2.2 in it. So the
/// value the reference hands the framebuffer encoder is ALREADY display-referred, and the correct
/// Godot equivalent is that same number written straight into ALBEDO - no decode on the way in,
/// no encode on the way out.
///
/// Two wrong answers, both of which were tried here:
///
///   decode in, no encode out - the natural port, on the reasoning that StandardMaterial3D decodes
///   an sRGB albedo texture for free and a ShaderMaterial must ask. But ALBEDO under
///   `render_mode unshaded` is written straight to the backbuffer, so there is no encode waiting
///   to undo it. Net gamma loss: the day side measured 0.503 -> 0.099 luminance, i.e. 5x, which is
///   why the planet looked dim and its glow looked absent. Caused by ground_debug_albedo = 2,
///   which paints the raw fetch and then the fetch with the decode; a round trip would leave the
///   two equal.
///   no decode, encode out - the over-correction. Measured centre pixel 0.82, essentially white,
///   because the composite was encoded a second time on top of an already display-domain value.
///
/// The tell for both: the symptom is a *global* gamma shift, which flattens mid-tone detail
/// everywhere at once. That is never an intensity problem. Do not reach for
/// `ground_atmosphere_intensity` to compensate - it tunes one term and leaves the veil and the
/// imagery misbalanced against each other, which is much harder to diagnose later.
///
/// SIZE CEILING - READ BEFORE ADDING A COMMENT IN HERE.
///
/// This is one raw-string GLSL literal and MSVC caps a single string literal at 16380
/// bytes, reported as a bare `error C2026: string too big, trailing characters truncated`
/// at whatever line happens to cross the boundary. That line is nowhere near the real
/// cause, the diagnostic does not name the literal, and the failure appears only after a
/// comment that looked harmless - so it costs a full build cycle to find each time.
///
/// Three consequences for editing:
///   - Long prose does not belong in this literal. Put it here, in the C++ comment around
///     it, where it costs nothing. `srgb_to_linear_approx` carries a two-line version for
///     exactly this reason.
///   - Never write this literal's own delimiter characters in a comment near it. Naming
///     them makes any substring search for the literal's start or end terminate on the
///     comment instead, which is silent corruption - and it happened while writing this note.
///   - If C2026 does appear, measure before cutting: the fix is to move prose out, never to
///     drop reference arithmetic. Deleting a numerical line to satisfy a compiler limit
///     produces a *plausible* render with a silently wrong integral, which is far worse
///     than a build error.
constexpr const char *kGlslAtmoCommon = R"GLSL(
// ---- shared, per-frame (global shader parameters) ----
global uniform vec3 globe_atmo_viewer_position;   // camera, Y-up ECEF metres from the centre
global uniform vec3 globe_atmo_ellipsoid_radii;   // (A, C, B) - the polar radius is on Y
global uniform float globe_atmo_camera_height;    // metres above the ellipsoid
global uniform float globe_atmo_camera_distance;  // metres from the Earth's centre
global uniform vec3 globe_atmo_sun_direction;     // unit vector, Y-up ECEF
global uniform float globe_atmo_ground_on;        // 1 = the ground pass is enabled
global uniform float globe_atmo_enable_lighting;  // 1 = a real sun, 0 = flat (reference's
                                                  //     uEnableLighting / uLightEnum != NONE)
global uniform float globe_atmo_lighting_fade;    // the effect's own distance ramp
global uniform float globe_atmo_ground_light_intensity;
global uniform float globe_atmo_shell_light_intensity;
global uniform float globe_atmo_sunset_tint;      // 0 disables the warm-up term
global uniform vec3 globe_atmo_tint;
global uniform mat4 globe_atmo_local_to_ecef_y_up; // mesh local space -> Y-up ECEF
global uniform float globe_atmo_debug_pure;       // 1 = paint the raw integral, no tonemap
global uniform float globe_atmo_debug_albedo;      // 1 = paint the raw texture fetch only

const float ATMOSPHERE_THICKNESS = 111e3;
const int PRIMARY_STEPS_MAX = 48;
const int LIGHT_STEPS_MAX = 4;

float approximate_tanh(float x) {
    float x2 = x * x;
    return max(-1.0, min(1.0, x * (27.0 + x2) / (27.0 + 9.0 * x2)));
}

vec2 ray_sphere_interval(vec3 origin, vec3 direction, float radius) {
    float b = dot(origin, direction);
    float c = dot(origin, origin) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant < 0.0) { return vec2(-1.0); }
    float sqrt_disc = sqrt(discriminant);
    return vec2(-b - sqrt_disc, -b + sqrt_disc);
}

// Projects a point onto the ellipsoid along the ray from its centre.
//
// The reference applies this to every surface fragment (atmosphere.ts:430) and it is not
// optional: tile skirts are pushed *below* the surface, so without it they are shaded as
// sub-surface points and every LOD crack turns into a dark band.
vec3 project_to_ellipsoid(vec3 p) {
    return p / length(p / globe_atmo_ellipsoid_radii);
}

// The radius, measured from the Earth's centre, of the atmosphere's inner boundary along the
// camera's own geocentric direction.
//
// The reference derives this from uCameraHeight and an ellipsoid-flattening correction rather
// than passing A directly, because its shell mesh is a *scaled sphere* (radius A, squashed to C
// on Y): the radius the raymarch wants is the one along the view direction, which is between A
// and C. Skipping this on the polar view makes the atmosphere ~20 km too thick or too thin.
// Factored out of compute_shellScattering because the ground pass needs the same number.
float atmosphere_inner_radius() {
    float radii_difference = globe_atmo_ellipsoid_radii.x - globe_atmo_ellipsoid_radii.y;
    float adjust_min = globe_atmo_ellipsoid_radii.x / 4.0;
    float adjust_max = globe_atmo_ellipsoid_radii.x;
    float adjust_modifier = radii_difference / 2.0;
    float distance_adjust = adjust_modifier *
        clamp((globe_atmo_camera_height - adjust_min) / (adjust_max - adjust_min), 0.0, 1.0);
    float radius_adjust = (radii_difference / 4.0) + distance_adjust;
    return length(globe_atmo_viewer_position) - globe_atmo_camera_height - radius_adjust;
}

void compute_scattering(
    vec3 primary_origin, vec3 primary_direction, float primary_ray_length,
    vec3 light_direction, float atmosphere_inner_radius,
    out vec3 rayleigh_color, out vec3 mie_color, out float opacity) {
  rayleigh_color = vec3(0.0);
  mie_color = vec3(0.0);
  opacity = 0.0;

  float atmosphere_outer_radius = atmosphere_inner_radius + ATMOSPHERE_THICKNESS;
  vec2 intersect = ray_sphere_interval(primary_origin, primary_direction,
                                       atmosphere_outer_radius);
  if (intersect.y <= max(intersect.x, 0.0)) { return; }

  float x = 1e-7 * intersect.y / max(primary_ray_length, 1.0);
  float w_stop_gt_lprl = 0.5 * (1.0 + approximate_tanh(x));

  float start_0 = intersect.x;
  intersect.x = max(intersect.x, 0.0);
  intersect.y = min(intersect.y, primary_ray_length);

  // Captured before intersect is reused, because the step budget below has to reason about how
  // far the ray actually travels through air rather than through vacuum.
  float total_ray_length_pre = intersect.y - intersect.x;

  float x_o_a = start_0 - ATMOSPHERE_THICKNESS;
  float w_inside_atmosphere = 1.0 - 0.5 * (1.0 + approximate_tanh(x_o_a));

  // Step budget.
  //
  // The reference computes PRIMARY_STEPS = 16 - int(w_inside_atmosphere * 12), i.e. four samples
  // whenever the ray starts outside the atmosphere, and spaces them over total_ray_length / 7.
  // That is calibrated for a camera a few hundred km up, where a traversal really is that short.
  // From a globe-view distance the same four samples are spread over ~1e7 m, roughly 2.5e6 m
  // apart, while the Rayleigh density varies over a 1.0e4 m scale height: every sample then
  // lands in air so thin that exp(-h/H) underflows and the integral returns nothing.
  //
  // The two consumers here both mitigate that, and they mitigate it in the same place, because
  // the veil and the limb have to be integrated consistently or they disagree at the horizon:
  //   - the ground pass clips the primary ray to the segment inside the atmosphere before
  //     calling in (see ground_scattering), which is what actually makes the traversal short;
  //   - the bound below keeps the count from collapsing when a traversal *is* long.
  float traversal_ratio = total_ray_length_pre / ATMOSPHERE_THICKNESS;
  int adaptive_reduction = int(w_inside_atmosphere * 12.0);
  if (traversal_ratio > 1.0) {
    // One step per ~2.3 km of traversal, floored so even a full-chord ray keeps 16 samples.
    int needed = int(clamp(traversal_ratio / 25.0, 16.0, float(PRIMARY_STEPS_MAX)));
    // min(), not mini(): mini() is GLSL ES 3.0 and this compiles as ES 3.0-less GLSL under
    // Godot's compatibility renderer, where it fails to resolve and the whole material
    // silently falls back.
    adaptive_reduction = min(adaptive_reduction, PRIMARY_STEPS_MAX - needed);
  }
  int primary_steps = PRIMARY_STEPS_MAX - adaptive_reduction;
  int light_steps = LIGHT_STEPS_MAX - int(w_inside_atmosphere * 2.0);

  float ray_position_length = intersect.x;
  float total_ray_length = intersect.y - ray_position_length;
  float ray_step_length_increase = w_inside_atmosphere *
      ((1.0 - w_stop_gt_lprl) * total_ray_length /
      (float(primary_steps * (primary_steps + 1)) / 2.0));
  float ray_step_length =
      max(1.0 - w_inside_atmosphere, w_stop_gt_lprl) * total_ray_length /
      max(7.0 * w_inside_atmosphere, float(primary_steps));

  vec3 rayleigh_accumulation = vec3(0.0);
  vec3 mie_accumulation = vec3(0.0);
  vec2 optical_depth = vec2(0.0);
  vec2 height_scale = vec2(10000.0, 3200.0);
  vec3 rayleigh_coefficient = vec3(5.5e-6, 13.0e-6, 28.4e-6);
  vec3 mie_coefficient = vec3(21e-6, 21e-6, 21e-6);

  for (int i = 0; i < PRIMARY_STEPS_MAX; ++i) {
    if (i >= primary_steps) { break; }
    vec3 sample_position =
        primary_origin + primary_direction * (ray_position_length + ray_step_length);
    float sample_height = length(sample_position) - atmosphere_inner_radius;
    vec2 sample_density = exp(-sample_height / height_scale) * ray_step_length;
    optical_depth += sample_density;

    vec2 light_intersect =
        ray_sphere_interval(sample_position, light_direction, atmosphere_outer_radius);
    float light_step_length = light_intersect.y / float(light_steps);
    float light_position_length = 0.0;
    vec2 light_optical_depth = vec2(0.0);
    for (int j = 0; j < LIGHT_STEPS_MAX; ++j) {
      if (j >= light_steps) { break; }
      vec3 light_position = sample_position +
          light_direction * (light_position_length + light_step_length * 0.5);
      float light_height = length(light_position) - atmosphere_inner_radius;
      light_optical_depth += exp(-light_height / height_scale) * light_step_length;
      light_position_length += light_step_length;
    }

    vec3 attenuation = exp(-((mie_coefficient * (optical_depth.y + light_optical_depth.y)) +
                             (rayleigh_coefficient * (optical_depth.x + light_optical_depth.x))));
    rayleigh_accumulation += sample_density.x * attenuation;
    mie_accumulation += sample_density.y * attenuation;

    ray_position_length += (ray_step_length += ray_step_length_increase);
  }

  rayleigh_color = rayleigh_coefficient * rayleigh_accumulation;
  mie_color = mie_coefficient * mie_accumulation;
  opacity = length(exp(-((mie_coefficient * optical_depth.y) +
                         (rayleigh_coefficient * optical_depth.x))));
}

// The reference's coverage value, which is *not* the optical depth.
//
// computeShellScattering overwrites the opacity that computeScattering produced with a purely
// geometric quantity - how much of the atmosphere's radial extent lies beyond the camera - and
// then fades it by the sun angle. The ground pass inherits that overwrite, which is why its
// transmittance is view-dependent rather than a constant; at any camera above ~111 km the
// geometric term clamps to 0, so transmittance is exactly 1.5. Reproducing this is load-bearing:
// a ground pass that instead used the true optical depth would land at ~0.6 and the veil would
// come out two and a half times too weak.
float shell_coverage(vec3 position, vec3 light_direction) {
  float inner = atmosphere_inner_radius();
  float outer = inner + ATMOSPHERE_THICKNESS;
  float camera_height = globe_atmo_camera_height + inner;
  float opacity = clamp((outer - camera_height) / (outer - inner), 0.0, 1.0);
  float night_alpha = (globe_atmo_enable_lighting > 0.5)
      ? clamp(dot(normalize(position), light_direction), 0.0, 1.0)
      : 1.0;
  return opacity * pow(night_alpha, 0.5);
}

void compute_shell_scattering(vec3 position, vec3 light_direction,
    out vec3 rayleigh_color, out vec3 mie_color, out float opacity) {
  float inner = atmosphere_inner_radius();
  compute_scattering(
      globe_atmo_viewer_position,
      normalize(position - globe_atmo_viewer_position),
      length(position - globe_atmo_viewer_position),
      light_direction,
      inner,
      rayleigh_color, mie_color, opacity);
  opacity = shell_coverage(position, light_direction);
}

vec4 compute_atmosphere_color(vec3 position, vec3 light_direction,
    vec3 rayleigh_color, vec3 mie_color, float opacity, float light_intensity) {
  vec3 camera_to_position = normalize(position - globe_atmo_viewer_position);
  float cos_angle = dot(camera_to_position, light_direction);
  float cos_angle_sq = cos_angle * cos_angle;

  float g = 0.9; // uMieAnisotropy
  float g_sq = g * g;

  float rayleigh_phase = 3.0 / (50.2654824574) * (1.0 + cos_angle_sq);
  float mie_phase = 3.0 / (25.1327412287) *
      ((1.0 - g_sq) * (cos_angle_sq + 1.0)) /
      (pow(1.0 + g_sq - 2.0 * cos_angle * g, 1.5) * (2.0 + g_sq));

  vec3 color = (rayleigh_phase * rayleigh_color + mie_phase * mie_color) * light_intensity;

  // Sunset warm-up. Not in the reference: a single-scattering integral with a uniform sun
  // direction cannot redden, because a real sunset is the light path lengthening as the sun
  // grazes the limb, and a straight light march with a fixed direction never sees that.
  if (globe_atmo_sunset_tint > 0.0) {
    float grazing = 1.0 - clamp(abs(cos_angle), 0.0, 1.0);
    // Smoothed so the terminator reads as a band rather than a seam.
    float warm = smoothstep(0.55, 1.0, grazing) * globe_atmo_sunset_tint;
    color *= mix(vec3(1.0), vec3(1.35, 0.72, 0.42), warm);
  }

  return vec4(color * globe_atmo_tint, opacity);
}

// The ground pass's integral: the air column between the camera and a surface point.
//
// The reference reuses computeShellScattering here rather than writing a second integral, and
// this function is that reuse with one correction applied to the *inputs*: the primary ray is
// clipped to the segment between the atmosphere's outer boundary and the surface point. The
// un-clipped ray runs from the camera, which at a globe-view distance is ~1e7 m of vacuum
// followed by ~111 km of air, and the step distribution cannot resolve the difference - the
// samples land in vacuum and the integral underflows to nothing. Clipping is not a change of
// model: the integrand is zero outside the atmosphere, so the same integral over the same
// segment gives the same answer with the samples where the density actually varies.
void ground_scattering(vec3 surface_position, vec3 light_direction,
    out vec3 rayleigh_color, out vec3 mie_color, out float opacity) {
  rayleigh_color = vec3(0.0);
  mie_color = vec3(0.0);
  opacity = 0.0;

  float inner = atmosphere_inner_radius();
  float outer = inner + ATMOSPHERE_THICKNESS;
  vec3 to_surface = surface_position - globe_atmo_viewer_position;
  float surface_distance = length(to_surface);
  if (surface_distance < 1.0) { return; }
  vec3 view_direction = to_surface / surface_distance;

  vec2 span = ray_sphere_interval(globe_atmo_viewer_position, view_direction, outer);
  float start = max(span.x, 0.0);
  float end = min(surface_distance, span.y);
  if (end <= start) { return; }

  compute_scattering(
      globe_atmo_viewer_position + view_direction * start,
      view_direction,
      end - start,
      light_direction,
      inner,
      rayleigh_color, mie_color, opacity);

  // The reference's ground pass inherits shell_coverage's overwrite, not computeScattering's
  // optical depth - see the note there for why that difference is a factor of 2.5.
  opacity = shell_coverage(surface_position, light_direction);
}

// The reference's uSunDirWC, or its "no sun" stand-in (atmosphere.ts:237-243).
vec3 dynamic_light_direction(vec3 position) {
  if (globe_atmo_enable_lighting > 0.5) { return normalize(globe_atmo_sun_direction); }
  return normalize(position);
}

// Neutral tonemap + sRGB encode, ported verbatim from the reference (atmosphere.ts:218-235).
// Not optional polish: the integral is radiometric (~0.005-0.05) and a linear framebuffer
// shows that as a black ring. Only the shell uses these; the ground pass has its own curve.
vec3 pbr_neutral_tonemapping(vec3 color) {
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = (x < 0.08) ? (x - 6.25 * x * x) : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression) { return color; }
    const float d = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(color, new_peak * vec3(1.0), g);
}

vec3 inverse_gamma(vec3 color) {
    return pow(color, vec3(1.0 / 2.2));
}

// sRGB -> linear. Used ONLY by the ground_debug_albedo = 2 diagnostic, which exists to measure
// the colour-space round trip. The real path does not decode - see the COLOUR SPACE note above.
vec3 srgb_to_linear_approx(vec3 c) {
    return pow(c, vec3(2.2));
}
)GLSL";

            /// The shell: the glow outside the planet's silhouette.
            ///
            /// The reference's ATMO_VS + ATMO_FS. Two Godot-side adaptations:
            ///
            ///  - `cull_back` with the mesh wound *outward* like the surface. An inward-wound
            ///    shell is back-facing everywhere the camera can see it, so every fragment
            ///    outside the silhouette is culled and the halo vanishes while every other
            ///    check still passes.
            ///  - `ALBEDO` is the final colour under `render_mode unshaded` (EMISSION is
            ///    ignored), so the scattering goes into ALBEDO and the alpha into ALPHA.
            ///    depth_draw_never because a depth-writing transparent pass would hide
            ///    everything behind the limb.
            ///
            /// The vertices arrive already in Y-up ECEF: the mesh is built in that space and
            /// `Globe3D::apply_ecef_y_up_placement` gives the node the transform into the
            /// shared frame. The ground pass cannot do that (it is bolted onto meshes it does
            /// not own), which is why it needs globe_atmo_local_to_ecef_y_up and this does not.
            const Ref<Shader> &build_shell_shader()
            {
                static Ref<Shader> shader;
                if ( shader.is_valid() )
                {
                    return shader;
                }
                shader.instantiate();
                shader->set_code(
                    "shader_type spatial;\n"
                    "render_mode blend_mix, cull_back, unshaded, depth_draw_never,\n"
                    "            shadows_disabled, fog_disabled, specular_disabled;\n"
                    "\n" +
                    String( kGlslAtmoCommon ) +
                    "\n"
                    "varying vec3 v_outer_position;\n"
                    "\n"
                    "void vertex() {\n"
                    "    v_outer_position = VERTEX;\n"
                    "}\n"
                    "\n"
                    "void fragment() {\n"
                    "    vec3 light_direction = dynamic_light_direction(v_outer_position);\n"
                    "    vec3 rayleigh_color;\n"
                    "    vec3 mie_color;\n"
                    "    float opacity;\n"
                    "    compute_shell_scattering(v_outer_position, light_direction,\n"
                    "                             rayleigh_color, mie_color, opacity);\n"
                    "    vec4 color = compute_atmosphere_color(v_outer_position, light_direction,\n"
                    "                                          rayleigh_color, mie_color, opacity,\n"
                    "                                          globe_atmo_shell_light_intensity);\n"
                    // Diagnostic: paints the raw integral, amplified, with no tonemap and no
                    // compositing. It answers the question numeric probes kept failing to: is
                    // the integral returning anything at all? Written as if/else rather than an
                    // early `return` - Godot rejects `return` inside fragment(), and a rejected
                    // fragment shader silently drops the whole material.
                    "    if (globe_atmo_debug_pure > 0.5) {\n"
                    "        ALBEDO = clamp((rayleigh_color + mie_color) * 1.0e5, vec3(0.0), vec3(1.0));\n"
                    "        ALPHA = 1.0;\n"
                    "    } else {\n"
                    "        color.rgb = pbr_neutral_tonemapping(color.rgb);\n"
                    "        color.rgb = inverse_gamma(color.rgb);\n"
                    // Coverage, exactly as the reference: mix(color.b, 1.0, color.a).
                    //
                    // The blue channel is the *tonemapped* one, which is the whole point. The
                    // integral is a smooth function of how much air a ray crossed, so after
                    // the tonemap + sRGB encode the blue rises smoothly from 0 at the outer
                    // edge of the atmosphere to 1 in the dense layer near the surface. Feeding
                    // that back as coverage means the shell's own silhouette is never visible:
                    // the alpha has fallen to ~0 before the geometry runs out, so the image
                    // ends on a gradient rather than on the mesh boundary.
                    //
                    // Boosting the blue instead (the `* 4.0` this replaced) forces alpha to
                    // 1.0 across the dense band, and the halo then terminates exactly at the
                    // shell's projected edge - a bright crescent with a hard outer boundary.
                    "        color.a = mix(color.b, 1.0, color.a);\n"
                    "        ALBEDO = color.rgb;\n"
                    "        ALPHA = clamp(color.a, 0.0, 1.0);\n"
                    "    }\n"
                    "}\n" );
                return shader;
            }

            /// The ground pass: the blue veil across the disc, plus the day/night terminator.
            ///
            /// This is the half that was missing, and it is a separate pass in the reference
            /// too - `applyDayNightShading` (atmosphere.ts:385-451) injects it into the **tile**
            /// material via onBeforeCompile, and Cesium computes the same thing per tile
            /// fragment in GlobeFS.glsl:516. It has to live on whatever actually draws the
            /// visible surface, which for a streamed globe is GlobeTileLayer's per-tile meshes
            /// and not Globe3D's own ellipsoid: the latter is fully covered by them and
            /// contributes no pixels at all.
            ///
            /// Beyond the integral, this reproduces the tile material's whole feature set,
            /// because it replaces it: UNSHADED (the day/night term below *is* the lighting),
            /// CULL_DISABLED (the skirts must seal LOD cracks from both sides), the placeholder
            /// albedo, and the mercator UV scale/offset that maps a child tile into its
            /// ancestor's texture. Note what this material does NOT do: decode the texture to linear. The
            /// reference composites in display space and relies on three.js's framebuffer encode,
            /// so the equivalent here is to write the display-domain value out untouched - see the
            /// COLOUR SPACE note on kGlslAtmoCommon above, which records both wrong answers.
            const Ref<Shader> &build_ground_shader()
            {
                static Ref<Shader> shader;
                if ( shader.is_valid() )
                {
                    return shader;
                }
                shader.instantiate();
                shader->set_code(
                    "shader_type spatial;\n"
                    "render_mode unshaded, cull_disabled, specular_disabled, shadows_disabled,\n"
                    "            fog_disabled, ambient_light_disabled;\n"
                    "\n"
                    "uniform sampler2D u_albedo_texture;\n"
                    "uniform bool u_has_texture = false;\n"
                    "uniform vec3 u_base_color = vec3(1.0);\n"
                    "uniform vec2 u_uv_scale = vec2(1.0);\n"
                    "uniform vec2 u_uv_offset = vec2(0.0);\n"
                    "\n" +
                    String( kGlslAtmoCommon ) +
                    "\n"
                    "varying vec3 v_ecef_position;\n"
                    "varying vec3 v_geo_normal;\n"
                    "varying vec2 v_tile_uv;\n"
                    "\n"
                    "void vertex() {\n"
                    // The mesh is authored in the shared frame's local space (ENU, Z-up,
                    // anchored on the georeference) but the integral measures from the Earth's
                    // centre. Without this the two are ~6371 km apart and every length() in the
                    // raymarch is wrong by that much.
                    "    vec3 ecef = (globe_atmo_local_to_ecef_y_up * vec4(VERTEX, 1.0)).xyz;\n"
                    "    v_ecef_position = ecef;\n"
                    // Geometric normal, i.e. the true ellipsoid normal rather than the vertex
                    // direction. The tiles' own normals come from the mesh and are unreliable at
                    // the seams; the reference derives it the same way, from the position:
                    //   normalize(vec3(p.x/A^2, p.y/C^2, p.z/B^2))
                    "    v_geo_normal = normalize(vec3(\n"
                    "        ecef.x / (globe_atmo_ellipsoid_radii.x * globe_atmo_ellipsoid_radii.x),\n"
                    "        ecef.y / (globe_atmo_ellipsoid_radii.y * globe_atmo_ellipsoid_radii.y),\n"
                    "        ecef.z / (globe_atmo_ellipsoid_radii.z * globe_atmo_ellipsoid_radii.z)));\n"
                    "    v_tile_uv = UV * u_uv_scale + u_uv_offset;\n"
                    "}\n"
                    "\n"
                    "void fragment() {\n"
                    "    vec3 albedo = u_base_color;\n"
                    "    if (u_has_texture) { albedo = texture(u_albedo_texture, v_tile_uv).rgb; }\n"
                    "    vec3 light_direction = dynamic_light_direction(v_ecef_position);\n"
                    // Diagnostic: 1 = raw fetch, 2 = raw fetch + sRGB decode. See Globe3D.h.
                    "    if (globe_atmo_debug_albedo > 0.5) {\n"
                    "        vec3 dbg = u_has_texture ? texture(u_albedo_texture, v_tile_uv).rgb\n"
                    "                                     : u_base_color;\n"
                    "        if (globe_atmo_debug_albedo > 1.5) { dbg = srgb_to_linear_approx(dbg); }\n"
                    "        ALBEDO = dbg;\n"
                    "    } else {\n"
                    // Reference (atmosphere.ts:424-426). The 5.0 and the 0.3 floor are a
                    // hand-tuned terminator softness, not physics: the floor keeps the night
                    // side from going fully black.
                    "    float day_lambert = dot(normalize(v_geo_normal), light_direction);\n"
                    "    float day_intensity = clamp(day_lambert * 5.0 + 0.3, 0.0, 1.0);\n"
                    "    day_intensity = mix(1.0, day_intensity,\n"
                    "                        globe_atmo_enable_lighting * globe_atmo_lighting_fade);\n"
                    "    vec3 lit_color = albedo * day_intensity;\n"
                    "    if (globe_atmo_ground_on > 0.5 && globe_atmo_lighting_fade > 0.001) {\n"
                    "        vec3 surface = project_to_ellipsoid(v_ecef_position);\n"
                    "        vec3 gr; vec3 gm; float go;\n"
                    "        ground_scattering(surface, light_direction, gr, gm, go);\n"
                    "        vec4 gac = compute_atmosphere_color(surface, light_direction, gr, gm, go,\n"
                    "                                            globe_atmo_ground_light_intensity);\n"
                    "        float transmittance = 0.5 + clamp(1.0 - gac.a, 0.0, 1.0);\n"
                    "        vec3 fa = lit_color + gac.rgb * transmittance;\n"
                    "        if (globe_atmo_enable_lighting > 0.5) {\n"
                    "            float sunlit_intensity = clamp((globe_atmo_camera_distance - 40000000.0) /\n"
                    "                (10000000.0 - 40000000.0), 0.05, 1.0);\n"
                    "            float darken = clamp(dot(normalize(surface), light_direction), 0.0, 1.0);\n"
                    "            fa = mix(mix(gac.rgb, fa, darken), fa, sunlit_intensity);\n"
                    "        }\n"
                    // The reference's exposure curve. This - not the shell's PBR-Neutral
                    // tonemap - is what lifts the veil from a radiometric ~0.05 into the
                    // display band, and it is applied to the composite (ground + air in front of
                    // it), so the imagery is lifted by the air rather than replaced by it.
                    "        fa = vec3(1.0) - exp(-2.0 * fa);\n"
                    "        ALBEDO = mix(lit_color, fa, globe_atmo_lighting_fade);\n"
                    "    } else {\n"
                    "        ALBEDO = lit_color;\n"
                    "    }\n"
                    "    }\n"
                    "}\n" );
                return shader;
            }

            /// Registers the globals if they are not there yet.
            ///
            /// A `global uniform` with no matching registration does not fall back to its
            /// declared default - the shader fails to compile, and it fails *silently*: the
            /// material renders untextured and every other check still passes. So this has to
            /// run before any material using either shader is created, which is why the two
            /// accessors below call it themselves rather than trusting the caller.
            ///
            /// Idempotence is tracked with a process-local flag rather than by asking the
            /// RenderingServer what it already has. `global_shader_parameter_get_list()` is an
            /// editor-only query: outside the editor it hits an ERR_FAIL and returns nothing,
            /// which reads as "none of them exist" and would re-add every global on every
            /// call. Re-adding is not harmless either - it resets the value to the default, so
            /// a per-frame publish() would be undone by the next ensure_globals() and the
            /// whole effect would silently stop tracking the camera.
            void add_global( const char *p_name, RenderingServer::GlobalShaderParameterType p_type,
                             const godot::Variant &p_default )
            {
                RenderingServer *rs = RenderingServer::get_singleton();
                if ( rs == nullptr )
                {
                    return;
                }
                rs->global_shader_parameter_add( godot::StringName( p_name ), p_type, p_default );
                g_globals_registered = true;
            }

        } // namespace

        void ensure_globals()
        {
            using Type = RenderingServer::GlobalShaderParameterType;
            // Guarded on the RenderingServer actually existing, not just on this being the
            // first call: a call that happens before the singleton is up must not consume the
            // one-shot.
            if ( g_globals_registered )
            {
                return;
            }
            if ( RenderingServer::get_singleton() == nullptr )
            {
                return;
            }
            // The reference's own defaults (createAtmosphereUniforms / createSharedUniforms).
            add_global( kViewerPosition, Type::GLOBAL_VAR_TYPE_VEC3, Vector3( 0.0f, 0.0f, 0.0f ) );
            add_global( kEllipsoidRadii, Type::GLOBAL_VAR_TYPE_VEC3,
                        Vector3( static_cast<float>( math::kWgs84SemiMajorAxis ),
                                 static_cast<float>( math::kWgs84SemiMinorAxis ),
                                 static_cast<float>( math::kWgs84SemiMajorAxis ) ) );
            add_global( kCameraHeight, Type::GLOBAL_VAR_TYPE_FLOAT, 0.0f );
            add_global( kCameraDistance, Type::GLOBAL_VAR_TYPE_FLOAT, 0.0f );
            add_global( kSunDirection, Type::GLOBAL_VAR_TYPE_VEC3, Vector3( 1.0f, 0.0f, 0.0f ) );
            add_global( kGroundOn, Type::GLOBAL_VAR_TYPE_FLOAT, 1.0f );
            add_global( kEnableLighting, Type::GLOBAL_VAR_TYPE_FLOAT, 1.0f );
            add_global( kLightingFade, Type::GLOBAL_VAR_TYPE_FLOAT, 1.0f );
            add_global( kGroundIntensity, Type::GLOBAL_VAR_TYPE_FLOAT, 10.0f );
            add_global( kShellIntensity, Type::GLOBAL_VAR_TYPE_FLOAT, 50.0f );
            add_global( kSunsetTint, Type::GLOBAL_VAR_TYPE_FLOAT, 0.0f );
            add_global( kTint, Type::GLOBAL_VAR_TYPE_VEC3, Vector3( 1.0f, 1.0f, 1.0f ) );
            add_global( kDebugPure, Type::GLOBAL_VAR_TYPE_FLOAT, 0.0f );
            add_global( kDebugAlbedo, Type::GLOBAL_VAR_TYPE_FLOAT, 0.0f );
            // Identity: correct only for a lone globe with no georeference, which is exactly
            // the case publish_frame() has nothing to correct. A frame is published before the
            // first material is built, so this default is never actually seen.
            add_global( kLocalToEcefYUpParam, Type::GLOBAL_VAR_TYPE_MAT4, Transform3D() );
        }

        const Ref<Shader> &shell_shader()
        {
            ensure_globals();
            return build_shell_shader();
        }

        const Ref<Shader> &ground_shader()
        {
            ensure_globals();
            return build_ground_shader();
        }

        void publish( const Vector3 &p_viewer_ecef_y_up, double p_camera_height,
                      double p_camera_distance, const Vector3 &p_sun_y_up,
                      double p_ground_light_intensity, double p_shell_light_intensity,
                      double p_lighting_fade, bool p_ground_atmosphere_on,
                      bool p_enable_lighting )
        {
            ensure_globals();
            RenderingServer *rs = RenderingServer::get_singleton();
            if ( rs == nullptr )
            {
                return;
            }
            rs->global_shader_parameter_set( kViewerPosition, p_viewer_ecef_y_up );
            rs->global_shader_parameter_set( kCameraHeight,
                                             static_cast<float>( p_camera_height ) );
            rs->global_shader_parameter_set( kCameraDistance,
                                             static_cast<float>( p_camera_distance ) );
            rs->global_shader_parameter_set( kSunDirection, p_sun_y_up );
            rs->global_shader_parameter_set( kGroundIntensity,
                                             static_cast<float>( p_ground_light_intensity ) );
            rs->global_shader_parameter_set( kShellIntensity,
                                             static_cast<float>( p_shell_light_intensity ) );
            rs->global_shader_parameter_set( kLightingFade,
                                             static_cast<float>( p_lighting_fade ) );
            rs->global_shader_parameter_set( kGroundOn, p_ground_atmosphere_on ? 1.0f : 0.0f );
            rs->global_shader_parameter_set( kEnableLighting, p_enable_lighting ? 1.0f : 0.0f );
        }

        Transform3D local_to_ecef_y_up( const GlobeFrame &p_frame )
        {
            // The inverse of Globe3D::apply_ecef_y_up_placement. That function maps a Y-up ECEF
            // point into the frame's local space; this maps a frame-local point back out into
            // the space the integral is calibrated in. The chain is
            //   local -> Z-up ECEF (the frame's inverse) -> Y-up ECEF (x, y, z) -> (x, z, -y)
            // which is the same flip GlobeFrame::camera_ecef_y_up applies, so a tile vertex and
            // the camera agree on where the Earth's centre is by construction.
            const glm::dmat3 linear( p_frame.local_to_ecef_ );
            const auto to_y_up = [&]( const math::Vec3 &z_up )
            {
                const math::Vec3 mapped = linear * z_up;
                return Vector3( static_cast<float>( mapped.x ), static_cast<float>( mapped.z ),
                                static_cast<float>( -mapped.y ) );
            };

            const Vector3 column_x = to_y_up( math::Vec3( 1.0, 0.0, 0.0 ) );
            const Vector3 column_y = to_y_up( math::Vec3( 0.0, 1.0, 0.0 ) );
            const Vector3 column_z = to_y_up( math::Vec3( 0.0, 0.0, 1.0 ) );

            // The frame's local origin, expressed in Y-up ECEF. Zero for a globe centred on the
            // ECEF origin, ~6.4e6 for one anchored on a georeference - which is the whole
            // reason this transform has to exist.
            const math::Vec3 origin_ecef_z_up = p_frame.to_ecef_z_up( math::Vec3( 0.0, 0.0, 0.0 ) );
            const Vector3 origin( static_cast<float>( origin_ecef_z_up.x ),
                                  static_cast<float>( origin_ecef_z_up.z ),
                                  static_cast<float>( -origin_ecef_z_up.y ) );

            return Transform3D( godot::Basis( column_x, column_y, column_z ), origin );
        }

        void publish_frame( const GlobeFrame &p_frame )
        {
            ensure_globals();
            RenderingServer *rs = RenderingServer::get_singleton();
            if ( rs == nullptr )
            {
                return;
            }
            rs->global_shader_parameter_set( kLocalToEcefYUpParam, local_to_ecef_y_up( p_frame ) );
        }

    } // namespace GlobeAtmosphereShading

} // namespace tiles3d
