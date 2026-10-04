// SPDX-License-Identifier: Unlicense

#include "Globe3D.h"

#include "GlobeAtmosphereShading.h"
#include "GlobeTileLayer.h"
#include "GodotMathConvert.h"

#include "core/math/GeoMath.h"
#include "core/math/Mat4.h"

#include "godot_cpp/classes/array_mesh.hpp"
#include "godot_cpp/classes/base_material3d.hpp"
#include "godot_cpp/classes/camera3d.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/image.hpp"
#include "godot_cpp/classes/image_texture.hpp"
#include "godot_cpp/classes/quad_mesh.hpp"
#include "godot_cpp/classes/rendering_server.hpp"
#include "godot_cpp/classes/scene_tree.hpp"
#include "godot_cpp/classes/shader.hpp"
#include "godot_cpp/classes/shader_material.hpp"
#include "godot_cpp/classes/standard_material3d.hpp"
#include "godot_cpp/classes/surface_tool.hpp"
#include "godot_cpp/classes/time.hpp"
#include "godot_cpp/classes/viewport.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/packed_int32_array.hpp"
#include "godot_cpp/variant/vector2.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

namespace tiles3d
{
    using godot::ArrayMesh;
    using godot::BaseMaterial3D;
    using godot::Callable;
    using godot::Camera3D;
    using godot::ClassDB;
    using godot::Color;
    using godot::D_METHOD;
    using godot::Engine;
    using godot::Image;
    using godot::ImageTexture;
    using godot::Mesh;
    using godot::PackedByteArray;
    using godot::PropertyInfo;
    using godot::QuadMesh;
    using godot::Ref;
    using godot::RenderingServer;
    using godot::Shader;
    using godot::ShaderMaterial;
    using godot::StandardMaterial3D;
    using godot::SurfaceTool;
    using godot::Texture2D;
    using godot::Variant;
    using godot::Vector2;
    using godot::Vector3;
    using godot::Vector2;
    using godot::Vector3;

    namespace
    {
        constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
        constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;

        /// How far above the ellipsoid's widest axis the graticule ring is drawn, so the
        /// lines are not z-fighting with the surface itself.
        constexpr double kGraticuleAltitude = 1000.0;

        /// The reference's SUN_GLOW_LENGTH_TS: the glow skirt's half-extent, measured in disc
        /// radii. Shared by the texture generator and the billboard scale so the two cannot
        /// drift apart - the skirt would then either be cut off by the quad or waste pixels.
        constexpr double kSunGlowLengthTs = 5.0;

        /// The reference's uEllipsoidRadii is (A, C, B): the polar radius sits on the *middle*
        /// component because the reference globe is Y-up, and the shader consumes it in that
        /// same order. Spelled out here so the C++ reads like the TypeScript.
        constexpr double kEllipsoidRadiiX = math::kWgs84SemiMajorAxis;
        constexpr double kEllipsoidRadiiY = math::kWgs84SemiMinorAxis;
        constexpr double kEllipsoidRadiiZ = math::kWgs84SemiMajorAxis;


        /// The sun glow texture, generated rather than shipped.
        ///
        /// A direct port of the reference `makeSunGlowTexture()` (atmosphere.ts), which itself
        /// is a port of Cesium's Sun.js sprite: a bright disc, a smooth radial skirt, and six
        /// rotated elliptical "bursts" that give the glow its non-circular, lens-flare-ish
        /// shape. Shipped as generated data because a GDExtension cannot reference a
        /// res:// texture (the host project is not ours), the same constraint that made the
        /// shaders string literals.
        Ref<ImageTexture> make_sun_glow_texture()
        {
            constexpr int kSize = 256;
            constexpr double kGlowLengthTs = kSunGlowLengthTs;
            // Both the disc and the glow are expressed in the texture's own 0..1 units, so the
            // radius the *sprite* has to be scaled to is derived by the caller from the same
            // constant. Keep the two in sync through kSunGlowLengthTs, never by hand.
            const double radius_ts = ( 1.0 / ( 1.0 + 2.0 * kGlowLengthTs ) ) * 0.5;
            const double length_scalar = 2.0 / std::sqrt( 2.0 );

            // (dir.x, dir.y, weight) for the six bursts, straight from Sun.js.
            constexpr double kBursts[6][3] = {
                { 0.38942, 0.92106, 0.4 },  { 0.99235, 0.12348, 0.4 },
                { 0.60327, -0.79754, 0.4 }, { 0.31457, 0.94924, 0.3 },
                { 0.97931, 0.20239, 0.3 },  { 0.66507, -0.74678, 0.3 },
            };

            const auto smooth_step = []( const double p_edge0, const double p_edge1,
                                         const double p_x )
            {
                double t = ( p_x - p_edge0 ) / ( p_edge1 - p_edge0 );
                t = t < 0.0 ? 0.0 : ( t > 1.0 ? 1.0 : t );
                return t * t * ( 3.0 - 2.0 * t );
            };
            const auto clamp01 = []( const double p_v )
            {
                return p_v < 0.0 ? 0.0 : ( p_v > 1.0 ? 1.0 : p_v );
            };

            PackedByteArray pixels;
            pixels.resize( kSize * kSize * 4 );

            for ( int j = 0; j < kSize; ++j )
            {
                for ( int i = 0; i < kSize; ++i )
                {
                    const double px = ( static_cast<double>( i ) + 0.5 ) / kSize - 0.5;
                    const double py = ( static_cast<double>( j ) + 0.5 ) / kSize - 0.5;
                    const double radius = std::sqrt( px * px + py * py ) * length_scalar;
                    const double surface = radius <= radius_ts ? 1.0 : 0.0;

                    double r = 1.0;
                    double g = 1.0;
                    double b = surface + 0.2;
                    double a = surface;

                    const double glow_add =
                        ( 1.0 - smooth_step( 0.0, 0.55, radius ) ) * 0.75;
                    b += glow_add;
                    a += glow_add;

                    double burst_r = 0.0;
                    double burst_g = 0.0;
                    double burst_b = 0.0;
                    double burst_a = 0.0;
                    for ( const auto &burst : kBursts )
                    {
                        const double bx = ( px * burst[0] - py * burst[1] ) * 25.0;
                        const double by = ( px * burst[1] + py * burst[0] ) * 0.75;
                        const double bv = burst[2] *
                                          ( 1.0 - smooth_step( 0.0, 0.55,
                                                              std::sqrt( bx * bx + by * by ) *
                                                                  length_scalar ) );
                        burst_r += bv;
                        burst_g += bv;
                        burst_b += bv;
                        burst_a += bv;
                    }
                    r += clamp01( burst_r ) * 0.15;
                    g += clamp01( burst_g ) * 0.15;
                    b += clamp01( burst_b ) * 0.15;
                    a += clamp01( burst_a ) * 0.15;

                    const int offset = ( j * kSize + i ) * 4;
                    pixels[offset + 0] = static_cast<std::uint8_t>( clamp01( r ) * 255.0 );
                    pixels[offset + 1] = static_cast<std::uint8_t>( clamp01( g ) * 255.0 );
                    pixels[offset + 2] = static_cast<std::uint8_t>( clamp01( b ) * 255.0 );
                    pixels[offset + 3] = static_cast<std::uint8_t>( clamp01( a ) * 255.0 );
                }
            }

            Ref<Image> image = Image::create_from_data( kSize, kSize, false, Image::FORMAT_RGBA8,
                                                        pixels );
            Ref<ImageTexture> texture;
            texture.instantiate();
            texture->set_image( image );
            return texture;
        }

        /// How far along the sun direction the glow billboard is placed, metres. Arbitrary and
        /// deliberately large: it only has to be far enough that the sprite's angular size is
        /// stable while the camera moves within the scene, and near enough to stay inside the
        /// camera's far plane (which GlobeCameraController grows to a few Earth radii, not to
        /// infinity). The sun's real distance is not representable here.
        constexpr double kSunSpriteDistance = 4.0e8;

        /// Real solar angular radius, radians: 6.955e8 m / 1.495978707e11 m. The reference
        /// derives it from SOLAR_RADIUS / SUN_DISTANCE for the same reason - so the glow is
        /// anchored to a physical number rather than to whatever looked right.
        constexpr double kSolarAngularRadius = 6.955e8 / 1.495978707e11;

        /// A mild, non-physical material for the surface while no texture is assigned. The
        /// globe is mostly lit by a DirectionalLight3D in the demo; keeping metallic at 0
        /// and roughness high stops the ellipsoid reading as a shiny ball.
    } // namespace

    Globe3D::Globe3D() = default;

    // Godot frees the child MeshInstance3D nodes through the scene tree; the destructor only
    // has to drop the raw pointers.
    Globe3D::~Globe3D() = default;

    void Globe3D::_bind_methods()
    {
        ClassDB::bind_method( D_METHOD( "set_radial_segments", "p_value" ),
                              &Globe3D::set_radial_segments );
        ClassDB::bind_method( D_METHOD( "get_radial_segments" ), &Globe3D::get_radial_segments );
        ClassDB::add_property( "Globe3D", PropertyInfo( Variant::INT, "radial_segments" ),
                               "set_radial_segments", "get_radial_segments" );

        ClassDB::bind_method( D_METHOD( "set_rings", "p_value" ), &Globe3D::set_rings );
        ClassDB::bind_method( D_METHOD( "get_rings" ), &Globe3D::get_rings );
        ClassDB::add_property( "Globe3D", PropertyInfo( Variant::INT, "rings" ), "set_rings",
                               "get_rings" );

        ClassDB::bind_method( D_METHOD( "set_base_color", "p_color" ), &Globe3D::set_base_color );
        ClassDB::bind_method( D_METHOD( "get_base_color" ), &Globe3D::get_base_color );
        ClassDB::add_property( "Globe3D", PropertyInfo( Variant::COLOR, "base_color" ),
                               "set_base_color", "get_base_color" );

        ClassDB::bind_method( D_METHOD( "set_albedo_texture", "p_texture" ),
                              &Globe3D::set_albedo_texture );
        ClassDB::bind_method( D_METHOD( "get_albedo_texture" ), &Globe3D::get_albedo_texture );
        ClassDB::add_property(
            "Globe3D",
            PropertyInfo( Variant::OBJECT, "albedo_texture", godot::PROPERTY_HINT_RESOURCE_TYPE,
                          "Texture2D" ),
            "set_albedo_texture", "get_albedo_texture" );

        ClassDB::bind_method( D_METHOD( "set_show_graticule", "p_value" ),
                              &Globe3D::set_show_graticule );
        ClassDB::bind_method( D_METHOD( "get_show_graticule" ), &Globe3D::get_show_graticule );
        ClassDB::add_property( "Globe3D", PropertyInfo( Variant::BOOL, "show_graticule" ),
                               "set_show_graticule", "get_show_graticule" );

        // Enum hint rather than a bool: the third state is the load-bearing one, and a
        // two-valued property cannot express "get out of the way when tiles are streaming".
        ClassDB::bind_method( D_METHOD( "set_show_surface", "p_value" ),
                              &Globe3D::set_show_surface );
        ClassDB::bind_method( D_METHOD( "get_show_surface" ), &Globe3D::get_show_surface );
        ClassDB::add_property(
            "Globe3D",
            PropertyInfo( Variant::INT, "show_surface", godot::PROPERTY_HINT_ENUM,
                          "Auto,-1,Never,0,Always,1" ),
            "set_show_surface", "get_show_surface" );

        ClassDB::bind_method( D_METHOD( "set_show_atmosphere", "p_value" ),
                              &Globe3D::set_show_atmosphere );
        ClassDB::bind_method( D_METHOD( "get_show_atmosphere" ), &Globe3D::get_show_atmosphere );
        ClassDB::add_property( "Globe3D", PropertyInfo( Variant::BOOL, "show_atmosphere" ),
                               "set_show_atmosphere", "get_show_atmosphere" );

        ClassDB::bind_method( D_METHOD( "set_atmosphere_scale", "p_value" ),
                              &Globe3D::set_atmosphere_scale );
        ClassDB::bind_method( D_METHOD( "get_atmosphere_scale" ),
                              &Globe3D::get_atmosphere_scale );
        ClassDB::add_property(
            "Globe3D",
            PropertyInfo( Variant::FLOAT, "atmosphere_scale", godot::PROPERTY_HINT_RANGE,
                          "1.0,1.2,0.001" ),
            "set_atmosphere_scale", "get_atmosphere_scale" );

        ClassDB::bind_method( D_METHOD( "set_atmosphere_color", "p_color" ),
                              &Globe3D::set_atmosphere_color );
        ClassDB::bind_method( D_METHOD( "get_atmosphere_color" ),
                              &Globe3D::get_atmosphere_color );
        ClassDB::add_property( "Globe3D", PropertyInfo( Variant::COLOR, "atmosphere_color" ),
                               "set_atmosphere_color", "get_atmosphere_color" );

        ClassDB::bind_method( D_METHOD( "set_atmosphere_intensity", "p_value" ),
                              &Globe3D::set_atmosphere_intensity );
        ClassDB::bind_method( D_METHOD( "get_atmosphere_intensity" ),
                              &Globe3D::get_atmosphere_intensity );
        // 0..200 rather than the old 0..5: the raymarch multiplies a 1e-5-scale scattering
        // coefficient by a density integral, so its natural operating point is ~50, not ~2.
        // A 5.0 ceiling (kept from the fresnel shell this replaced) silently clamped the
        // scene's value and made the halo look 9x fainter than the C++ default.
        ClassDB::add_property(
            "Globe3D",
            PropertyInfo( Variant::FLOAT, "atmosphere_intensity", godot::PROPERTY_HINT_RANGE,
                          "0.0,200.0,0.5" ),
            "set_atmosphere_intensity", "get_atmosphere_intensity" );

        ClassDB::bind_method( D_METHOD( "set_atmosphere_sunset_tint", "p_value" ),
                              &Globe3D::set_atmosphere_sunset_tint );
        ClassDB::bind_method( D_METHOD( "get_atmosphere_sunset_tint" ),
                              &Globe3D::get_atmosphere_sunset_tint );
        ClassDB::add_property(
            "Globe3D",
            PropertyInfo( Variant::FLOAT, "atmosphere_sunset_tint", godot::PROPERTY_HINT_RANGE,
                          "0.0,1.0,0.01" ),
            "set_atmosphere_sunset_tint", "get_atmosphere_sunset_tint" );

        ClassDB::bind_method( D_METHOD( "set_atmosphere_debug_pure", "p_value" ),
                              &Globe3D::set_atmosphere_debug_pure );
        ClassDB::bind_method( D_METHOD( "get_atmosphere_debug_pure" ),
                              &Globe3D::get_atmosphere_debug_pure );
        ClassDB::add_property( "Globe3D",
                               PropertyInfo( Variant::BOOL, "atmosphere_debug_pure" ),
                               "set_atmosphere_debug_pure", "get_atmosphere_debug_pure" );

        // Ground atmosphere. `ground_atmosphere` is the blue veil over the disc - the pass
        // that turns a lit sphere into a planet with air on it. The shell alone can only
        // brighten the ring outside the silhouette.
        ClassDB::bind_method( D_METHOD( "set_ground_atmosphere", "p_value" ),
                              &Globe3D::set_ground_atmosphere );
        ClassDB::bind_method( D_METHOD( "get_ground_atmosphere" ),
                              &Globe3D::get_ground_atmosphere );
        ClassDB::add_property( "Globe3D", PropertyInfo( Variant::BOOL, "ground_atmosphere" ),
                               "set_ground_atmosphere", "get_ground_atmosphere" );

        ClassDB::bind_method( D_METHOD( "set_ground_atmosphere_intensity", "p_value" ),
                              &Globe3D::set_ground_atmosphere_intensity );
        ClassDB::bind_method( D_METHOD( "get_ground_atmosphere_intensity" ),
                              &Globe3D::get_ground_atmosphere_intensity );
        // Range follows the reference's uGroundLightIntensity = 10.0 default, not the shell's
        // 50.0 - they are separate knobs because they are separate passes.
        ClassDB::add_property(
            "Globe3D",
            PropertyInfo( Variant::FLOAT, "ground_atmosphere_intensity",
                          godot::PROPERTY_HINT_RANGE, "0.0,60.0,0.5" ),
            "set_ground_atmosphere_intensity", "get_ground_atmosphere_intensity" );

        ClassDB::bind_method( D_METHOD( "set_atmosphere_ground_fade", "p_value" ),
                              &Globe3D::set_atmosphere_ground_fade );
        ClassDB::bind_method( D_METHOD( "get_atmosphere_ground_fade" ),
                              &Globe3D::get_atmosphere_ground_fade );
        ClassDB::add_property( "Globe3D", PropertyInfo( Variant::BOOL, "atmosphere_ground_fade" ),
                               "set_atmosphere_ground_fade", "get_atmosphere_ground_fade" );

        // Diagnostic, not a look knob. Paints the raw texture fetch on the tiles and drops
        // the scattering entirely, which is the only way to tell "the imagery is not
        // reaching the pixels" (a binding/UV fault in the material the shader replaced)
        // apart from "the imagery is there but the veil is too strong" (a magnitude fault
        // in the integral). No class of screenshot distinguishes them.
        ClassDB::bind_method( D_METHOD( "set_ground_debug_albedo", "p_value" ),
                              &Globe3D::set_ground_debug_albedo );
        ClassDB::bind_method( D_METHOD( "get_ground_debug_albedo" ),
                              &Globe3D::get_ground_debug_albedo );
        ClassDB::add_property( "Globe3D", PropertyInfo( Variant::INT, "ground_debug_albedo",
                                                     godot::PROPERTY_HINT_ENUM,
                                                     "Normal,0,Raw albedo,1,Decode only,2" ),
                               "set_ground_debug_albedo", "get_ground_debug_albedo" );

        ClassDB::bind_method( D_METHOD( "set_show_sun", "p_value" ), &Globe3D::set_show_sun );
        ClassDB::bind_method( D_METHOD( "get_show_sun" ), &Globe3D::get_show_sun );
        ClassDB::add_property( "Globe3D", PropertyInfo( Variant::BOOL, "show_sun" ),
                               "set_show_sun", "get_show_sun" );

        ClassDB::bind_method( D_METHOD( "set_sun_angular_scale", "p_value" ),
                              &Globe3D::set_sun_angular_scale );
        ClassDB::bind_method( D_METHOD( "get_sun_angular_scale" ),
                              &Globe3D::get_sun_angular_scale );
        ClassDB::add_property(
            "Globe3D",
            PropertyInfo( Variant::FLOAT, "sun_angular_scale", godot::PROPERTY_HINT_RANGE,
                          "0.1,10.0,0.01" ),
            "set_sun_angular_scale", "get_sun_angular_scale" );

        // NaN is the "derive from the clock" sentinel; PROPERTY_HINT_NONE with the default
        // NaN keeps the inspector honest (a range hint would clamp the sentinel on load).
        ClassDB::bind_method( D_METHOD( "set_sun_longitude_degrees", "p_value" ),
                              &Globe3D::set_sun_longitude_degrees );
        ClassDB::bind_method( D_METHOD( "get_sun_longitude_degrees" ),
                              &Globe3D::get_sun_longitude_degrees );
        ClassDB::add_property(
            "Globe3D", PropertyInfo( Variant::FLOAT, "sun_longitude_degrees" ),
            "set_sun_longitude_degrees", "get_sun_longitude_degrees" );

        ClassDB::bind_method( D_METHOD( "set_sun_latitude_degrees", "p_value" ),
                              &Globe3D::set_sun_latitude_degrees );
        ClassDB::bind_method( D_METHOD( "get_sun_latitude_degrees" ),
                              &Globe3D::get_sun_latitude_degrees );
        ClassDB::add_property( "Globe3D",
                               PropertyInfo( Variant::FLOAT, "sun_latitude_degrees" ),
                               "set_sun_latitude_degrees", "get_sun_latitude_degrees" );

        ClassDB::bind_method( D_METHOD( "get_sub_solar_point" ), &Globe3D::get_sub_solar_point );
        ClassDB::bind_method( D_METHOD( "get_sun_direction" ), &Globe3D::get_sun_direction );
        ClassDB::bind_method( D_METHOD( "get_sun_position" ), &Globe3D::get_sun_position );
        ClassDB::bind_method( D_METHOD( "compute_sun_direction_y_up" ),
                              &Globe3D::compute_sun_direction_y_up );

        ClassDB::bind_method( D_METHOD( "rebuild" ), &Globe3D::rebuild );
        ClassDB::bind_method( D_METHOD( "rebase" ), &Globe3D::rebase );

        ClassDB::bind_method( D_METHOD( "geodetic_to_local", "p_longitude_degrees",
                                        "p_latitude_degrees", "p_height" ),
                              &Globe3D::geodetic_to_local );
        ClassDB::bind_method( D_METHOD( "ecef_to_local", "p_ecef" ), &Globe3D::ecef_to_local );
        ClassDB::bind_method( D_METHOD( "local_to_geodetic", "p_local" ),
                              &Globe3D::local_to_geodetic );
    }

    void Globe3D::_notification( int p_what )
    {
        switch ( p_what )
        {
            case NOTIFICATION_READY:
                ensure_children();
                rebuild();
                // The atmosphere and the sun both follow the camera and the clock, so they
                // have to be refreshed per frame. Doing it in the shader with
                // CAMERA_POSITION_WORLD is not an option: the shell mesh is built in this
                // node's *local* frame while that built-in is world space, and the two differ
                // by the Georeference3D's Z-up -> Y-up flip.
                set_process( true );
                break;
            case NOTIFICATION_PROCESS:
                update_atmosphere_uniforms();
                update_sun_pose();
                // After the uniforms, because both this and the tile layer count as "covered"
                // decisions that must agree within a frame - see update_surface_visibility.
                update_surface_visibility();
                break;
            case NOTIFICATION_ENTER_TREE:
                // The frame depends on the ancestor chain; a re-parent must re-resolve it.
                frame_resolved_ = false;
                // Building in the editor means the ellipsoid is visible without running the
                // project, which is how the tile sets were validated too.
                if ( Engine::get_singleton() != nullptr && Engine::get_singleton()->is_editor_hint() )
                {
                    ensure_children();
                    rebuild();
                    // The editor never sends NOTIFICATION_PROCESS to a @tool-free extension,
                    // and the viewport camera moves freely there, so the one-shot build above
                    // would leave the atmosphere lit from wherever the sun was at load. The
                    // editor's own redraw drives this instead.
                    set_process( true );
                }
                break;
            default:
                break;
        }
    }

    void Globe3D::ensure_children()
    {
        if ( surface_ == nullptr )
        {
            surface_ = memnew( godot::MeshInstance3D );
            surface_->set_name( "Surface" );
            add_child( surface_, false, godot::Node::INTERNAL_MODE_FRONT );
        }
        if ( graticule_ == nullptr )
        {
            graticule_ = memnew( godot::MeshInstance3D );
            graticule_->set_name( "Graticule" );
            add_child( graticule_, false, godot::Node::INTERNAL_MODE_FRONT );
        }
        if ( atmosphere_ == nullptr )
        {
            atmosphere_ = memnew( godot::MeshInstance3D );
            atmosphere_->set_name( "Atmosphere" );
            // Drawn after the surface so it blends over it. INTERNAL_MODE_BACK keeps these
            // children out of the scene the user edits while still being drawn last.
            add_child( atmosphere_, false, godot::Node::INTERNAL_MODE_BACK );

            // The atmosphere is locked open: `sun_` is a billboard that lives in the camera's
            // half of the scene and must never sort in front of the shell. Forcing the shell
            // to render first (render_priority is Godot's per-instance sort override) removes
            // the depth tie between two transparent surfaces that have no meaningful order.
            //
            // Both nodes are transparent and neither writes depth, so without a cut the shell
            // can be drawn on top of a near sun and wash it out - which is exactly what the
            // screenshot showed: a glow with no visible disc.
            atmosphere_->set_transparency( 1.0f );
        }
        if ( sun_ == nullptr )
        {
            sun_ = memnew( godot::MeshInstance3D );
            sun_->set_name( "Sun" );
            // Drawn last of all, and *not* internal: a user may want to nudge or hide it, and
            // unlike the shell it has no build-time state to protect.
            add_child( sun_, true );
            sun_->set_transparency( 0.0f );
            sun_->set_cast_shadows_setting(
                godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF );
        }
    }

    // ---- appearance ----

    void Globe3D::set_radial_segments( const int p_value )
    {
        const int clamped = p_value < 3 ? 3 : p_value;
        if ( radial_segments_ != clamped )
        {
            radial_segments_ = clamped;
            rebuild();
        }
    }

    int Globe3D::get_radial_segments() const
    {
        return radial_segments_;
    }

    void Globe3D::set_rings( const int p_value )
    {
        const int clamped = p_value < 2 ? 2 : p_value;
        if ( rings_ != clamped )
        {
            rings_ = clamped;
            rebuild();
        }
    }

    int Globe3D::get_rings() const
    {
        return rings_;
    }

    void Globe3D::set_base_color( const Color &p_color )
    {
        if ( base_color_ != p_color )
        {
            base_color_ = p_color;
            update_materials();
        }
    }

    Color Globe3D::get_base_color() const
    {
        return base_color_;
    }

    void Globe3D::set_albedo_texture( const Ref<Texture2D> &p_texture )
    {
        if ( albedo_texture_ != p_texture )
        {
            albedo_texture_ = p_texture;
            update_materials();
        }
    }

    Ref<Texture2D> Globe3D::get_albedo_texture() const
    {
        return albedo_texture_;
    }

    void Globe3D::set_show_graticule( const bool p_value )
    {
        if ( show_graticule_ != p_value )
        {
            show_graticule_ = p_value;
            rebuild();
        }
    }
    bool Globe3D::get_show_graticule() const
    {
        return show_graticule_;
    }

    void Globe3D::set_show_surface( const int p_value )
    {
        // -1 auto / 0 never / 1 always. Anything else is a typo, and silently coercing it
        // would hide the planet for a scene that asked for something that does not exist.
        if ( p_value < -1 || p_value > 1 )
        {
            WARN_PRINT( "Globe3D: show_surface must be -1 (auto), 0 or 1; got " +
                        godot::String::num_int64( p_value ) + " - using auto." );
            show_surface_ = -1;
        }
        else
        {
            show_surface_ = p_value;
        }
        // Apply now rather than next frame: a script that sets this and immediately reads
        // pixels should not have to wait for the process tick to find out.
        surface_visible_applied_ = !show_surface_;
        update_surface_visibility();
    }

    int Globe3D::get_show_surface() const
    {
        return show_surface_;
    }

    // ---- atmosphere ----

    void Globe3D::set_show_atmosphere( const bool p_value )
    {
        if ( show_atmosphere_ != p_value )
        {
            show_atmosphere_ = p_value;
            rebuild_atmosphere();
        }
    }

    bool Globe3D::get_show_atmosphere() const
    {
        return show_atmosphere_;
    }

    void Globe3D::set_atmosphere_scale( const double p_value )
    {
        const double clamped = p_value < 1.0 ? 1.0 : p_value;
        if ( atmosphere_scale_ != clamped )
        {
            atmosphere_scale_ = clamped;
            rebuild_atmosphere();
        }
    }

    double Globe3D::get_atmosphere_scale() const
    {
        return atmosphere_scale_;
    }

    void Globe3D::set_atmosphere_color( const Color &p_color )
    {
        if ( atmosphere_color_ != p_color )
        {
            atmosphere_color_ = p_color;
            update_materials();
        }
    }

    Color Globe3D::get_atmosphere_color() const
    {
        return atmosphere_color_;
    }

    void Globe3D::set_atmosphere_intensity( const double p_value )
    {
        const double clamped = p_value < 0.0 ? 0.0 : p_value;
        if ( atmosphere_intensity_ != clamped )
        {
            atmosphere_intensity_ = clamped;
            update_materials();
        }
    }

    double Globe3D::get_atmosphere_intensity() const
    {
        return atmosphere_intensity_;
    }

    void Globe3D::set_atmosphere_sunset_tint( const double p_value )
    {
        const double clamped = p_value < 0.0 ? 0.0 : ( p_value > 1.0 ? 1.0 : p_value );
        if ( atmosphere_sunset_tint_ != clamped )
        {
            atmosphere_sunset_tint_ = clamped;
            update_materials();
        }
    }

    double Globe3D::get_atmosphere_sunset_tint() const
    {
        return atmosphere_sunset_tint_;
    }

    void Globe3D::set_atmosphere_debug_pure( const bool p_value )
    {
        if ( atmosphere_debug_pure_ != p_value )
        {
            atmosphere_debug_pure_ = p_value;
            update_materials();
        }
    }

    bool Globe3D::get_atmosphere_debug_pure() const
    {
        return atmosphere_debug_pure_;
    }

    void Globe3D::set_ground_debug_albedo( const int p_value )
    {
        // A global, not a material parameter: the tiles that actually draw the planet are built
        // by GlobeTileLayer, which has no back-reference to this node, so a per-material flag
        // set here would never reach them. Read every frame, so no refresh is needed.
        ground_debug_albedo_ = p_value < 0 ? 0 : ( p_value > 2 ? 2 : p_value );
    }

    int Globe3D::get_ground_debug_albedo() const
    {
        return ground_debug_albedo_;
    }

    // ---- ground atmosphere ----

    void Globe3D::set_ground_atmosphere( const bool p_value )
    {
        if ( ground_atmosphere_ != p_value )
        {
            ground_atmosphere_ = p_value;
            // Only the uniform changes, but it is read every frame from the same material,
            // so a refresh is what makes the change visible.
            update_materials();
        }
    }

    bool Globe3D::get_ground_atmosphere() const
    {
        return ground_atmosphere_;
    }

    void Globe3D::set_ground_atmosphere_intensity( const double p_value )
    {
        const double clamped = p_value < 0.0 ? 0.0 : p_value;
        if ( ground_atmosphere_intensity_ != clamped )
        {
            ground_atmosphere_intensity_ = clamped;
            update_materials();
        }
    }

    double Globe3D::get_ground_atmosphere_intensity() const
    {
        return ground_atmosphere_intensity_;
    }

    void Globe3D::set_atmosphere_ground_fade( const bool p_value )
    {
        if ( atmosphere_ground_fade_ != p_value )
        {
            atmosphere_ground_fade_ = p_value;
        }
    }

    bool Globe3D::get_atmosphere_ground_fade() const
    {
        return atmosphere_ground_fade_;
    }

    // ---- sun ----

    void Globe3D::set_show_sun( const bool p_value )
    {
        if ( show_sun_ != p_value )
        {
            show_sun_ = p_value;
            rebuild_sun();
        }
    }

    bool Globe3D::get_show_sun() const
    {
        return show_sun_;
    }

    void Globe3D::set_sun_angular_scale( const double p_value )
    {
        const double clamped = p_value < 0.1 ? 0.1 : p_value;
        if ( sun_angular_scale_ != clamped )
        {
            sun_angular_scale_ = clamped;
            rebuild_sun();
        }
    }

    double Globe3D::get_sun_angular_scale() const
    {
        return sun_angular_scale_;
    }

    void Globe3D::set_sun_longitude_degrees( const double p_value )
    {
        // NaN restores the clock-derived sub-solar point. It must NOT be stored: Godot's
        // property diff writes whenever value != default and NaN != NaN is always true, so a
        // stored NaN lands in every .tscn save (this is where those `nan` entries came from).
        // A stale `nan` in an existing scene hits this setter on load and falls back to the
        // clock, which is exactly what the sentinel used to mean.
        has_sun_longitude_override_ = !std::isnan( p_value );
        if ( has_sun_longitude_override_ )
        {
            sun_longitude_degrees_ = p_value;
        }
    }

    double Globe3D::get_sun_longitude_degrees() const
    {
        return sun_longitude_degrees_;
    }

    void Globe3D::set_sun_latitude_degrees( const double p_value )
    {
        has_sun_latitude_override_ = !std::isnan( p_value );
        if ( has_sun_latitude_override_ )
        {
            sun_latitude_degrees_ = p_value;
        }
    }

    double Globe3D::get_sun_latitude_degrees() const
    {
        return sun_latitude_degrees_;
    }

    Vector2 Globe3D::sub_solar_point_radians( const double p_unix_seconds )
    {
        // The reference computeSubSolarPoint (FullGlobe.ts:111), unmodified. Low-precision
        // solar position: mean longitude and mean anomaly driven linearly by days since
        // J2000.0, one equation-of-centre term, then the right ascension converted to a
        // longitude through Greenwich mean sidereal time. Good to ~0.01 deg, which is ~1 km
        // of terminator on the surface - invisible at globe scale, and identical to what the
        // web build produces for the same instant.
        constexpr double kRad = math::kPi / 180.0;
        const double n = p_unix_seconds / 86400.0 - 10957.5;
        const double l = kRad * ( 280.46 + 0.9856474 * n );
        const double g = kRad * ( 357.528 + 0.9856003 * n );
        const double lambda = l + kRad * 1.915 * std::sin( g ) + kRad * 0.02 * std::sin( 2.0 * g );
        const double eps = kRad * ( 23.439 - 0.0000004 * n );
        const double lat = std::asin( std::sin( eps ) * std::sin( lambda ) );
        const double ra = std::atan2( std::cos( eps ) * std::sin( lambda ), std::cos( lambda ) );
        const double gmst = kRad * ( 280.46061837 + 360.98564736629 * n );
        const double lon = std::fmod(
            std::fmod( ra - gmst, 2.0 * math::kPi ) + 3.0 * math::kPi, 2.0 * math::kPi ) -
                           math::kPi;
        return Vector2( static_cast<float>( lon ), static_cast<float>( lat ) );
    }

    Vector2 Globe3D::get_sub_solar_point() const
    {
        // Overrides win individually: setting only the longitude (to freeze the terminator at
        // a chosen meridian, say) still lets the clock supply the season-dependent latitude.
        double longitude_degrees = sun_longitude_degrees_;
        double latitude_degrees = sun_latitude_degrees_;

        if ( !has_sun_longitude_override_ || !has_sun_latitude_override_ )
        {
            // Seconds since the Unix epoch. Frozen while the editor is open: a terminator that
            // creeps across the globe makes every screenshot irreproducible, and the whole
            // point of the raymarch is that the day/night boundary is visible and comparable.
            const bool in_editor = Engine::get_singleton() != nullptr &&
                                   Engine::get_singleton()->is_editor_hint();
            const double now = in_editor
                                   ? 0.0
                                   : static_cast<double>( godot::Time::get_singleton()
                                                              ->get_unix_time_from_system() );
            const Vector2 radians = sub_solar_point_radians( now );
            if ( !has_sun_longitude_override_ )
            {
                longitude_degrees = static_cast<double>( radians.x ) * kRadiansToDegrees;
            }
            if ( !has_sun_latitude_override_ )
            {
                latitude_degrees = static_cast<double>( radians.y ) * kRadiansToDegrees;
            }
        }

        return Vector2( static_cast<float>( longitude_degrees ),
                        static_cast<float>( latitude_degrees ) );
    }

    Vector3 Globe3D::compute_sun_direction_y_up() const
    {
        const Vector2 radians = get_sub_solar_point();
        const double lon = static_cast<double>( radians.x ) * kDegreesToRadians;
        const double lat = static_cast<double>( radians.y ) * kDegreesToRadians;

        // The reference sets uSunDirWC = (cos lat cos lon, sin lat, -cos lat sin lon): a
        // *unit direction* in Y-up ECEF, Y being the polar axis, with the negative sign on Z
        // so that east is +X and the handedness matches cartographicToXYZ. Mathematically it
        // is `normalize(geodeticToYUp(lon, lat, 0))`, but written directly because the
        // height is irrelevant and the kernel helper takes radians with a height.
        return Vector3( static_cast<float>( std::cos( lat ) * std::cos( lon ) ),
                        static_cast<float>( std::sin( lat ) ),
                        static_cast<float>( -std::cos( lat ) * std::sin( lon ) ) );
    }

    Vector3 Globe3D::get_sun_direction() const
    {
        // Y-up ECEF -> this node's local space. Going through local_direction() rather than
        // using the ECEF vector raw is the entire point: under a Georeference3D the local
        // frame is the ENU tangent frame, so a raw ECEF direction would point the terminator
        // 90 degrees off from the atmosphere it is supposed to light.
        const Vector3 y_up = compute_sun_direction_y_up();
        const math::Vec3 local = frame().local_direction(
            math::Vec3( static_cast<double>( y_up.x ), static_cast<double>( y_up.y ),
                        static_cast<double>( y_up.z ) ) );
        return Vector3( static_cast<float>( local.x ), static_cast<float>( local.y ),
                        static_cast<float>( local.z ) )
            .normalized();
    }

    Vector3 Globe3D::get_sun_position() const
    {
        return get_sun_direction() * static_cast<float>( kSunSpriteDistance );
    }

    void Globe3D::rebuild()
    {
        // The frame is cached and only invalidated on ENTER_TREE, but it is not only
        // re-parenting that moves it: Georeference3D's origin authority can be re-pointed at
        // runtime (the demo switches datasets, and the anchor has to follow the dataset or the
        // content lands 17 000 km away in a float32 range where the quantisation is a metre).
        // rebuild() is the one call every re-mesh path goes through, so re-resolve here rather
        // than making every caller remember to.
        frame_resolved_ = false;
        ensure_children();
        rebuild_surface();
        rebuild_graticule();
        rebuild_atmosphere();
        rebuild_sun();
        // One placement covers all three ECEF-authored meshes. It must also run on the
        // very first build: freshly created children sit at an identity transform, which
        // would draw the surface around this node's origin (i.e. inside the ground) rather
        // than at the frame's ellipsoid centre.
        apply_ecef_y_up_placement();
        update_materials();
        // Force the visibility decision to be re-applied: rebuild() can be called from a
        // property setter, and set_show_surface() is not the only path that changes the
        // answer.
        surface_visible_applied_ = !show_surface_;
        update_surface_visibility();
    }

    void Globe3D::rebase()
    {
        // The frame is cached here (resolve() walks the ancestor chain for a Georeference3D)
        // and the georeference invalidated its own cached matrices when it moved, so this
        // node's copy has to go too or the rebase would be invisible to it.
        frame_resolved_ = false;

        // Every mesh this node draws is authored in Y-up ECEF (see rebuild_surface), so an
        // origin move touches none of them: ECEF coordinates are frame-independent by
        // definition, and what changed is only where ECEF lands in local space. One
        // transform write per child *is* the whole update - no SurfaceTool, no vertex
        // rewrite, no GPU upload. The measured 32.85 ms median this replaces was the
        // controller's origin-shift threshold being a distance rather than "every frame"
        // for a reason that no longer exists.
        apply_ecef_y_up_placement();

        // Shading reads the frame and the viewer position, both of which just changed.
        // update_atmosphere_uniforms() publishes the frame for the tile material as well, so
        // a rebase does not leave one frame of atmosphere computed against the old origin.
        update_atmosphere_uniforms();
    }

    void Globe3D::rebuild_surface()
    {
        if ( surface_ == nullptr )
        {
            return;
        }

        // The reference globe is a Y-up ECEF sphere: +Y is the north pole. The mesh is built
        // in that space directly (docs/GLOBE_PLAN.md 3.3 - the surface never goes through the
        // ENU tangent plane), so Godot sees exactly what the reference draws.
        //
        // Vertex (i, j): i indexes longitude 0..radial_segments (wrapping), j indexes
        // latitude 0..rings (pole to pole). A row of duplicated pole vertices is emitted so
        // the UV seam and the pole caps behave; the alternative (a single pole vertex) breaks
        // the equirectangular UVs.
        const int columns = radial_segments_;
        const int rows = rings_;

        // SurfaceTool derives from RefCounted, so it must be created through instantiate().
        // A stack instance trips godot-cpp's "created without binding callbacks" guard.
        Ref<SurfaceTool> tool;
        tool.instantiate();
        tool->begin( Mesh::PRIMITIVE_TRIANGLES );

        const auto vertex_at = [&]( const int column, const int row )
        {
            // Longitude runs west (-pi) to east (+pi) as the column index grows, which is
            // what makes the equirectangular texture land the right way round.
            const double longitude =
                -math::kPi + 2.0 * math::kPi * static_cast<double>( column ) /
                                 static_cast<double>( columns );
            const double latitude =
                math::kPi / 2.0 - math::kPi * static_cast<double>( row ) /
                                      static_cast<double>( rows );
            // Y-up ECEF metres, the same authoring space the atmosphere shell uses: the
            // flip is the same (x, y, z) -> (x, z, -y) rotation (det = +1, so the winding
            // below survives), and the node transform set by apply_ecef_y_up_placement()
            // does the placing. Baking frame-local coordinates instead would make every
            // origin move a full re-mesh; ECEF is frame-independent by definition, so a
            // rebase costs one transform write.
            const math::Vec3 ecef = math::wgs84ToCartesian( longitude, latitude, 0.0 );
            return Vector3( static_cast<float>( ecef.x ), static_cast<float>( ecef.z ),
                            static_cast<float>( -ecef.y ) );
        };

        const auto uv_at = [&]( const int column, const int row )
        {
            const float u = static_cast<float>( column ) / static_cast<float>( columns );
            const float v = static_cast<float>( row ) / static_cast<float>( rows );
            return Vector2( u, v );
        };

        for ( int row = 0; row < rows; ++row )
        {
            for ( int column = 0; column < columns; ++column )
            {
                const int next_column = column + 1;
                const int next_row = row + 1;

                // Quad corners, counter-clockwise seen from outside.
                const Vector3 tl = vertex_at( column, row );
                const Vector3 tr = vertex_at( next_column, row );
                const Vector3 bl = vertex_at( column, next_row );
                const Vector3 br = vertex_at( next_column, next_row );

                const Vector2 uv_tl = uv_at( column, row );
                const Vector2 uv_tr = uv_at( next_column, row );
                const Vector2 uv_bl = uv_at( column, next_row );
                const Vector2 uv_br = uv_at( next_column, next_row );

                // Winding: Godot's default front face is clockwise when seen from the front,
                // and the surface normal points outward, so the order below keeps the visible
                // side facing out (same fix the Mars globe needed for its skirts).
                tool->set_uv( uv_tl );
                tool->add_vertex( tl );
                tool->set_uv( uv_bl );
                tool->add_vertex( bl );
                tool->set_uv( uv_br );
                tool->add_vertex( br );

                tool->set_uv( uv_tl );
                tool->add_vertex( tl );
                tool->set_uv( uv_br );
                tool->add_vertex( br );
                tool->set_uv( uv_tr );
                tool->add_vertex( tr );
            }
        }

        // Normals point away from the ellipsoid centre; the generated surface is closed and
        // convex, so the default (outward) normal generation is what we want.
        tool->generate_normals( false );

        const Ref<ArrayMesh> mesh = tool->commit();
        surface_->set_mesh( mesh );
    }

    void Globe3D::rebuild_graticule()
    {
        if ( graticule_ == nullptr )
        {
            return;
        }

        if ( !show_graticule_ )
        {
            graticule_->set_mesh( Ref<ArrayMesh >() );
            return;
        }

        // A coarse latitude/longitude grid, drawn slightly above the surface so it does not
        // z-fight. It exists so the globe is legible before any texture pipeline lands, and
        // is the cheap way to confirm the 0 degree meridian and the equator are where they
        // should be. Heap-allocated for the same RefCounted reason as the surface tool.
        Ref<SurfaceTool> tool;
        tool.instantiate();
        tool->begin( Mesh::PRIMITIVE_LINES );

        // Same Y-up ECEF authoring as the surface (see rebuild_surface): the lines are
        // placed by the node transform, never baked into the frame, so an origin move does
        // not re-derive them.
        const auto to_ecef_y_up = [&]( const double longitude, const double latitude,
                                       const double height )
        {
            const math::Vec3 ecef = math::wgs84ToCartesian( longitude, latitude, height );
            return Vector3( static_cast<float>( ecef.x ), static_cast<float>( ecef.z ),
                            static_cast<float>( -ecef.y ) );
        };

        const auto add_ring = [&]( const double latitude_degrees )
        {
            const double latitude = latitude_degrees * kDegreesToRadians;
            for ( int i = 0; i < 360; ++i )
            {
                const double lon0 = ( i - 180.0 ) * kDegreesToRadians;
                const double lon1 = ( i + 1 - 180.0 ) * kDegreesToRadians;
                tool->add_vertex( to_ecef_y_up( lon0, latitude, kGraticuleAltitude ) );
                tool->add_vertex( to_ecef_y_up( lon1, latitude, kGraticuleAltitude ) );
            }
        };

        for ( int latitude_degrees = -60; latitude_degrees <= 60; latitude_degrees += 30 )
        {
            add_ring( latitude_degrees );
        }

        const auto add_meridian = [&]( const double longitude_degrees )
        {
            const double longitude = longitude_degrees * kDegreesToRadians;
            for ( int i = -90; i < 90; ++i )
            {
                const double lat0 = i * kDegreesToRadians;
                const double lat1 = ( i + 1 ) * kDegreesToRadians;
                tool->add_vertex( to_ecef_y_up( longitude, lat0, kGraticuleAltitude ) );
                tool->add_vertex( to_ecef_y_up( longitude, lat1, kGraticuleAltitude ) );
            }
        };

        for ( int longitude_degrees = -150; longitude_degrees <= 180; longitude_degrees += 30 )
        {
            add_meridian( longitude_degrees );
        }

        // No generate_normals here: it only applies to triangle primitives and would log an
        // engine error for this line list. Line geometry is drawn unshaded anyway.
        graticule_->set_mesh( tool->commit() );
    }

    void Globe3D::rebuild_atmosphere()
    {
        if ( atmosphere_ == nullptr )
        {
            return;
        }

        if ( !show_atmosphere_ )
        {
            atmosphere_->set_mesh( Ref<ArrayMesh >() );
            return;
        }

        // A shell large enough to contain the scattering volume the shader integrates
        // (ATMOSPHERE_THICKNESS = 111 km above the surface). Built from the same geodetic grid
        // so it tracks the ellipsoid exactly.
        //
        // The winding is the opposite way round from the surface, which - together with the
        // shader's `cull_back` - leaves the *far* half of the shell visible. That is
        // deliberate and matches the reference's `side: THREE.BackSide`: the shell then sits
        // behind the planet, the depth test hides the part that would cover the disc, and
        // what remains on screen is the ring where the atmosphere spills past the silhouette.
        // The integral itself does not care which half is drawn - a view ray is sampled
        // identically either way - but the compositing does.
        const int columns = radial_segments_;
        const int rows = rings_;

        // The shell's vertices are emitted in **Y-up ECEF**, and the node's own transform is
        // then set so that those coordinates land where they belong in the scene.
        //
        // The alternative - baking each vertex straight into local space - cannot work for
        // this shader: the integral calls length(vertex) and expects metres from the Earth's
        // *centre*, and a Georeference3D's local origin sits on the surface. Emitting ECEF
        // and letting the transform place the shell satisfies both at once, with no
        // per-vertex conversion, and it keeps the vertex values at Earth-radius magnitude
        // (uniform for the float precision, and identical to what the reference feeds
        // three.js). Placement is applied by rebuild()/rebase(), which now position the
        // surface and the graticule through the same transform - this mesh has no special
        // status any more, it was just the first one authored this way.

        Ref<SurfaceTool> tool;
        tool.instantiate();
        tool->begin( Mesh::PRIMITIVE_TRIANGLES );

        const auto vertex_at = [&]( const int column, const int row )
        {
            const double longitude = -math::kPi + 2.0 * math::kPi * static_cast<double>( column ) /
                                                    static_cast<double>( columns );
            const double latitude = math::kPi / 2.0 - math::kPi * static_cast<double>( row ) /
                                                          static_cast<double>( rows );
            // Y-up ECEF, exactly as atmo_vs.glsl receives it: a unit-scaled sphere whose
            // vertices are already the ECEF metres the raymarch measures against.
            const math::Vec3 ecef_z_up =
                math::wgs84ToCartesian( longitude, latitude, 0.0 ) * atmosphere_scale_;
            return Vector3( static_cast<float>( ecef_z_up.x ), static_cast<float>( ecef_z_up.z ),
                            static_cast<float>( -ecef_z_up.y ) );
        };

        for ( int row = 0; row < rows; ++row )
        {
            for ( int column = 0; column < columns; ++column )
            {
                const Vector3 tl = vertex_at( column, row );
                const Vector3 tr = vertex_at( column + 1, row );
                const Vector3 bl = vertex_at( column, row + 1 );
                const Vector3 br = vertex_at( column + 1, row + 1 );

                // Winding matches the surface's outward convention (tl, bl, br / tl, br, tr)
                // so the shell's faces point *outward*.
                //
                // This is load-bearing with `cull_back`: an inward-wound shell is back-facing
                // everywhere the camera can see it, so the rasteriser culls every fragment
                // outside the planet's silhouette and the halo disappears entirely - while
                // still reporting as "loaded", with correct geometry, a live material and a
                // correct AABB. Every numeric check except this one passed on the broken
                // build. The far half still composites correctly because the depth test
                // rejects it against the planet.
                tool->add_vertex( tl );
                tool->add_vertex( bl );
                tool->add_vertex( br );

                tool->add_vertex( tl );
                tool->add_vertex( br );
                tool->add_vertex( tr );
            }
        }

        // generate_normals() would recompute outward normals for the enlarged shell. The
        // shader no longer uses NORMAL (the raymarch derives everything from the view ray),
        // but leaving it unset would make the mesh unusable for a quick diagnostic material
        // swap. The shell is convex and closed, so the default outward result is correct.
        tool->generate_normals( false );

        atmosphere_->set_mesh( tool->commit() );
    }

    void Globe3D::rebuild_sun()
    {
        if ( sun_ == nullptr )
        {
            return;
        }

        if ( !show_sun_ )
        {
            sun_->set_mesh( Ref<ArrayMesh >() );
            return;
        }

        // A camera-facing quad, scaled so that it subtends the sun's angular size plus the
        // glow skirt, and repositioned every frame along the sub-solar direction.
        //
        // Three properties make this read as a sun rather than a smear, and all three are in
        // the reference for the same reason:
        //
        // 1. It is pinned to the camera, not placed at a fixed world position, so it has *zero
        //    parallax* and a constant angular size. A sun at a finite world position would
        //    visibly slide against the stars when the camera orbits - an artefact nobody can
        //    name but everybody notices.
        // 2. The angular size is derived from the real solar radius rather than picked as
        //    "looks about right", so it stays correct if the glow factor changes.
        // 3. depth_draw_never + depth_test on: the sun is hidden when the planet is in front
        //    of it (a genuinely useful cue while checking that the terminator and the sun
        //    agree) but never occludes anything itself.
        if ( sun_texture_.is_null() )
        {
            sun_texture_ = make_sun_glow_texture();
        }

        sun_quad_.instantiate();
        // Size is set per frame; a placeholder keeps the mesh valid for the first draw.
        sun_quad_->set_size( Vector2( 1.0f, 1.0f ) );
        sun_->set_mesh( sun_quad_ );
    }

    void Globe3D::update_materials()
    {
        if ( surface_ != nullptr )
        {
            const bool has_texture = albedo_texture_.is_valid();
            // The surface carries the *ground atmosphere* pass - the blue veil over the disc.
            //
            // It shares GlobeAtmosphereShading::ground_shader() with the streamed tiles, which
            // is where the effect actually has to be: once GlobeTileLayer is present it covers
            // this mesh completely, so a pass that lives only here is invisible. This node
            // keeps it for the no-imagery case, where the ellipsoid *is* the visible surface.
            Ref<ShaderMaterial> material;
            material.instantiate();
            material->set_shader( GlobeAtmosphereShading::ground_shader() );
            material->set_shader_parameter( "u_has_texture", has_texture );
            // This mesh's vertices are Y-up ECEF (see rebuild_surface), not frame-local the
            // way the streamed tiles' RTC vertices are. The branch skips the frame matrix
            // for this material only; leaving it off would translate every vertex by the
            // ellipsoid centre and draw the ground pass six million metres underground.
            material->set_shader_parameter( "u_ecef_vertices", true );
            if ( has_texture )
            {
                material->set_shader_parameter( "u_albedo_texture", albedo_texture_ );
            }
            else
            {
                material->set_shader_parameter( "u_base_color",
                                                Vector3( base_color_.r, base_color_.g,
                                                         base_color_.b ) );
            }
            // Everything per-frame (viewer, height, sun, fade) arrives through global shader
            // parameters; see update_atmosphere_uniforms().
            surface_->set_material_override( material );
        }

        if ( graticule_ != nullptr )
        {
            Ref<StandardMaterial3D> material;
            material.instantiate();
            material->set_albedo( Color( 1.0f, 1.0f, 1.0f, 0.35f ) );
            material->set_transparency( BaseMaterial3D::TRANSPARENCY_ALPHA );
            material->set_shading_mode( BaseMaterial3D::SHADING_MODE_UNSHADED );
            material->set_cull_mode( BaseMaterial3D::CULL_DISABLED );
            graticule_->set_material_override( material );
        }

        if ( atmosphere_ != nullptr && atmosphere_->get_mesh().is_valid() )
        {
            // The raymarch. StandardMaterial3D has no notion of a volumetric integral, so
            // this is the one place the globe needs a real shader. It is compiled from a
            // string rather than shipped as a .gdshader file because the extension should be
            // self-contained - a demo project that just drops in addons/ must not have to
            // copy resource files alongside it.
            Ref<ShaderMaterial> material;
            material.instantiate();
            material->set_shader( GlobeAtmosphereShading::shell_shader() );
            atmosphere_->set_material_override( material );
        }

        if ( sun_ != nullptr && sun_quad_.is_valid() )
        {
            Ref<StandardMaterial3D> material;
            material.instantiate();
            material->set_transparency( BaseMaterial3D::TRANSPARENCY_ALPHA );
            material->set_shading_mode( BaseMaterial3D::SHADING_MODE_UNSHADED );
            material->set_cull_mode( BaseMaterial3D::CULL_DISABLED );
            material->set_texture( BaseMaterial3D::TEXTURE_ALBEDO, sun_texture_ );
            // The glow texture is authored in sRGB; without this Godot treats it as linear and
            // the disc comes out dim and grey rather than white-hot.
            material->set_texture_filter( BaseMaterial3D::TEXTURE_FILTER_LINEAR );
            material->set_albedo( Color( 1.0f, 1.0f, 1.0f, 1.0f ) );
            // Nothing about a sun should be tonemapped or fogged; it is the brightest thing in
            // the scene and stays that way.
            material->set_flag( BaseMaterial3D::FLAG_DISABLE_FOG, true );
            material->set_flag( BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, false );
            sun_->set_material_override( material );
        }
    }

    // ---- per-frame atmosphere / sun update ----

    void Globe3D::update_sun_pose()
    {
        if ( sun_ == nullptr || sun_quad_.is_null() )
        {
            return;
        }

        Node3D *camera = find_camera();
        if ( camera == nullptr )
        {
            return;
        }

        // A DirectionalLight-style placement would put the sun at a fixed world position, and
        // at 4e8 m that is *not* far enough to be parallax-free: orbiting the globe would make
        // the sun visibly slide. Pinning it to the camera along the sub-solar direction gives
        // zero parallax and a constant angular size, which is what the reference does too.
        //
        // The pin distance is clamped inside the camera's far plane: kSunSpriteDistance
        // (4e8 m) exceeds the demo's far = 2e8, and a sprite beyond the far plane is clipped
        // into invisibility - the demo ran for its whole life with no sun billboard at all,
        // and the fuzzy blob people saw in its place was the ProceduralSkyMaterial's own
        // procedural glare for the scene's DirectionalLight3D. Half the far plane keeps a
        // safety margin; the quad size below uses the same distance, so the *angular* size
        // is unchanged by the clamp.
        //
        // The sun is a child of this node, but its transform is set in *global* space because
        // its position is defined relative to the camera. set_global_transform() handles the
        // conversion, and the basis is taken from the camera so the quad always faces it - the
        // QuadMesh normal is +Z, so any other basis shows the sprite edge-on as a thin line.
        const Vector3 direction = get_sun_direction();
        const Vector3 camera_global = camera->get_global_position();
        double pin_distance = kSunSpriteDistance;
        if ( const Camera3D *camera3d = godot::Object::cast_to<Camera3D>( camera ) )
        {
            pin_distance = std::min( pin_distance,
                                     static_cast<double>( camera3d->get_far() ) * 0.5 );
        }
        const Vector3 centre_global =
            camera_global + direction * static_cast<float>( pin_distance );

        // Angular size: 2*tan(theta)*distance, where theta spans the disc plus its glow skirt.
        // SUN_GLOW_LENGTH_TS is the skirt's half-extent measured in disc radii, hence the
        // (1 + 2*ts) factor - the same expression as the reference.
        const double angular_radius = kSolarAngularRadius * sun_angular_scale_;
        const double size = 2.0 * std::tan( angular_radius ) *
                            ( 1.0 + 2.0 * kSunGlowLengthTs ) * pin_distance;
        sun_quad_->set_size( Vector2( static_cast<float>( size ), static_cast<float>( size ) ) );

        sun_->set_global_transform(
            godot::Transform3D( camera->get_global_basis(), centre_global ) );
    }

    godot::Node3D *Globe3D::find_camera() const
    {
        // get_viewport()->get_camera_3d() is right in both worlds: at runtime it is the
        // scene's active camera, and in the editor it is whichever 3D viewport camera is being
        // looked through. That covers every case this node actually runs in, so there is no
        // need to search the tree - and searching it would mean calling find_child on the
        // SceneTree root, which is a Window (an incomplete type in godot-cpp's public headers)
        // and would pull in window.hpp just to name the pointer.
        const godot::Viewport *viewport = get_viewport();
        if ( viewport == nullptr )
        {
            return nullptr;
        }
        return viewport->get_camera_3d();
    }

    void Globe3D::apply_ecef_y_up_placement()
    {
        if ( surface_ == nullptr && graticule_ == nullptr && atmosphere_ == nullptr )
        {
            return;
        }

        // Maps a Y-up ECEF point (vx, vy, vz) onto this node's local space, as a Godot
        // transform. The chain is Y-up ECEF -> Z-up ECEF -> frame local, and the frame's
        // matrices are kernel Mat4 (column-major, double). Building it as a Basis rather than
        // three nested conversions means the vertices stay in ECEF metres and only the node
        // transform does any work.
        //
        // Y-up -> Z-up ECEF is (vx, vy, vz) -> (vx, -vz, vy): the kernel's own z_up_to_y_up is
        // (vx, vy, vz) -> (vx, vz, -vy), so its inverse is what appears here.
        const math::Mat4 &ecef_to_local = frame().ecef_to_local_;

        // Compose: local = ecef_to_local * y_up_to_z_up * y_up_point. Godot's Basis columns
        // are the images of the basis vectors, which is exactly the linear part we want.
        const auto to_local_dir = [&]( const double p_x, const double p_y, const double p_z )
        {
            const math::Vec3 z_up( p_x, -p_z, p_y );
            const glm::dmat3 linear( ecef_to_local );
            const math::Vec3 mapped = linear * z_up;
            return Vector3( static_cast<float>( mapped.x ), static_cast<float>( mapped.y ),
                            static_cast<float>( mapped.z ) );
        };

        const Vector3 column_x = to_local_dir( 1.0, 0.0, 0.0 );
        const Vector3 column_y = to_local_dir( 0.0, 1.0, 0.0 );
        const Vector3 column_z = to_local_dir( 0.0, 0.0, 1.0 );

        // The ECEF origin (the Earth's centre) in local space. Constant term of the map.
        const math::Vec3 origin_local = frame().ellipsoid_center_local();
        const Vector3 origin( static_cast<float>( origin_local.x ),
                              static_cast<float>( origin_local.y ),
                              static_cast<float>( origin_local.z ) );

        const godot::Transform3D placement(
            godot::Basis( column_x, column_y, column_z ), origin );

        // All three ECEF-authored meshes share this one transform - this *is* the rebase:
        // the meshes never move, the frame landing does. Three transform writes, no vertex
        // touched, which is what turns the origin shift's dominant cost (the 31 ms
        // SurfaceTool rebuild it replaced) into a rounding error. The sun is deliberately
        // absent: it is a camera-pinned billboard placed via set_global_transform() every
        // frame and has no ECEF geometry.
        if ( surface_ != nullptr )
        {
            surface_->set_transform( placement );
        }
        if ( graticule_ != nullptr )
        {
            graticule_->set_transform( placement );
        }
        if ( atmosphere_ != nullptr )
        {
            atmosphere_->set_transform( placement );
        }
    }

    Vector3 Globe3D::get_camera_ecef_y_up() const
    {
        const Node3D *camera = find_camera();
        if ( camera == nullptr )
        {
            // No camera: the only finite answer that keeps the integral well-formed is the
            // Earth's centre. The height uniform is already clamped to >= 0 on the caller's
            // side, so this cannot produce a division by zero.
            return Vector3();
        }

        // World -> this node's local space -> Z-up ECEF -> Y-up ECEF, i.e. the exact inverse
        // of how the shell's vertices were built. Composing the two frames by hand is what
        // produces the "correct distances, wrong direction" class of bug documented on
        // GlobeCameraController; going through the transforms keeps one code path.
        const Vector3 world = camera->get_global_position();
        const Vector3 local = get_global_transform().affine_inverse().xform( world );
        const math::Vec3 ecef_z_up = frame().to_ecef_z_up(
            math::Vec3( static_cast<double>( local.x ), static_cast<double>( local.y ),
                        static_cast<double>( local.z ) ) );
        // Z-up ECEF (x, y, z) -> Y-up ECEF (x, z, -y). The kernel's `geodeticToYUp` emits
        // exactly this, so the sign on the third component is not incidental.
        return Vector3( static_cast<float>( ecef_z_up.x ), static_cast<float>( ecef_z_up.z ),
                        static_cast<float>( -ecef_z_up.y ) );
    }

    void Globe3D::update_atmosphere_uniforms()
    {
        if ( atmosphere_ == nullptr )
        {
            return;
        }

        // The integral is computed in **Y-up ECEF**, not in this node's local space, and that
        // choice is the whole reason this function exists.
        //
        // The shader calls length(globe_atmo_viewer_position) and length(sample_position) and
        // treats the answers as "metres from the Earth's centre" - which is how the reference's
        // GLSL is written, and how the Rayleigh/Mie exponentials are calibrated. That identity
        // only holds where the origin *is* the centre. A Globe3D under a Georeference3D is
        // built in an ENU frame anchored on the surface, so its origin is ~6371 km off-centre
        // and every length() in the integral would be wrong by that much: the atmosphere
        // collapses to a near-invisible sliver.
        //
        // The shell's own vertices are built in Y-up ECEF and the node transform carries them
        // into the frame (apply_ecef_y_up_placement). The ground pass cannot do that, because
        // it lives on tile meshes it does not own, so it gets the inverse as a matrix instead.
        const Vector3 viewer_y_up = get_camera_ecef_y_up();
        GlobeAtmosphereShading::publish_frame( frame() );

        // Height above the ellipsoid, straight from the Y-up ECEF position. Not
        // "length() - polar radius": at latitude 40 the ellipsoid surface is 8 km closer to the
        // centre than the equatorial radius suggests, so that shortcut parks the camera 8 km
        // too high and the whole atmosphere thins out.
        const math::Vec3 viewer_ecef_z_up =
            math::Vec3( static_cast<double>( viewer_y_up.x ), static_cast<double>( -viewer_y_up.z ),
                        static_cast<double>( viewer_y_up.y ) );
        const math::Vec3 geodetic = math::cartesianToWgs84( viewer_ecef_z_up );
        const double camera_height = std::max( geodetic.z, 0.0 );
        const double camera_distance = static_cast<double>( viewer_y_up.length() );

        // The sun direction in the same Y-up ECEF space. This is the vector the reference calls
        // uSunDirWC, unmodified.
        const Vector3 sun_y_up = compute_sun_direction_y_up();

        // globe_atmo_lighting_fade mirrors the reference's uLightingFade (FullGlobe.ts): 0 below
        // camDist 6.5e6 m, 1 above 9.0e6 m, so the effect switches itself off for close-up work
        // exactly as the reference's does. The ground pass and the shell read the same value,
        // so the veil and the limb cannot disagree about how much of the effect is active.
        const double fade_lo = 6.5e6;
        const double fade_hi = 9.0e6;
        const double fade = ( ground_atmosphere_ && atmosphere_ground_fade_ )
                                ? std::max( 0.0, std::min( 1.0, ( camera_distance - fade_lo ) /
                                                                          ( fade_hi - fade_lo ) ) )
                                : 1.0;

        // The shell's own knobs stay on its material: they are per-node settings, not shared
        // state, and there is exactly one shell.
        const Ref<ShaderMaterial> shell_material = atmosphere_->get_material_override();
        if ( shell_material.is_valid() )
        {
            shell_material->set_shader_parameter( "globe_atmo_debug_pure",
                                                  atmosphere_debug_pure_ ? 1.0f : 0.0f );
        }

        GlobeAtmosphereShading::publish(
            viewer_y_up, camera_height, camera_distance, sun_y_up,
            ground_atmosphere_intensity_, atmosphere_intensity_, fade,
            ground_atmosphere_ && show_atmosphere_, show_sun_ );

        // Two values that are per-node rather than per-frame, but global anyway so the tiles -
        // which GlobeTileLayer builds without any reference back to this node - see the same
        // tint and the same ellipsoid the shell does.
        RenderingServer *rs = RenderingServer::get_singleton();
        if ( rs != nullptr )
        {
            rs->global_shader_parameter_set(
                "globe_atmo_tint", Vector3( atmosphere_color_.r, atmosphere_color_.g,
                                            atmosphere_color_.b ) );
            rs->global_shader_parameter_set( "globe_atmo_sunset_tint",
                                             static_cast<float>( atmosphere_sunset_tint_ ) );
            rs->global_shader_parameter_set( "globe_atmo_debug_albedo",
                                             static_cast<float>( ground_debug_albedo_ ) );
            rs->global_shader_parameter_set( "globe_atmo_ellipsoid_radii",
                                             Vector3( static_cast<float>( kEllipsoidRadiiX ),
                                                      static_cast<float>( kEllipsoidRadiiY ),
                                                      static_cast<float>( kEllipsoidRadiiZ ) ) );
        }
    }

    // ---- geography ----

    const GlobeFrame &Globe3D::frame() const
    {
        // Resolved lazily and re-walked whenever the answer is stale: georeference
        // reparenting must be picked up, and resolve() only reads cached matrices.
        if ( !frame_resolved_ )
        {
            frame_ = GlobeFrame::resolve( this );
            frame_resolved_ = true;
        }
        return frame_;
    }

    Vector3 Globe3D::geodetic_to_local( const double p_longitude_degrees,
                                        const double p_latitude_degrees,
                                        const double p_height ) const
    {
        // Same path the mesh builder takes, so a position dropped here sits exactly on the
        // drawn surface.
        const math::Vec3 ecef = math::wgs84ToCartesian(
            p_longitude_degrees * kDegreesToRadians, p_latitude_degrees * kDegreesToRadians,
            p_height );
        return toGodotVector( frame().to_local( ecef ) );
    }

    Vector3 Globe3D::ecef_to_local( const Vector3 &p_ecef ) const
    {
        // `p_ecef` is Y-up ECEF (the convention geodeticToYUp and the camera controller
        // speak); the frame works in kernel Z-up, so convert first.
        const math::Vec3 ecef_z_up( p_ecef.x, -p_ecef.z, p_ecef.y );
        return toGodotVector( frame().to_local( ecef_z_up ) );
    }

    Vector3 Globe3D::local_to_geodetic( const Vector3 &p_local ) const
    {
        // Invert geodetic_to_local: local -> Z-up ECEF -> geodetic.
        //
        // The inverse has to go through the same ellipsoid the forward conversion uses -
        // cartesianToWgs84. Deriving latitude here by hand gave *geocentric* latitude
        // (atan2(z, p)) and a height of |v| - semiMajorAxis, which is not the geodetic pair
        // the class documents and is not the inverse of geodetic_to_local: at 40 degrees the
        // two latitudes differ by 0.19 degrees, about 21 km on the ground, and the height is
        // off by several hundred metres. Anything comparing a position against a published
        // coordinate (a dataset's own longitude/latitude, say) read that as a placement bug.
        const math::Vec3 ecef = frame().to_ecef_z_up( fromGodotVector( p_local ) );
        const math::Vec3 geodetic = math::cartesianToWgs84( ecef );

        return Vector3( static_cast<float>( geodetic.x * kRadiansToDegrees ),
                        static_cast<float>( geodetic.y * kRadiansToDegrees ),
                        static_cast<float>( geodetic.z ) );
    }

    const Georeference3D *Globe3D::find_georeference() const
    {
        for ( godot::Node *parent = get_parent(); parent != nullptr; parent = parent->get_parent() )
        {
            if ( const Georeference3D *reference =
                     godot::Object::cast_to<Georeference3D>( parent ) )
            {
                return reference;
            }
        }
        return nullptr;
    }

    void Globe3D::update_surface_visibility()
    {
        if ( surface_ == nullptr )
        {
            return;
        }

        bool visible = true;
        if ( show_surface_ >= 0 )
        {
            visible = show_surface_ != 0;
        }
        else
        {
            // AUTO. Look for a sibling GlobeTileLayer that is actually putting tiles on the
            // screen, and get out of its way.
            //
            // The test is `rendered > 0` rather than "a layer node exists", because a layer
            // whose imagery has all failed still exists, still has a node, and still covers
            // nothing. Hiding the shell for that case would leave a hole in the planet. It
            // also has to be a *sibling* walk rather than a global one: the architecture
            // deliberately puts Globe3D and GlobeTileLayer as siblings under the same
            // Georeference3D, so anything else sharing that parent is a different globe.
            //
            // get_child(i, true): the include_internal flag is required because a tile layer
            // added by a tool script can carry internal children, and missing them would
            // make the shell reappear the moment anything was parented internally.
            godot::Node *parent = get_parent();
            if ( parent != nullptr )
            {
                const int32_t count = parent->get_child_count( true );
                for ( int32_t i = 0; i < count; ++i )
                {
                    godot::Node *child = parent->get_child( i, true );
                    const GlobeTileLayer *layer =
                        child != nullptr ? godot::Object::cast_to<GlobeTileLayer>( child )
                                         : nullptr;
                    if ( layer != nullptr && layer->get_rendered_tile_count() > 0 )
                    {
                        visible = false;
                        break;
                    }
                }
            }
        }

        if ( visible != surface_visible_applied_ )
        {
            surface_visible_applied_ = visible;
            surface_->set_visible( visible );
        }
    }

} // namespace tiles3d
