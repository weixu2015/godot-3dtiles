// SPDX-License-Identifier: Unlicense

#include "Globe3D.h"

#include "GodotMathConvert.h"

#include "core/math/GeoMath.h"
#include "core/math/Mat4.h"

#include "godot_cpp/classes/array_mesh.hpp"
#include "godot_cpp/classes/base_material3d.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/standard_material3d.hpp"
#include "godot_cpp/classes/surface_tool.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/packed_int32_array.hpp"
#include "godot_cpp/variant/vector2.hpp"

#include <cmath>

namespace tiles3d
{
    using godot::ArrayMesh;
    using godot::BaseMaterial3D;
    using godot::Callable;
    using godot::ClassDB;
    using godot::Color;
    using godot::D_METHOD;
    using godot::Engine;
    using godot::Mesh;
    using godot::PropertyInfo;
    using godot::Ref;
    using godot::StandardMaterial3D;
    using godot::SurfaceTool;
    using godot::Texture2D;
    using godot::Variant;
    using godot::Vector2;
    using godot::Vector3;

    namespace
    {
        constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
        constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;

        /// How far above the ellipsoid's widest axis the graticule ring is drawn, so the
        /// lines are not z-fighting with the surface itself.
        constexpr double kGraticuleAltitude = 1000.0;

        /// A mild, non-physical material for the surface while no texture is assigned. The
        /// globe is mostly lit by a DirectionalLight3D in the demo; keeping metallic at 0
        /// and roughness high stops the ellipsoid reading as a shiny ball.
        Ref<StandardMaterial3D> make_surface_material( const Color &p_base, bool p_has_texture )
        {
            Ref<StandardMaterial3D> material;
            material.instantiate();
            material->set_shading_mode( BaseMaterial3D::SHADING_MODE_PER_PIXEL );
            material->set_cull_mode( BaseMaterial3D::CULL_BACK );
            material->set_metallic( 0.0f );
            material->set_roughness( 0.9f );
            if ( p_has_texture )
            {
                material->set_albedo( Color( 1.0f, 1.0f, 1.0f, 1.0f ) );
            }
            else
            {
                material->set_albedo( p_base );
            }
            return material;
        }
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

        ClassDB::bind_method( D_METHOD( "rebuild" ), &Globe3D::rebuild );

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
                break;
            case NOTIFICATION_EDITOR_PRE_SAVE:
            case NOTIFICATION_ENTER_TREE:
                // Building in the editor means the ellipsoid is visible without running the
                // project, which is how the tile sets were validated too.
                if ( Engine::get_singleton() != nullptr && Engine::get_singleton()->is_editor_hint() )
                {
                    ensure_children();
                    rebuild();
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

    void Globe3D::rebuild()
    {
        ensure_children();
        rebuild_surface();
        rebuild_graticule();
        update_materials();
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
            return math::geodeticToYUp( longitude, latitude, 0.0 );
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
                const Vector3 tl = toGodotVector( vertex_at( column, row ) );
                const Vector3 tr = toGodotVector( vertex_at( next_column, row ) );
                const Vector3 bl = toGodotVector( vertex_at( column, next_row ) );
                const Vector3 br = toGodotVector( vertex_at( next_column, next_row ) );

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

        const auto add_ring = [&]( const double latitude_degrees )
        {
            const double latitude = latitude_degrees * kDegreesToRadians;
            for ( int i = 0; i < 360; ++i )
            {
                const double lon0 = ( i - 180.0 ) * kDegreesToRadians;
                const double lon1 = ( i + 1 - 180.0 ) * kDegreesToRadians;
                tool->add_vertex( toGodotVector(
                    math::geodeticToYUp( lon0, latitude, kGraticuleAltitude ) ) );
                tool->add_vertex( toGodotVector(
                    math::geodeticToYUp( lon1, latitude, kGraticuleAltitude ) ) );
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
                tool->add_vertex( toGodotVector(
                    math::geodeticToYUp( longitude, lat0, kGraticuleAltitude ) ) );
                tool->add_vertex( toGodotVector(
                    math::geodeticToYUp( longitude, lat1, kGraticuleAltitude ) ) );
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

    void Globe3D::update_materials()
    {
        if ( surface_ != nullptr )
        {
            const bool has_texture = albedo_texture_.is_valid();
            const Ref<StandardMaterial3D> material =
                make_surface_material( base_color_, has_texture );
            if ( has_texture )
            {
                material->set_texture( BaseMaterial3D::TEXTURE_ALBEDO, albedo_texture_ );
            }
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
    }

    // ---- geography ----

    math::Mat4 Globe3D::ecef_to_local_matrix() const
    {
        if ( const Georeference3D *reference = find_georeference(); reference != nullptr )
        {
            return reference->ecef_to_local();
        }

        // No Georeference3D ancestor: build the same kind of frame Tileset3D builds for its
        // implicit fallback, anchored at (longitude 0, latitude 0) on the ellipsoid. The
        // globe then sits with its local origin on the equator rather than at the Earth's
        // centre, which keeps coordinates small for float32.
        if ( !fallback_frame_built_ )
        {
            const math::Vec3 origin =
                math::wgs84ToCartesian( 0.0, 0.0, 0.0 ); // (A, 0, 0) in Z-up ECEF.
            fallback_ecef_to_local_ = math::invert( math::eastNorthUpToFixedFrame( origin ) );
            fallback_frame_built_ = true;
        }
        return fallback_ecef_to_local_;
    }

    Vector3 Globe3D::geodetic_to_local( const double p_longitude_degrees,
                                        const double p_latitude_degrees,
                                        const double p_height ) const
    {
        // The drawn surface lives in this node's local, Y-up space (see rebuild_surface), so
        // a position dropped here has to end up in exactly that space or it will not sit on
        // the globe. geodeticToYUp already produces Y-up ECEF-relative-to-centre coordinates,
        // and subtracting the frame origin maps that into the node's local frame.
        const math::Vec3 ecef_rel = math::geodeticToYUp( p_longitude_degrees * kDegreesToRadians,
                                                         p_latitude_degrees * kDegreesToRadians,
                                                         p_height );
        const math::Vec3 origin =
            math::transformPoint( math::invert( ecef_to_local_matrix() ), math::Vec3( 0.0 ) );
        return toGodotVector( ecef_rel - origin );
    }

    Vector3 Globe3D::ecef_to_local( const Vector3 &p_ecef ) const
    {
        // `p_ecef` is Y-up ECEF, i.e. the same convention geodeticToYUp emits. Re-expressing
        // it in the node's local frame is a plain origin subtraction, matching
        // geodetic_to_local.
        const math::Vec3 origin =
            math::transformPoint( math::invert( ecef_to_local_matrix() ), math::Vec3( 0.0 ) );
        return toGodotVector( fromGodotVector( p_ecef ) - origin );
    }

    Vector3 Globe3D::local_to_geodetic( const Vector3 &p_local ) const
    {
        // Invert geodetic_to_local: local -> Y-up ECEF -> geodetic.
        const math::Vec3 origin =
            math::transformPoint( math::invert( ecef_to_local_matrix() ), math::Vec3( 0.0 ) );
        const math::Vec3 ecef = fromGodotVector( p_local ) + origin;

        const double x = ecef.x;
        const double y = ecef.y;
        const double z = ecef.z;
        const double p = std::sqrt( x * x + z * z );

        // Y-up convention: +Y is the pole, lon 0 is +X and lon +90 is -Z, so longitude is
        // atan2(-z, x) and latitude is measured from the equatorial plane.
        const double longitude = std::atan2( -z, x );
        const double latitude = std::atan2( y, p );
        const double height = std::sqrt( x * x + y * y + z * z ) - math::kWgs84SemiMajorAxis;

        return Vector3( static_cast<float>( longitude * kRadiansToDegrees ),
                        static_cast<float>( latitude * kRadiansToDegrees ),
                        static_cast<float>( height ) );
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

} // namespace tiles3d
