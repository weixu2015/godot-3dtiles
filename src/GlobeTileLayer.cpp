// SPDX-License-Identifier: Unlicense

#include "GlobeTileLayer.h"

#include "Georeference3D.h"
#include "GlobeAtmosphereShading.h"
#include "GodotMathConvert.h"

#include "core/math/EllipsoidalOccluder.h"
#include "core/math/GeoMath.h"
#include "core/math/Mat4.h"

#include "godot_cpp/classes/array_mesh.hpp"
#include "godot_cpp/classes/camera3d.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/http_request.hpp"
#include "godot_cpp/classes/image.hpp"
#include "godot_cpp/classes/image_texture.hpp"
#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "godot_cpp/classes/object.hpp"
#include "godot_cpp/classes/project_settings.hpp"
#include "godot_cpp/classes/rendering_server.hpp"
#include "godot_cpp/classes/shader_material.hpp"
#include "godot_cpp/classes/standard_material3d.hpp"
#include "godot_cpp/classes/sub_viewport.hpp"
#include "godot_cpp/classes/texture2d.hpp"
#include "godot_cpp/classes/time.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/utility_functions.hpp"
#include "godot_cpp/variant/array.hpp"
#include "godot_cpp/variant/packed_int32_array.hpp"
#include "godot_cpp/variant/packed_vector2_array.hpp"
#include "godot_cpp/variant/packed_vector3_array.hpp"
#include "godot_cpp/variant/plane.hpp"
#include "godot_cpp/variant/projection.hpp"
#include "godot_cpp/variant/transform3d.hpp"
#include "godot_cpp/variant/vector2.hpp"
#include "godot_cpp/variant/vector3.hpp"

#ifdef TILES3D_EDITOR_TARGET
#include "godot_cpp/classes/editor_interface.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <limits>

namespace tiles3d
{
    using godot::ArrayMesh;
    using godot::Callable;
    using godot::ClassDB;
    using godot::Color;
    using godot::D_METHOD;
    using godot::Engine;
    using godot::Error;
    using godot::HTTPRequest;
    using godot::Image;
    using godot::ImageTexture;
    using godot::MeshInstance3D;
    using godot::PackedInt32Array;
    using godot::PackedVector2Array;
    using godot::PackedVector3Array;
    using godot::Plane;
    using godot::Projection;
    using godot::PropertyInfo;
    using godot::Ref;
    using godot::ShaderMaterial;
    using godot::StandardMaterial3D;
    using godot::String;
    using godot::Time;
    using godot::Vector2;
    using godot::Vector3;

    namespace
    {
        constexpr double kDegreesToRadians = math::kPi / 180.0;

        /// Placeholder blue shown while a tile has no ancestor texture yet
        /// (reference PLACEHOLDER_COLOR 0x1c4a80).
        const Color kPlaceholderColor( 0.11f, 0.29f, 0.50f, 1.0f );

        /// Frames the editor pose is re-applied for before the camera is released.
        constexpr int kFramingFrames = 5;
        /// Upper bound on that hold, so a camera that never resolves is not pinned forever.
        constexpr int kFramingFrameCap = 600;

        /// Segments per tile edge at level 0; halved every level up to the clamp
        /// (reference: `64 >> min(level, 4)` clamped to [6, 32]).
        int tile_edge_segments( int level )
        {
            const int raw = 64 >> std::min( level, 4 );
            return std::clamp( raw, 6, 32 );
        }

        /// Row `row` of a column-major Projection, as a homogeneous plane row.
        math::Vec4 projection_row( const Projection &projection, const int row )
        {
            math::Vec4 result{ 0.0 };
            for ( int column = 0; column < 4; ++column )
            {
                const godot::Vector4 axis = projection.columns[column];
                result[column] = axis[row];
            }
            return result;
        }

        /// Seconds since engine start, for the failure retry cool-down.
        double now_seconds()
        {
            const Time *time = Time::get_singleton();
            return time != nullptr ? static_cast<double>( time->get_ticks_msec() ) / 1000.0 : 0.0;
        }
    } // namespace

    GlobeTileLayer::GlobeTileLayer() = default;

    GlobeTileLayer::~GlobeTileLayer()
    {
        destroy_roots();
    }

    void GlobeTileLayer::_bind_methods()
    {
        ClassDB::bind_method( D_METHOD( "set_enabled", "p_value" ), &GlobeTileLayer::set_enabled );
        ClassDB::bind_method( D_METHOD( "get_enabled" ), &GlobeTileLayer::get_enabled );
        ClassDB::add_property( "GlobeTileLayer", PropertyInfo( godot::Variant::BOOL, "enabled" ),
                               "set_enabled", "get_enabled" );

        ClassDB::bind_method( D_METHOD( "set_url_template", "p_value" ),
                              &GlobeTileLayer::set_url_template );
        ClassDB::bind_method( D_METHOD( "get_url_template" ), &GlobeTileLayer::get_url_template );
        ClassDB::add_property(
            "GlobeTileLayer",
            PropertyInfo( godot::Variant::STRING, "url_template", godot::PROPERTY_HINT_MULTILINE_TEXT ),
            "set_url_template", "get_url_template" );

        ClassDB::bind_method( D_METHOD( "set_maximum_screen_space_error", "p_value" ),
                              &GlobeTileLayer::set_maximum_screen_space_error );
        ClassDB::bind_method( D_METHOD( "get_maximum_screen_space_error" ),
                              &GlobeTileLayer::get_maximum_screen_space_error );
        ClassDB::add_property(
            "GlobeTileLayer",
            PropertyInfo( godot::Variant::FLOAT, "maximum_screen_space_error",
                          godot::PROPERTY_HINT_RANGE, "0.1,32.0,0.1" ),
            "set_maximum_screen_space_error", "get_maximum_screen_space_error" );

        ClassDB::bind_method( D_METHOD( "set_maximum_level", "p_value" ),
                              &GlobeTileLayer::set_maximum_level );
        ClassDB::bind_method( D_METHOD( "get_maximum_level" ), &GlobeTileLayer::get_maximum_level );
        ClassDB::add_property(
            "GlobeTileLayer",
            PropertyInfo( godot::Variant::INT, "maximum_level", godot::PROPERTY_HINT_RANGE, "0,22,1" ),
            "set_maximum_level", "get_maximum_level" );

        ClassDB::bind_method( D_METHOD( "set_tile_cache_size", "p_value" ),
                              &GlobeTileLayer::set_tile_cache_size );
        ClassDB::bind_method( D_METHOD( "get_tile_cache_size" ), &GlobeTileLayer::get_tile_cache_size );
        ClassDB::add_property(
            "GlobeTileLayer",
            PropertyInfo( godot::Variant::INT, "tile_cache_size", godot::PROPERTY_HINT_RANGE,
                          "16,4096,1" ),
            "set_tile_cache_size", "get_tile_cache_size" );

        ClassDB::bind_method( D_METHOD( "set_max_concurrent_requests", "p_value" ),
                              &GlobeTileLayer::set_max_concurrent_requests );
        ClassDB::bind_method( D_METHOD( "get_max_concurrent_requests" ),
                              &GlobeTileLayer::get_max_concurrent_requests );
        ClassDB::add_property(
            "GlobeTileLayer",
            PropertyInfo( godot::Variant::INT, "max_concurrent_requests",
                          godot::PROPERTY_HINT_RANGE, "1,32,1" ),
            "set_max_concurrent_requests", "get_max_concurrent_requests" );

        ClassDB::bind_method( D_METHOD( "set_print_telemetry", "p_value" ),
                              &GlobeTileLayer::set_print_telemetry );
        ClassDB::bind_method( D_METHOD( "get_print_telemetry" ),
                              &GlobeTileLayer::get_print_telemetry );
        ClassDB::add_property(
            "GlobeTileLayer",
            PropertyInfo( godot::Variant::BOOL, "print_telemetry" ),
            "set_print_telemetry", "get_print_telemetry" );

        ClassDB::bind_method( D_METHOD( "set_show_tile_bounds", "p_value" ),
                              &GlobeTileLayer::set_show_tile_bounds );
        ClassDB::bind_method( D_METHOD( "get_show_tile_bounds" ), &GlobeTileLayer::get_show_tile_bounds );
        ClassDB::add_property( "GlobeTileLayer",
                               PropertyInfo( godot::Variant::BOOL, "show_tile_bounds" ),
                               "set_show_tile_bounds", "get_show_tile_bounds" );

        ClassDB::bind_method( D_METHOD( "set_frame_editor_on_ready", "p_value" ),
                              &GlobeTileLayer::set_frame_editor_on_ready );
        ClassDB::bind_method( D_METHOD( "get_frame_editor_on_ready" ),
                              &GlobeTileLayer::get_frame_editor_on_ready );
        ClassDB::add_property( "GlobeTileLayer",
                               PropertyInfo( godot::Variant::BOOL, "frame_editor_on_ready" ),
                               "set_frame_editor_on_ready", "get_frame_editor_on_ready" );

        ClassDB::bind_method( D_METHOD( "set_attribution", "p_value" ),
                              &GlobeTileLayer::set_attribution );
        ClassDB::bind_method( D_METHOD( "get_attribution" ), &GlobeTileLayer::get_attribution );
        ClassDB::add_property( "GlobeTileLayer",
                               PropertyInfo( godot::Variant::STRING, "attribution",
                                             godot::PROPERTY_HINT_MULTILINE_TEXT ),
                               "set_attribution", "get_attribution" );

        ClassDB::bind_method( D_METHOD( "set_editor_view_longitude", "p_value" ),
                              &GlobeTileLayer::set_editor_view_longitude );
        ClassDB::bind_method( D_METHOD( "get_editor_view_longitude" ),
                              &GlobeTileLayer::get_editor_view_longitude );
        ClassDB::add_property(
            "GlobeTileLayer",
            PropertyInfo( godot::Variant::FLOAT, "editor_view_longitude",
                          godot::PROPERTY_HINT_RANGE, "-180,180,0.01" ),
            "set_editor_view_longitude", "get_editor_view_longitude" );

        ClassDB::bind_method( D_METHOD( "set_editor_view_latitude", "p_value" ),
                              &GlobeTileLayer::set_editor_view_latitude );
        ClassDB::bind_method( D_METHOD( "get_editor_view_latitude" ),
                              &GlobeTileLayer::get_editor_view_latitude );
        ClassDB::add_property(
            "GlobeTileLayer",
            PropertyInfo( godot::Variant::FLOAT, "editor_view_latitude",
                          godot::PROPERTY_HINT_RANGE, "-90,90,0.01" ),
            "set_editor_view_latitude", "get_editor_view_latitude" );

        ClassDB::bind_method( D_METHOD( "set_editor_view_distance", "p_value" ),
                              &GlobeTileLayer::set_editor_view_distance );
        ClassDB::bind_method( D_METHOD( "get_editor_view_distance" ),
                              &GlobeTileLayer::get_editor_view_distance );
        ClassDB::add_property(
            "GlobeTileLayer",
            PropertyInfo( godot::Variant::FLOAT, "editor_view_distance",
                          godot::PROPERTY_HINT_RANGE, "1000,200000000,1000" ),
            "set_editor_view_distance", "get_editor_view_distance" );

        ClassDB::bind_method( D_METHOD( "reframe_editor_view" ),
                              &GlobeTileLayer::reframe_editor_view );
        // Bound so the RenderingServer frame_pre_draw Callable can resolve it by name.
        ClassDB::bind_method( D_METHOD( "_on_frame_pre_draw" ),
                              &GlobeTileLayer::_on_frame_pre_draw );

        ClassDB::bind_method( D_METHOD( "set_manage_editor_clip", "p_value" ),
                              &GlobeTileLayer::set_manage_editor_clip );
        ClassDB::bind_method( D_METHOD( "get_manage_editor_clip" ),
                              &GlobeTileLayer::get_manage_editor_clip );
        ClassDB::add_property( "GlobeTileLayer",
                               PropertyInfo( godot::Variant::BOOL, "manage_editor_clip" ),
                               "set_manage_editor_clip", "get_manage_editor_clip" );

        ClassDB::bind_method( D_METHOD( "reload_tiles" ), &GlobeTileLayer::reload_tiles );
        ClassDB::bind_method( D_METHOD( "rebase" ), &GlobeTileLayer::rebase );

        ClassDB::bind_method( D_METHOD( "get_rendered_tile_count" ),
                              &GlobeTileLayer::get_rendered_tile_count );
        ClassDB::bind_method( D_METHOD( "get_loading_tile_count" ),
                              &GlobeTileLayer::get_loading_tile_count );
        ClassDB::bind_method( D_METHOD( "get_cached_tile_count" ),
                              &GlobeTileLayer::get_cached_tile_count );
        ClassDB::bind_method( D_METHOD( "get_max_selected_level" ),
                              &GlobeTileLayer::get_max_selected_level );

        ClassDB::bind_method( D_METHOD( "_on_tile_request_completed", "p_result",
                                        "p_response_code", "p_headers", "p_body", "p_request_id" ),
                              &GlobeTileLayer::_on_tile_request_completed );
    }

    void GlobeTileLayer::_notification( const int p_what )
    {
        switch ( p_what )
        {
            case NOTIFICATION_READY:
            {
                // Same pattern as Tileset3D: the traversal runs through NOTIFICATION_PROCESS
                // because godot-cpp does not expose _process as a virtual on Node. This also
                // makes the layer work inside the editor - the editor scene tree is a real
                // SceneTree and keeps sending PROCESS notifications.
                set_process( true );
                needs_framing_ = frame_editor_on_ready_ && Engine::get_singleton() != nullptr &&
                                 Engine::get_singleton()->is_editor_hint();
                framing_frames_ = 0;
#ifdef TILES3D_EDITOR_TARGET
                // Re-assert the clip planes the instant before every frame is drawn: the
                // editor writes its own 0.1/4000 during navigation, and a write from
                // _process loses that race for the whole inertia tail.
                Engine *ready_engine = Engine::get_singleton();
                if ( ready_engine != nullptr && ready_engine->is_editor_hint() &&
                     !pre_draw_connected_ )
                {
                    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
                    if ( rs != nullptr )
                    {
                        rs->connect( "frame_pre_draw",
                                     Callable( this, "_on_frame_pre_draw" ) );
                        pre_draw_connected_ = true;
                    }
                }
#endif
                break;
            }

            case NOTIFICATION_PROCESS:
                if ( enabled_ )
                {
                    update_tiles();
                }
                if ( print_telemetry_ && now_seconds() - last_telemetry_stamp_ > 3.0 )
                {
                    last_telemetry_stamp_ = now_seconds();
                    godot::Camera3D *cam = resolve_camera();
                    WARN_PRINT( "[globe-tile-layer] editor=" +
                                ( Engine::get_singleton() != nullptr &&
                                          Engine::get_singleton()->is_editor_hint()
                                      ? godot::String( "1" )
                                      : godot::String( "0" ) ) +
                                " needs_framing=" +
                                ( needs_framing_ ? godot::String( "1" ) : godot::String( "0" ) ) +
                                " campos=" +
                                ( cam != nullptr ? godot::String( cam->get_global_position() )
                                                 : godot::String( "<none>" ) ) +
                                " camcenter=" +
                                godot::String( to_global( godot::Vector3( center_cache_.x,
                                                                         center_cache_.y,
                                                                         center_cache_.z ) ) ) +
                                ( cam != nullptr
                                      ? godot::String( " near=" ) +
                                                godot::String::num( cam->get_near(), 1 ) +
                                                " far=" +
                                                godot::String::num( cam->get_far(), 0 )
                                      : godot::String( "" ) ) +
                                " rendered=" + godot::String::num_int64( rendered_count_ ) );
                }
                break;

            case NOTIFICATION_EXIT_TREE:
                cancel_pending_requests();
                disconnect_pre_draw();
                break;

            case NOTIFICATION_PREDELETE:
                cancel_pending_requests();
                disconnect_pre_draw();
                destroy_roots();
                break;

            default:
                break;
        }
    }

    // ---- property setters / getters ----

    void GlobeTileLayer::set_enabled( const bool p_value )
    {
        enabled_ = p_value;
        set_process( enabled_ );
    }

    bool GlobeTileLayer::get_enabled() const
    {
        return enabled_;
    }

    void GlobeTileLayer::set_url_template( const String &p_value )
    {
        if ( url_template_ != p_value )
        {
            url_template_ = p_value;
            // Same imagery, different server: drop everything so tiles re-request.
            reload_tiles();
        }
    }

    String GlobeTileLayer::get_url_template() const
    {
        return url_template_;
    }

    void GlobeTileLayer::set_maximum_screen_space_error( const double p_value )
    {
        maximum_screen_space_error_ = p_value;
    }

    double GlobeTileLayer::get_maximum_screen_space_error() const
    {
        return maximum_screen_space_error_;
    }

    void GlobeTileLayer::set_maximum_level( const int p_value )
    {
        maximum_level_ = p_value;
    }

    int GlobeTileLayer::get_maximum_level() const
    {
        return maximum_level_;
    }

    void GlobeTileLayer::set_tile_cache_size( const int p_value )
    {
        tile_cache_size_ = p_value;
    }

    int GlobeTileLayer::get_tile_cache_size() const
    {
        return tile_cache_size_;
    }

    void GlobeTileLayer::set_max_concurrent_requests( const int p_value )
    {
        max_concurrent_requests_ = p_value;
    }

    int GlobeTileLayer::get_max_concurrent_requests() const
    {
        return max_concurrent_requests_;
    }

    void GlobeTileLayer::set_print_telemetry( const bool p_value )
    {
        print_telemetry_ = p_value;
    }

    bool GlobeTileLayer::get_print_telemetry() const
    {
        return print_telemetry_;
    }

    void GlobeTileLayer::set_manage_editor_clip( const bool p_value )
    {
        manage_editor_clip_ = p_value;
    }

    bool GlobeTileLayer::get_manage_editor_clip() const
    {
        return manage_editor_clip_;
    }

    void GlobeTileLayer::set_editor_view_longitude( const double p_value )
    {
        editor_view_longitude_ = p_value;
        reframe_editor_view();
    }

    double GlobeTileLayer::get_editor_view_longitude() const
    {
        return editor_view_longitude_;
    }

    void GlobeTileLayer::set_editor_view_latitude( const double p_value )
    {
        editor_view_latitude_ = p_value;
        reframe_editor_view();
    }

    double GlobeTileLayer::get_editor_view_latitude() const
    {
        return editor_view_latitude_;
    }

    void GlobeTileLayer::set_editor_view_distance( const double p_value )
    {
        editor_view_distance_ = p_value;
        reframe_editor_view();
    }

    double GlobeTileLayer::get_editor_view_distance() const
    {
        return editor_view_distance_;
    }

    void GlobeTileLayer::reframe_editor_view()
    {
        needs_framing_ = true;
        framing_frames_ = 0;

        // Property setters run while the scene is still being deserialized, i.e. before the
        // node is in the tree; to_global() there is an engine error and hands back an
        // identity transform. Arming the request is enough - update_tiles() applies it on
        // the first frame, and reframe_editor_view() called from a script applies it now.
        if ( !is_inside_tree() )
        {
            return;
        }

        // Apply straight away so an Inspector tweak shows up immediately instead of on the
        // next process frame. frame_ is normally refreshed inside update_tiles(); a setter
        // can arrive before that has ever run.
        resolve_frame();
        center_cache_ = frame_.ellipsoid_center_local();
        apply_editor_framing();
    }

    void GlobeTileLayer::set_show_tile_bounds( const bool p_value )
    {
        show_tile_bounds_ = p_value;
    }

    bool GlobeTileLayer::get_show_tile_bounds() const
    {
        return show_tile_bounds_;
    }

    void GlobeTileLayer::set_frame_editor_on_ready( const bool p_value )
    {
        frame_editor_on_ready_ = p_value;
    }

    bool GlobeTileLayer::get_frame_editor_on_ready() const
    {
        return frame_editor_on_ready_;
    }

    void GlobeTileLayer::set_attribution( const String &p_value )
    {
        attribution_ = p_value;
    }

    String GlobeTileLayer::get_attribution() const
    {
        return attribution_;
    }

    int GlobeTileLayer::get_rendered_tile_count() const
    {
        return rendered_count_;
    }

    int GlobeTileLayer::get_loading_tile_count() const
    {
        return inflight_count_;
    }

    int GlobeTileLayer::get_cached_tile_count() const
    {
        return replacement_queue_.count;
    }

    int GlobeTileLayer::get_max_selected_level() const
    {
        return max_selected_level_;
    }

    void GlobeTileLayer::reload_tiles()
    {
        cancel_pending_requests();
        destroy_roots();
        replacement_queue_.clear();
        inflight_count_ = 0;
        rendered_count_ = 0;
        max_selected_level_ = 0;
        frame_number_ = 0;
        ++texture_version_;
    }

    // ---- frame plumbing ----

    void GlobeTileLayer::resolve_frame()
    {
        // The same shared frame Globe3D and Tileset3D resolve: georeference frame when one
        // is an ancestor, otherwise Y-up ECEF with the flip baked in. Using it verbatim is
        // what makes imagery land on the globe and beside the tilesets.
        frame_ = GlobeFrame::resolve( this );

        // The tile material needs the frame's local space -> Y-up ECEF matrix, because the
        // scattering integral measures from the Earth's centre and the tile vertices are
        // authored in the georeference's ENU frame. Published here as well as from Globe3D so
        // a scene with a bare GlobeTileLayer and no Globe3D still shades correctly; both nodes
        // resolve the same frame, so the two writes agree.
        GlobeAtmosphereShading::publish_frame( frame_ );
    }

    math::Vec3 GlobeTileLayer::mesh_point( const math::Vec3 &ecef_z_up ) const
    {
        return frame_.to_local( ecef_z_up );
    }

    godot::Camera3D *GlobeTileLayer::resolve_camera() const
    {
#ifdef TILES3D_EDITOR_TARGET
        // In the editor the camera the user actually flies is the editor viewport's own
        // camera; a Camera3D node in the scene would be a static stand-in. Same trade-off
        // Tileset3D makes.
        Engine *engine = Engine::get_singleton();
        if ( engine != nullptr && engine->is_editor_hint() )
        {
            godot::EditorInterface *editor = godot::EditorInterface::get_singleton();
            if ( editor != nullptr )
            {
                godot::SubViewport *viewport = editor->get_editor_viewport_3d();
                if ( viewport != nullptr )
                {
                    return viewport->get_camera_3d();
                }
            }
            return nullptr;
        }
#endif
        const godot::Viewport *viewport = get_viewport();
        return viewport != nullptr ? viewport->get_camera_3d() : nullptr;
    }

    // ---- editor viewport plumbing ----

    godot::Camera3D *GlobeTileLayer::resolve_editor_camera() const
    {
#ifdef TILES3D_EDITOR_TARGET
        Engine *engine = Engine::get_singleton();
        if ( engine == nullptr || !engine->is_editor_hint() )
        {
            return nullptr;
        }
        godot::EditorInterface *editor = godot::EditorInterface::get_singleton();
        if ( editor == nullptr )
        {
            return nullptr;
        }
        godot::SubViewport *viewport = editor->get_editor_viewport_3d();
        return viewport != nullptr ? viewport->get_camera_3d() : nullptr;
#else
        return nullptr;
#endif
    }

    bool GlobeTileLayer::apply_editor_framing()
    {
        godot::Camera3D *editor_camera = resolve_editor_camera();
        if ( editor_camera == nullptr )
        {
            return false;
        }

        const double distance = std::max( editor_view_distance_, 1.0 );
        const math::Vec3 direction_local = frame_.local_direction(
            math::geodeticToYUp( editor_view_longitude_ * kDegreesToRadians,
                                 editor_view_latitude_ * kDegreesToRadians, 0.0 ) );
        const math::Vec3 center_local = frame_.ellipsoid_center_local();
        const Vector3 direction( static_cast<float>( direction_local.x ),
                                 static_cast<float>( direction_local.y ),
                                 static_cast<float>( direction_local.z ) );
        const Vector3 center( static_cast<float>( center_local.x ),
                              static_cast<float>( center_local.y ),
                              static_cast<float>( center_local.z ) );
        const Vector3 eye = center + direction.normalized() * static_cast<float>( distance );
        const Vector3 forward = ( center - eye ).normalized();
        Vector3 up( 0.0f, 1.0f, 0.0f );
        if ( std::abs( forward.dot( up ) ) > 0.99f )
        {
            up = Vector3( 0.0f, 0.0f, 1.0f );
        }
        const Vector3 right = up.cross( forward ).normalized();
        const Vector3 camera_up = forward.cross( right ).normalized();
        // Godot cameras look down their local -Z.
        //
        // The pose above was built out of frame-local vectors (frame_.ellipsoid_center_local()
        // and frame_.local_direction()), while the editor camera's global transform lives in
        // the editor's own space. This layer sits inside the georeference, so its own global
        // transform is exactly the mapping between the two - without it the Z-up -> Y-up flip
        // on Georeference3D puts the camera at the wrong longitude and latitude.
        editor_camera->set_global_transform(
            get_global_transform() *
            godot::Transform3D( godot::Basis( right, camera_up, -forward ), eye ) );
        assert_editor_clip( editor_camera );
        return true;
    }

    void GlobeTileLayer::assert_editor_clip( godot::Camera3D *p_camera )
    {
        if ( p_camera == nullptr || !manage_editor_clip_ || !is_inside_tree() )
        {
            return;
        }
        // Run-time cameras belong to the scene (GlobeCameraController); only the editor's
        // free camera is ours to fix.
        Engine *engine = Engine::get_singleton();
        if ( engine == nullptr || !engine->is_editor_hint() )
        {
            return;
        }
        const Vector3 center_world =
            to_global( Vector3( center_cache_.x, center_cache_.y, center_cache_.z ) );
        const double center_distance =
            std::max( 1.0, static_cast<double>(
                              ( center_world - p_camera->get_global_position() ).length() ) );

        // Near grows with the distance so the depth ratio stays usable, and is capped so a
        // camera pressed against the surface does not clip the ground it is standing on.
        p_camera->set_near(
            static_cast<float>( std::clamp( center_distance * 1e-4, 1.0, 1.0e6 ) ) );
        // Far must clear the far side of the globe, which sits at (distance + radius), not
        // just some multiple of the distance: below radius/3 the two differ and the floor
        // takes over. Half a radius of margin beyond that covers the atmosphere shell. For
        // every view from outside the planet the 4x term dominates, so the depth ratio is
        // exactly what a plain multiple would give.
        const double radius = math::kWgs84SemiMajorAxis;
        p_camera->set_far( static_cast<float>(
            std::max( center_distance * 4.0, center_distance + 1.5 * radius ) ) );
    }

    void GlobeTileLayer::disconnect_pre_draw()
    {
        if ( !pre_draw_connected_ )
        {
            return;
        }
        pre_draw_connected_ = false;

        godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
        if ( rs == nullptr )
        {
            // Already tearing down; the server drops its own connections with it.
            return;
        }
        const Callable callable( this, "_on_frame_pre_draw" );
        if ( rs->is_connected( "frame_pre_draw", callable ) )
        {
            rs->disconnect( "frame_pre_draw", callable );
        }
    }

    void GlobeTileLayer::_on_frame_pre_draw()
    {
        // Runs after every Node's _process and immediately before the frame is drawn, which
        // is the only point that beats the editor: Node3DEditorViewport rewrites the viewport
        // camera's near/far (0.1 / 4000) for as long as a navigation is in flight, and a
        // _process-time write loses that race for the whole inertia tail - the globe blinks
        // out in the middle of every wheel zoom.
        assert_editor_clip( resolve_editor_camera() );
    }

    void GlobeTileLayer::compute_frustum( godot::Camera3D *camera )
    {
        // Gribb-Hartmann plane extraction. Godot's Projection follows the OpenGL
        // convention (camera looks down -Z), so the five useful planes are row combinations
        // with the homogeneous row. The far plane is skipped on purpose: with far = 2e8 m it
        // never rejects anything the horizon culling does not.
        // The editor's 3D viewport camera briefly reports a zero basis while the editor
        // creates it; inverting that is a floating point error and a frustum of NaNs, which
        // culls every tile for a frame and prints a scary Condition "det == 0". Skip instead.
        const godot::Basis camera_basis = camera->get_global_transform().basis;
        if ( std::abs( camera_basis.determinant() ) <= std::numeric_limits<float>::min() )
        {
            for ( math::Vec4 &plane : frustum_planes_ )
            {
                plane = math::Vec4{ 0.0, 0.0, 0.0, 1.0e30 }; // nothing behind, nothing in front
            }
            return;
        }

        const Projection clip =
            Projection( camera->get_camera_projection() ) *
            Projection( camera->get_global_transform().affine_inverse() );

        const math::Vec4 row0 = projection_row( clip, 0 );
        const math::Vec4 row1 = projection_row( clip, 1 );
        const math::Vec4 row2 = projection_row( clip, 2 );
        const math::Vec4 row3 = projection_row( clip, 3 );

        frustum_planes_[0] = row3 + row0; // left
        frustum_planes_[1] = row3 - row0; // right
        frustum_planes_[2] = row3 + row1; // bottom
        frustum_planes_[3] = row3 - row1; // top
        frustum_planes_[4] = row3 + row2; // near

        // Gribb-Hartmann row combinations are NOT unit planes: at fov 75 the bottom/top
        // plane normal carries |n| ~ 1.6 and left/right ~ 1.2 (aspect-widened). Testing
        // `distance < -radius` against such a plane shrinks the survival margin from
        // `radius` to `radius / |n|` - at 75 deg that culles every edge tile whose sphere
        // pokes less than ~40% of its radius into the view, which showed up as black
        // wedge-shaped notches along the viewport edges and, near the ground, as whole
        // missing tiles exposing the neighbours' skirts. Normalising puts the signed
        // distance in metres and makes the sphere test exact.
        for ( math::Vec4 &plane : frustum_planes_ )
        {
            const double normal_length =
                std::sqrt( plane.x * plane.x + plane.y * plane.y + plane.z * plane.z );
            if ( normal_length > 1e-12 )
            {
                plane.x /= normal_length;
                plane.y /= normal_length;
                plane.z /= normal_length;
                plane.w /= normal_length;
            }
        }
    }

    bool GlobeTileLayer::sphere_intersects_frustum( const math::Vec3 &center_world,
                                                    const double radius ) const
    {
        for ( const math::Vec4 &plane : frustum_planes_ )
        {
            // Signed distance of the centre; a sphere survives while it is not fully
            // behind any plane.
            const double distance = plane.x * center_world.x + plane.y * center_world.y +
                                    plane.z * center_world.z + plane.w;
            if ( distance < -radius )
            {
                return false;
            }
        }
        return true;
    }

    // ---- traversal ----

    void GlobeTileLayer::rebase()
    {
        // The shared frame's origin moved. Every tile mesh is authored relative to its own ECEF
        // centre, so nothing about the geometry changed - only where those centres land in this
        // node's local space. One transform write per live tile, and no vertex work.
        resolve_frame();

        for ( GlobeTile *root : roots_ )
        {
            reapply_tile_placement( root );
        }
    }

    void GlobeTileLayer::reapply_tile_placement( GlobeTile *tile )
    {
        if ( tile == nullptr )
        {
            return;
        }

        // The cached bounding sphere is expressed in the *old* origin's local space, so the
        // horizon-culling point and the frustum tests would otherwise keep answering for where
        // the tile used to be. Dropped rather than translated: compute_bounds() samples the
        // surface again, which is exact and happens lazily anyway.
        tile->bounds_computed = false;
        tile->occludee_valid = false;

        if ( tile->mesh != nullptr )
        {
            // Re-derived from the ECEF centre, never accumulated off the previous placement, so
            // a long run of rebases cannot drift.
            const math::Vec3 center_local = mesh_point( tile->rtc_center_ecef );
            tile->mesh->set_position( toGodotVector( center_local ) );

            // The shader's copy of the same number. It is not redundant with the node position:
            // Godot feeds the vertex shader model-space VERTEX, and the model matrix's
            // translation is applied afterwards, so the ground pass has no other way to recover
            // where on Earth this tile is. See GlobeAtmosphereShading's u_tile_center.
            Ref<ShaderMaterial> material = tile->mesh->get_material_override();
            if ( material.is_valid() )
            {
                material->set_shader_parameter( "u_tile_center", toGodotVector( center_local ) );
            }
        }

        if ( tile->has_children() )
        {
            const int count = tile->child_count();
            for ( int index = 0; index < count; ++index )
            {
                reapply_tile_placement( tile->child( index ) );
            }
        }
    }

    void GlobeTileLayer::update_tiles()
    {
        if ( roots_[0] == nullptr )
        {
            for ( int i = 0; i < 4; ++i )
            {
                // Level 1 is the top of the tree: its four quadrants span the whole
                // Mercator range, matching the reference roots.
                roots_[i] = new GlobeTile( i % 2, i / 2, 1, nullptr );
            }
        }

        godot::Camera3D *camera = resolve_camera();
        if ( camera == nullptr )
        {
            return;
        }

        // The frame has to be resolved *before* the framing block below, not after it:
        // apply_editor_framing() builds its pose out of frame_ and the ellipsoid centre, and
        // on the very first process frame an unresolved frame is all zeroes, which puts the
        // camera at the anchor and makes the opening pose jump on the next frame.
        resolve_frame();
        center_cache_ = frame_.ellipsoid_center_local();

        if ( needs_framing_ )
        {
#ifdef TILES3D_EDITOR_TARGET
            // Fly the editor camera to the opening pose, so the globe is framed at a glance
            // the moment the scene opens.
            if ( apply_editor_framing() )
            {
                // Only settled once the editor camera actually exists; otherwise the very
                // first frames of an editor session swallow the request and the globe stays
                // off-screen at (0, 0, 10) for the rest of the session. Then keep re-applying
                // until a frame really draws something, and never longer than the cap, so a
                // camera that never resolves does not leave the user locked out of their
                // own viewport.
                ++framing_frames_;
                const bool hold_until_loaded =
                    !tiles_rendered_once_ && framing_frames_ < kFramingFrameCap;
                if ( framing_frames_ >= kFramingFrames && !hold_until_loaded )
                {
                    needs_framing_ = false;
                }
            }
#endif
        }

        // Camera in mesh space, and in Y-up ECEF for the horizon culling test.
        const Vector3 world_camera = camera->get_global_position();
        const Vector3 local_camera = to_local( world_camera );
        camera_local_ = math::Vec3( local_camera.x, local_camera.y, local_camera.z );
        camera_ecef_ = frame_.camera_ecef_y_up( camera_local_ );

        viewport_height_ = static_cast<double>( get_viewport() != nullptr
                                                    ? static_cast<int>( get_viewport()->get_visible_rect().size.y )
                                                    : 600 );
#ifdef TILES3D_EDITOR_TARGET
        Engine *engine_check = Engine::get_singleton();
        if ( engine_check != nullptr && engine_check->is_editor_hint() )
        {
            godot::EditorInterface *editor = godot::EditorInterface::get_singleton();
            godot::SubViewport *viewport = editor != nullptr ? editor->get_editor_viewport_3d() : nullptr;
            if ( viewport != nullptr )
            {
                // Viewport::get_visible_rect() in the editor measures 2.0 units and would
                // shrink the SSE by orders of magnitude; get_size() is the pixel truth.
                viewport_height_ = static_cast<double>( viewport->get_size().y );
            }
        }
#endif
        // The editor viewport camera keeps its own 0.1 / 4000 m defaults, which cannot even
        // reach the Earth's centre; the run-time camera stays the scene's own, which
        // GlobeCameraController already configures. assert_editor_clip() is a no-op for the
        // latter, and _on_frame_pre_draw() re-asserts it again the instant before the frame
        // is drawn, which is what beats the editor's own rewrite during navigation.
        assert_editor_clip( camera );

        fov_radians_ = static_cast<double>( camera->get_fov() ) * kDegreesToRadians;

        compute_frustum( camera );

        ++frame_number_;
        rendered_count_ = 0;
        max_selected_level_ = 0;
        high_queue_.clear();
        medium_queue_.clear();
        replacement_queue_.mark_start_of_render_frame();

        select_tiles();
        for ( GlobeTile *root : roots_ )
        {
            hide_stale_tiles( root );
        }
        if ( rendered_count_ > 0 )
        {
            tiles_rendered_once_ = true;
        }

        process_load_queue();
        trim_tiles();
    }

    double GlobeTileLayer::compute_distance_to_tile( GlobeTile *tile )
    {
        tile->compute_bounds( frame_ );
        tile->distance = std::max( 0.0, glm::length( camera_local_ - tile->bounds_center ) -
                                             tile->bounds_radius );
        return tile->distance;
    }

    double GlobeTileLayer::screen_space_error( const GlobeTile *tile ) const
    {
        const double geometric_error = math::levelGeometricError( tile->level );
        const double distance = std::max( tile->distance, 1.0 );
        const double sse_denominator = 2.0 * std::tan( fov_radians_ / 2.0 );
        return geometric_error * viewport_height_ / ( distance * sse_denominator );
    }

    bool GlobeTileLayer::compute_tile_visibility( GlobeTile *tile )
    {
        tile->compute_bounds( frame_ );

        // World-space sphere for the engine frustum. The layer is meant to sit at identity;
        // a scaled ancestor would need a radius scale too.
        const Vector3 center_world = to_global(
            Vector3( static_cast<float>( tile->bounds_center.x ),
                     static_cast<float>( tile->bounds_center.y ),
                     static_cast<float>( tile->bounds_center.z ) ) );
        // The frustum lives in world space; planes were extracted from the world->clip
        // matrix, so test the sphere in world coordinates directly.
        const math::Vec3 center_world_v{ center_world.x, center_world.y, center_world.z };
        if ( !sphere_intersects_frustum( center_world_v, tile->bounds_radius ) )
        {
            return false;
        }

        // Horizon culling: tiles on the far side of the planet are inside no frustum plane
        // the near side fails, so curvature has to reject them.
        if ( tile->occludee_valid &&
             !math::isScaledSpacePointVisible( tile->occludee_point, camera_ecef_ ) )
        {
            return false;
        }
        return true;
    }

    void GlobeTileLayer::select_tiles()
    {
        // Near roots first, so the traversal fills the closest part of the planet before
        // spending the load budget on the far side.
        GlobeTile *sorted[4] = { roots_[0], roots_[1], roots_[2], roots_[3] };
        std::sort( sorted, sorted + 4,
                   [this]( GlobeTile *a, GlobeTile *b )
                   { return compute_distance_to_tile( a ) < compute_distance_to_tile( b ); } );
        for ( GlobeTile *root : sorted )
        {
            visit_if_visible( root );
        }
    }

    void GlobeTileLayer::visit_if_visible( GlobeTile *tile )
    {
        replacement_queue_.mark_tile_rendered( tile );
        if ( !compute_tile_visibility( tile ) )
        {
            return;
        }
        visit_tile( tile );
    }

    void GlobeTileLayer::visit_tile( GlobeTile *tile )
    {
        compute_distance_to_tile( tile );

        // Imagery availability gate. A 404 means the cache pyramid ends at this tile, and
        // by dyadic subdivision every descendant would 404 too. Showing this tile and
        // stopping is what kills the hard-edged "tile matrix" up close: descending would
        // subdivide the view into hundreds of leaf meshes that each re-sample a sliver of
        // an upsampled ancestor (worst case one ancestor texel per tile - a flat colour
        // square), with a half-texel discontinuity stitched between every pair. The tile's
        // appearance resolves to the nearest loaded ancestor either way.
        if ( tile->imagery_missing )
        {
            show_tile_this_frame( tile );
            return;
        }

        const bool meets_sse = screen_space_error( tile ) <= maximum_screen_space_error_;
        if ( meets_sse || tile->level >= maximum_level_ )
        {
            // Refined enough (or as deep as allowed): show it, request imagery gently.
            if ( tile->needs_loading() )
            {
                queue_tile_load( tile, medium_queue_ );
            }
            show_tile_this_frame( tile );
            return;
        }

        // Needs refining: its own imagery is wanted urgently (it is the fallback the
        // children will render until their textures arrive), then descend.
        if ( tile->needs_loading() )
        {
            queue_tile_load( tile, high_queue_ );
        }
        GlobeTile *children[4] = { tile->child( 0 ), tile->child( 1 ), tile->child( 2 ),
                                   tile->child( 3 ) };
        std::sort( children, children + 4,
                   [this]( GlobeTile *a, GlobeTile *b )
                   { return compute_distance_to_tile( a ) < compute_distance_to_tile( b ); } );
        for ( GlobeTile *child_tile : children )
        {
            visit_if_visible( child_tile );
        }
    }

    void GlobeTileLayer::show_tile_this_frame( GlobeTile *tile )
    {
        if ( tile->mesh == nullptr )
        {
            create_tile_mesh( tile );
        }
        update_tile_appearance( tile );
        if ( tile->mesh != nullptr )
        {
            tile->mesh->set_visible( true );
        }
        tile->selection_frame = frame_number_;
        ++rendered_count_;
        max_selected_level_ = std::max( max_selected_level_, tile->level );
    }

    void GlobeTileLayer::hide_stale_tiles( GlobeTile *tile )
    {
        if ( tile->mesh != nullptr && tile->selection_frame != frame_number_ )
        {
            tile->mesh->set_visible( false );
        }
        if ( tile->has_children() )
        {
            for ( int i = 0; i < 4; ++i )
            {
                hide_stale_tiles( tile->child( i ) );
            }
        }
    }

    // ---- loading ----

    String GlobeTileLayer::tile_url( const GlobeTile &tile ) const
    {
        char quadkey[24] = { 0 };
        math::tileXYToQuadKey( tile.x, tile.y, tile.level, quadkey, sizeof( quadkey ) );

        String url = url_template_;
        url = url.replace( "{z}", String::num_int64( tile.level ) );
        url = url.replace( "{x}", String::num_int64( tile.x ) );
        url = url.replace( "{y}", String::num_int64( tile.y ) );
        url = url.replace( "{q}", String::utf8( quadkey ) );
        return url;
    }

    void GlobeTileLayer::queue_tile_load( GlobeTile *tile, std::vector<GlobeTile *> &queue )
    {
        if ( !tile->needs_loading() )
        {
            return;
        }
        if ( tile->state == GlobeTile::LoadState::FAILED &&
             now_seconds() - tile->fail_time < 15.0 )
        {
            return;
        }

        // Priority: facing the camera beats facing away at equal distance, matching the
        // reference's (1 - dot) weighting.
        const math::TileRectangle r = tile->rectangle;
        const math::Vec3 center =
            math::geodeticToYUp( ( r.west + r.east ) / 2.0, ( r.north + r.south ) / 2.0, 0.0 );
        const math::Vec3 tile_direction = glm::normalize( center );
        const math::Vec3 camera_direction = glm::normalize( camera_ecef_ );
        tile->load_priority = ( 1.0 - glm::dot( tile_direction, camera_direction ) ) * tile->distance;

        if ( std::find( queue.begin(), queue.end(), tile ) == queue.end() )
        {
            queue.push_back( tile );
        }
    }

    void GlobeTileLayer::process_load_queue()
    {
        const double deadline = now_seconds() + 0.006; // 6 ms time slice, like the reference
        std::vector<GlobeTile *> *queues[2] = { &high_queue_, &medium_queue_ };
        for ( std::vector<GlobeTile *> *queue : queues )
        {
            std::sort( queue->begin(), queue->end(),
                       []( const GlobeTile *a, const GlobeTile *b )
                       { return a->load_priority < b->load_priority; } );
            for ( size_t i = 0; i < queue->size(); )
            {
                GlobeTile *tile = ( *queue )[i];
                if ( !tile->needs_loading() )
                {
                    queue->erase( queue->begin() + static_cast<long>( i ) );
                    continue;
                }
                if ( inflight_count_ >= max_concurrent_requests_ )
                {
                    return;
                }
                const int request_id = next_request_id_++;
                dispatch_load( tile, request_id );
                queue->erase( queue->begin() + static_cast<long>( i ) );
                if ( now_seconds() > deadline )
                {
                    return;
                }
            }
        }
    }

    void GlobeTileLayer::dispatch_load( GlobeTile *tile, const int request_id )
    {
        tile->state = GlobeTile::LoadState::LOADING;
        ++inflight_count_;

        HTTPRequest *request = memnew( HTTPRequest );
        add_child( request );
        // TLS uses Godot's default verification (the engine bundles a Mozilla CA set).
        // If a deployment ever needs to tolerate broken certificate stores, this is the
        // line to change - the imagery itself is public and non-sensitive.
        request->set_timeout( 5 ); // TEMP diag: force a callback within the capture window

        PendingRequest pending;
        pending.tile = tile;
        pending.request = request;
        pending_.emplace( request_id, pending );

        request->connect( "request_completed",
                          Callable( this, "_on_tile_request_completed" ).bind( request_id ) );
        // A rejected request() never fires the signal; if we left the slot occupied the
        // layer would silently stall at max concurrency. Route it through the failure path.
        if ( request->request( tile_url( *tile ) ) != godot::Error::OK )
        {
            WARN_PRINT( String( "GlobeTileLayer: request() rejected for %s" ).format(
                godot::Array::make( tile_url( *tile ) ) ) );
            pending_.erase( request_id );
            remove_child( request );
            request->queue_free();
            --inflight_count_;
            tile->state = GlobeTile::LoadState::FAILED;
            tile->fail_time = now_seconds();
        }
    }

    void GlobeTileLayer::_on_tile_request_completed( const int p_result, const int p_response_code,
                                                     godot::PackedStringArray p_headers,
                                                     godot::PackedByteArray p_body,
                                                     const int p_request_id )
    {
        (void)p_headers;

        const auto found = pending_.find( p_request_id );
        if ( found == pending_.end() )
        {
            return;
        }
        GlobeTile *tile = found->second.tile;
        if ( found->second.request != nullptr )
        {
            found->second.request->queue_free();
        }
        pending_.erase( found );
        --inflight_count_;

        if ( tile == nullptr )
        {
            return;
        }

        const int kResultSuccess = 0; // HTTPRequest.RESULT_SUCCESS
        bool decoded = false;
        if ( p_result == kResultSuccess && p_response_code == 200 && p_body.size() > 0 )
        {
            // The buffer decoders are engine-static but surface as members in godot-cpp;
            // any instance works, the payload never touches it.
            Ref<Image> image;
            image.instantiate();
            Error decode_error = image->load_jpg_from_buffer( p_body );
            if ( decode_error != godot::Error::OK )
            {
                decode_error = image->load_png_from_buffer( p_body );
            }
            if ( decode_error == godot::Error::OK )
            {
                // Mipmaps are what makes an under-resolved tile read as a smooth blur instead
                // of a checkerboard: when the camera is close to the 3D Tiles dataset the
                // background imagery is still low-LOD, and without a mip chain every texel
                // aliases at screen scale - adjacent tiles then show hard colour boundaries
                // (the "mosaic" look). This mirrors what Cesium gets from GPU-side
                // generateMipmap; Godot needs the chain baked into the Image up front and a
                // mipmap-capable sampler on the shader side (filter_linear_mipmap in
                // GlobeAtmosphereShading).
                image->generate_mipmaps();
                tile->texture = ImageTexture::create_from_image( image );
                tile->renderable = true;
                tile->state = GlobeTile::LoadState::DONE;
                ++texture_version_;
                decoded = true;
            }
        }

        if ( !decoded )
        {
            tile->state = GlobeTile::LoadState::FAILED;
            tile->fail_time = now_seconds();
            // A 404 from the static tile server is definitive: this quadkey simply is not
            // in the cache. Flag it so the traversal stops subdividing here (see
            // visit_tile) instead of requesting the whole missing subtree over and over.
            tile->imagery_missing = ( p_response_code == 404 );

            // A failing tile server is indistinguishable from the placeholder colour without
            // this line. String::format() wants {0} placeholders, not printf ones, so build
            // the text explicitly; and throttle, because a partially populated tile cache
            // legitimately 404s a lot at the deep levels.
            const double stamp = now_seconds();
            if ( stamp - last_failure_report_ > 2.0 )
            {
                last_failure_report_ = stamp;
                ++failure_report_count_;
                WARN_PRINT( "GlobeTileLayer: tile request failed result=" +
                            godot::String::num_int64( p_result ) + " code=" +
                            godot::String::num_int64( p_response_code ) + " url=" +
                            tile_url( *tile ) + " (report #" +
                            godot::String::num_int64( failure_report_count_ ) + ")" );
            }
        }
    }

    void GlobeTileLayer::cancel_pending_requests()
    {
        for ( auto &[id, pending] : pending_ )
        {
            if ( pending.request != nullptr )
            {
                pending.request->cancel_request();
                pending.request->queue_free();
                pending.request = nullptr;
            }
            if ( pending.tile != nullptr && pending.tile->state == GlobeTile::LoadState::LOADING )
            {
                pending.tile->state = GlobeTile::LoadState::START;
            }
        }
        pending_.clear();
        inflight_count_ = 0;
    }

    // ---- mesh + appearance ----

    void GlobeTileLayer::create_tile_mesh( GlobeTile *tile )
    {
        const int segments = tile_edge_segments( tile->level );
        const math::TileRectangle r = tile->rectangle;
        const math::TileRectangle g = tile->geometry_rectangle();

        const double mercator_south = math::mercatorY( r.south );
        const double mercator_north = math::mercatorY( r.north );
        const double mercator_span = mercator_north - mercator_south;

        const int columns = segments + 1;
        const int top_vertex_count = columns * columns;

        const double max_radius = math::kWgs84SemiMajorAxis;
        const double root_max_seg_angle = math::kPi / 32.0; // level-1 root: 180 degrees / 32
        const double seg_angle = std::max(
            std::max( g.east - g.west, g.north - g.south ) / static_cast<double>( segments ),
            root_max_seg_angle );
        // Skirt depth covers the worst chord sagitta of any neighbour level this tile can
        // meet, times 1.6 plus margin - the reference's anti-crack recipe.
        const double dip = max_radius * ( 1.0 - std::cos( seg_angle * 0.5 ) );
        const double skirt_height = dip * 1.6 + 600.0;

        PackedVector3Array positions;
        PackedVector2Array uvs;
        positions.resize( top_vertex_count );
        uvs.resize( top_vertex_count );

        // ---- relative-to-centre authoring (see GlobeTile::rtc_center_ecef) ----
        //
        // Vertices go in relative to the tile's own centre and the node carries the offset, so
        // the numbers that reach the GPU are tile-sized instead of Earth-sized. That is what
        // lets the shared frame's origin move without touching a single vertex: an origin shift
        // only changes where this centre lands in the layer's local space, so rebase() is one
        // set_position() per live tile rather than a full re-authoring of every mesh.
        //
        // The centre is taken from the *geometry* rectangle (the one the mesh actually spans),
        // so the skirt, which is mirrored off the rim, stays symmetric about it.
        const math::Vec3 center_ecef = math::wgs84ToCartesian(
            0.5 * ( g.west + g.east ), 0.5 * ( g.north + g.south ), 0.0 );
        const math::Vec3 center_local = mesh_point( center_ecef );
        tile->rtc_center_ecef = center_ecef;

        // Z-up ECEF positions kept alongside, so the skirt normals can be rotated by the
        // frame instead of being reverse-engineered from mapped vertices.
        std::vector<math::Vec3> ecef_positions;
        ecef_positions.reserve( top_vertex_count );

        for ( int row = 0; row < columns; ++row )
        {
            // Row 0 is the north edge; the same orientation Globe3D's surface uses, so the
            // winding proven correct there carries over.
            const double latitude = g.north - ( g.north - g.south ) *
                                                   static_cast<double>( row ) /
                                                   static_cast<double>( segments );
            double v = 0.5;
            if ( mercator_span > 0.0 )
            {
                // Rows are linear in mercator Y - that is what makes a sub-rectangle of an
                // ancestor texture line up exactly (and what a naive latitude-linear UV
                // gets wrong at high latitudes).
                v = std::clamp( ( mercator_north - math::mercatorY( latitude ) ) / mercator_span,
                                0.0, 1.0 );
            }
            for ( int column_index = 0; column_index < columns; ++column_index )
            {
                const double longitude =
                    g.west + ( g.east - g.west ) * static_cast<double>( column_index ) /
                                 static_cast<double>( segments );
                const math::Vec3 ecef = math::wgs84ToCartesian( longitude, latitude, 0.0 );
                const math::Vec3 local = mesh_point( ecef );
                const int index = row * columns + column_index;
                positions[index] = Vector3( static_cast<float>( local.x - center_local.x ),
                                            static_cast<float>( local.y - center_local.y ),
                                            static_cast<float>( local.z - center_local.z ) );
                uvs[index] = Vector2( static_cast<float>( column_index ) / static_cast<float>( segments ),
                                      static_cast<float>( v ) );
                ecef_positions.push_back( ecef );
            }
        }

        PackedInt32Array indices;

        for ( int row = 0; row < segments; ++row )
        {
            for ( int column_index = 0; column_index < segments; ++column_index )
            {
                const int top_left = row * columns + column_index;
                const int top_right = top_left + 1;
                const int bottom_left = top_left + columns;
                const int bottom_right = bottom_left + 1;
                // Same winding as Globe3D::rebuild_surface (front face outward).
                indices.push_back( top_left );
                indices.push_back( bottom_left );
                indices.push_back( bottom_right );
                indices.push_back( top_left );
                indices.push_back( bottom_right );
                indices.push_back( top_right );
            }
        }

        // Skirt: copy the rim vertices, push them towards the ellipsoid centre along the
        // geodetic normal, and stitch a wall. Double-sided material (see below) seals the
        // crack from either side - the reference also renders skirts two-sided.
        const int rim_length = 4 * segments;
        std::vector<int> rim;
        rim.reserve( rim_length );
        for ( int column_index = 0; column_index <= segments; ++column_index )
        {
            rim.push_back( ( segments ) *columns + column_index ); // south edge, west -> east
        }
        for ( int row = segments - 1; row >= 0; --row )
        {
            rim.push_back( row * columns + segments ); // east edge, south -> north
        }
        for ( int column_index = segments - 1; column_index >= 0; --column_index )
        {
            rim.push_back( 0 * columns + column_index ); // north edge, east -> west
        }
        for ( int row = 1; row <= segments - 1; ++row )
        {
            rim.push_back( row * columns + 0 ); // west edge, north -> south
        }

        const int total_vertices = top_vertex_count + rim_length;
        positions.resize( total_vertices );
        uvs.resize( total_vertices );
        for ( int k = 0; k < rim_length; ++k )
        {
            const int rim_index = rim[static_cast<size_t>( k )];
            const Vector3 p = positions[rim_index];
            const Vector2 p_uv = uvs[rim_index];

            const math::Vec3 &ecef = ecef_positions[static_cast<size_t>( rim_index )];
            const math::Vec3 normal_ecef{ ecef.x / ( math::kWgs84SemiMajorAxis *
                                                     math::kWgs84SemiMajorAxis ),
                                          ecef.y / ( math::kWgs84SemiMinorAxis *
                                                     math::kWgs84SemiMinorAxis ),
                                          ecef.z / ( math::kWgs84SemiMajorAxis *
                                                     math::kWgs84SemiMajorAxis ) };
            // Rotate the geodetic normal into the frame; rotations preserve the drop
            // length, so the skirt depth is unchanged.
            const math::Vec3 normal_local = frame_.rotate_local( normal_ecef );
            const double normal_length = glm::length( normal_local );
            const double drop = skirt_height / std::max( normal_length, 1e-12 );
            const Vector3 skirt_point = p - Vector3(
                                                 static_cast<float>( normal_local.x * drop ),
                                                 static_cast<float>( normal_local.y * drop ),
                                                 static_cast<float>( normal_local.z * drop ) );

            const int skirt_index = top_vertex_count + k;
            positions[skirt_index] = skirt_point;
            uvs[skirt_index] = p_uv;

            const int next_rim = rim[static_cast<size_t>( ( k + 1 ) % rim_length )];
            const int skirt_next = top_vertex_count + ( k + 1 ) % rim_length;
            indices.push_back( rim_index );
            indices.push_back( skirt_index );
            indices.push_back( next_rim );
            indices.push_back( next_rim );
            indices.push_back( skirt_index );
            indices.push_back( skirt_next );
        }

        Ref<ArrayMesh> mesh;
        mesh.instantiate();
        godot::Array surface_arrays;
        surface_arrays.resize( godot::Mesh::ARRAY_MAX );
        surface_arrays[godot::Mesh::ARRAY_VERTEX] = positions;
        surface_arrays[godot::Mesh::ARRAY_TEX_UV] = uvs;
        surface_arrays[godot::Mesh::ARRAY_INDEX] = indices;
        mesh->add_surface_from_arrays( godot::Mesh::PRIMITIVE_TRIANGLES, surface_arrays );

        Ref<ShaderMaterial> material;
        material.instantiate();
        // The ground-atmosphere pass lives here, not on Globe3D's own ellipsoid.
        //
        // The reference injects the same integral into the *tile* material
        // (applyDayNightShading, atmosphere.ts:390) and Cesium computes it per tile fragment
        // (GlobeFS.glsl:516), because the tile material is the thing that actually draws the
        // planet. An earlier version of this port put the pass on Globe3D::Surface instead -
        // a mesh these tiles cover completely, so it contributed no pixels to any frame while
        // still reporting a live material and correct uniforms.
        //
        // Because the shader replaces StandardMaterial3D wholesale it has to reproduce the
        // feature set that was there before, which is exactly: UNSHADED, CULL_DISABLED (the
        // skirts must seal LOD cracks seen from both sides, like the reference's DoubleSide
        // tile material), the placeholder albedo, and a mercator UV scale/offset.
        material->set_shader( GlobeAtmosphereShading::ground_shader() );
        material->set_shader_parameter( "u_has_texture", false );
        material->set_shader_parameter( "u_base_color",
                                        Vector3( kPlaceholderColor.r, kPlaceholderColor.g,
                                                 kPlaceholderColor.b ) );
        // The centre the vertices were authored against. The ground shader needs it because
        // Godot hands it VERTEX in model space, which excludes this node's translation - see
        // u_tile_center in GlobeAtmosphereShading's ground shader. Kept in step with the node's
        // position by reapply_tile_placement() whenever the frame origin moves.
        material->set_shader_parameter( "u_tile_center", toGodotVector( center_local ) );

        tile->base_uvs = uvs;
        tile->appearance_source = nullptr;
        tile->texture_version = -1;

        MeshInstance3D *mesh_instance = memnew( MeshInstance3D );
        mesh_instance->set_mesh( mesh );
        mesh_instance->set_material_override( material );
        // The offset the vertices were authored against (see above). Re-derived from the ECEF
        // centre - never accumulated - so it cannot drift after a series of origin shifts.
        mesh_instance->set_position( toGodotVector( center_local ) );
        mesh_instance->set_name( String::utf8( "Tile_" ) + String::num_int64( tile->level ) + "_" +
                                 String::num_int64( tile->x ) + "_" + String::num_int64( tile->y ) );
        // INTERNAL_MODE_FRONT keeps the streamed tiles out of get_children(), the way
        // Globe3D hides its internal surface/graticule nodes - and without an owner the
        // editor never writes them into the .tscn.
        add_child( mesh_instance, false, godot::Node::INTERNAL_MODE_FRONT );
        tile->mesh = mesh_instance;
    }

    GlobeTile *GlobeTileLayer::find_appearance_source( GlobeTile *tile )
    {
        for ( GlobeTile *ancestor = tile; ancestor != nullptr; ancestor = ancestor->parent )
        {
            if ( ancestor->state == GlobeTile::LoadState::DONE && ancestor->texture.is_valid() )
            {
                return ancestor;
            }
        }
        return nullptr;
    }

    void GlobeTileLayer::update_tile_appearance( GlobeTile *tile )
    {
        if ( tile->mesh == nullptr )
        {
            return;
        }
        GlobeTile *source = find_appearance_source( tile );
        Ref<ShaderMaterial> material = tile->mesh->get_material_override();
        if ( material.is_null() )
        {
            return;
        }

        if ( source == nullptr )
        {
            if ( tile->appearance_source != nullptr || tile->texture_version != texture_version_ )
            {
                material->set_shader_parameter( "u_has_texture", false );
                material->set_shader_parameter( "u_albedo_texture", Ref<godot::Texture2D>() );
                material->set_shader_parameter( "u_base_color",
                                                Vector3( kPlaceholderColor.r, kPlaceholderColor.g,
                                                         kPlaceholderColor.b ) );
                material->set_shader_parameter( "u_uv_scale", Vector2( 1.0f, 1.0f ) );
                material->set_shader_parameter( "u_uv_offset", Vector2( 0.0f, 0.0f ) );
                tile->appearance_source = nullptr;
                tile->texture_version = texture_version_;
            }
            return;
        }

        if ( source == tile->appearance_source && tile->texture_version == texture_version_ )
        {
            return;
        }

        // Map this tile's mercator rectangle into the ancestor's. UVs stay tile-local in
        // the vertices ([0,1] over the tile), so the mapping is a plain scale+offset - the
        // mercator linearity is what makes that exact.
        const math::TileRectangle &tr = tile->rectangle;
        const math::TileRectangle &sr = source->rectangle;
        double du = ( tr.east - tr.west ) / ( sr.east - sr.west );
        double u0 = ( tr.west - sr.west ) / ( sr.east - sr.west );
        const double smn = math::mercatorY( sr.north );
        const double sms = math::mercatorY( sr.south );
        double dv = ( math::mercatorY( tr.north ) - math::mercatorY( tr.south ) ) / ( smn - sms );
        double v0 = ( smn - math::mercatorY( tr.north ) ) / ( smn - sms );

        // No half-texel inset here, on purpose. The edge fragment row of this tile and of
        // its neighbour both sample the exact same boundary of the ancestor texture, so
        // bilinear filtering blends the two straddling texels identically on both sides -
        // the seam is a soft one-texel gradient, exactly like the reference (which does no
        // inset either). The half-texel inset tried here earlier shifted each tile's
        // sampled window inward, which in the magnified regime of a partial imagery cache
        // (a tile stretching many screen pixels per ancestor texel) replaced that soft
        // gradient with a hard texel step pinned to every tile edge - the sharp "tile
        // matrix" grid. Tile rectangles are dyadic fractions of the ancestor's, so tile
        // edges land exactly on ancestor texel boundaries and no inset is needed for
        // alignment. When source == tile the edges hit the texture border, where the
        // default clamp-to-edge sampler is already clean.

        material->set_shader_parameter( "u_has_texture", true );
        material->set_shader_parameter( "u_albedo_texture", source->texture );
        material->set_shader_parameter( "u_uv_scale",
                                        Vector2( static_cast<float>( du ),
                                                 static_cast<float>( dv ) ) );
        material->set_shader_parameter( "u_uv_offset",
                                        Vector2( static_cast<float>( u0 ),
                                                 static_cast<float>( v0 ) ) );

        tile->appearance_source = source;
        tile->texture_version = texture_version_;
    }

    bool GlobeTileLayer::subtree_used_this_frame( const GlobeTile *tile ) const
    {
        if ( tile->selection_frame == frame_number_ )
        {
            return true;
        }
        if ( tile->has_children() )
        {
            for ( int i = 0; i < 4; ++i )
            {
                // child() lazily creates, which is safe here: has_children() just said the
                // four already exist, and the pointer is only used read-only.
                GlobeTile *kid = const_cast<GlobeTile *>( tile )->child( i );
                if ( subtree_used_this_frame( kid ) )
                {
                    return true;
                }
            }
        }
        return false;
    }

    void GlobeTileLayer::free_subtree_resources( GlobeTile *tile )
    {
        if ( tile->in_replacement_queue )
        {
            replacement_queue_.remove( tile );
        }
        if ( tile->has_children() )
        {
            for ( int i = 0; i < 4; ++i )
            {
                free_subtree_resources( tile->child( i ) );
            }
        }
        if ( tile->mesh != nullptr )
        {
            if ( tile->mesh->get_parent() == this )
            {
                remove_child( tile->mesh );
            }
            tile->mesh->queue_free();
        }
        tile->free_resources();
    }

    void GlobeTileLayer::trim_tiles()
    {
        replacement_queue_.trim_tiles(
            tile_cache_size_,
            [this]( GlobeTile *candidate ) { return subtree_used_this_frame( candidate ); },
            [this]( GlobeTile *candidate ) { free_subtree_resources( candidate ); } );
    }

    void GlobeTileLayer::destroy_roots()
    {
        for ( GlobeTile *&root : roots_ )
        {
            if ( root != nullptr )
            {
                free_subtree_resources( root );
                delete root;
                root = nullptr;
            }
        }
    }

} // namespace tiles3d
