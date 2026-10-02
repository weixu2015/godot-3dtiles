// SPDX-License-Identifier: Unlicense

#include "GlobeCameraController.h"

#include "Georeference3D.h"
#include "Globe3D.h"

#include "core/math/GeoMath.h"
#include "core/math/Mat4.h"

#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/input_event_mouse_button.hpp"
#include "godot_cpp/classes/input_event_mouse_motion.hpp"
#include "godot_cpp/classes/scene_tree.hpp"
#include "godot_cpp/classes/time.hpp"
#include "godot_cpp/classes/viewport.hpp"
// Needed for the Window -> Node upcast: SceneTree::get_root() declares its return type
// only through this header, and without it the georeference search below does not compile.
#include "godot_cpp/classes/window.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/quaternion.hpp"

#include <cmath>

namespace tiles3d
{
    using godot::Basis;
    using godot::ClassDB;
    using godot::D_METHOD;
    using godot::InputEvent;
    using godot::InputEventMouseButton;
    using godot::InputEventMouseMotion;
    using godot::MOUSE_BUTTON_LEFT;
    using godot::MOUSE_BUTTON_MIDDLE;
    using godot::MOUSE_BUTTON_RIGHT;
    using godot::MOUSE_BUTTON_WHEEL_DOWN;
    using godot::MOUSE_BUTTON_WHEEL_UP;
    using godot::Object;
    using godot::PropertyInfo;
    using godot::Quaternion;
    using godot::Ref;
    using godot::Time;
    using godot::Variant;
    using godot::Vector2;
    using godot::Vector3;

    namespace
    {
        // Constants copied from the reference (QuadtreeGlobe.ts lines 87-117).
        constexpr double kMinCameraHeight = 1.0;            // metres above the ellipsoid
        constexpr double kElLimit = 1.5607963267948966;     // pi/2 - 0.01, polar clamp
        constexpr double kMaximumMovementRatio = 0.1;
        constexpr double kHorizontalSign = 1.0;
        constexpr double kVerticalSign = 1.0;
        constexpr double kTiltHorizontalSign = 1.0;
        constexpr double kTiltVerticalSign = 1.0;

        // Wheel zoom, expressed in log-distance so one notch is a fixed zoom ratio at any
        // altitude (the reference integrates in log space for the same reason). Note how the
        // damping divides out of every one of these: the impulse and the stop threshold are
        // scaled by it, so `zoom_inertia_damping` only changes how quickly the zoom settles,
        // never how far one notch or one flick travels. Tuning feel must not silently change
        // locomotion.
        constexpr double kLogDistancePerNotch = 0.2;  // e^0.2 per notch, about 1.22x
        constexpr double kMaxFlickLogTravel = 1.0;    // cap on one flick, about 2.7x
        constexpr double kStopFlickLogTravel = 0.15;  // settle once this little is left

        constexpr double kInertiaMaxClickTime = 0.4;
        constexpr double kInertiaStopFactor = 0.001;

        /// How much of an atmosphere shell the far plane has to cover past the ellipsoid.
        /// Globe3D defaults to 1.06 radii, so 1.2 has room to spare.
        constexpr double kAtmosphereHeadroom = 1.2;

        /// Rotation-based direction interpolation that survives the near-antipodal case.
        ///
        /// Godot's Vector3::slerp builds its axis out of cross(from, to), which collapses to
        /// zero when the two are nearly opposite - and flying between two points on opposite
        /// sides of the planet is the normal case here (Taiwan to Philadelphia is about 160
        /// degrees apart). The result was a random-looking jump instead of a flight.
        godot::Vector3 slerp_direction( const godot::Vector3 &p_from, const godot::Vector3 &p_to,
                                        const double p_t )
        {
            const godot::Vector3 from = p_from.normalized();
            const godot::Vector3 to = p_to.normalized();
            const double raw_dot = static_cast<double>( from.dot( to ) );
            const double dot = raw_dot < -1.0 ? -1.0 : ( raw_dot > 1.0 ? 1.0 : raw_dot );

            if ( dot > 0.9995 )
            {
                return from.lerp( to, static_cast<float>( p_t ) ).normalized();
            }

            godot::Vector3 axis = from.cross( to );
            if ( axis.length_squared() < 1e-12 )
            {
                // Antipodal: no axis is preferred, so take any perpendicular one. The flight
                // then sweeps a half turn, which is what "the other side of the planet" means.
                axis = from.cross( godot::Vector3( 0.0f, 0.0f, 1.0f ) );
                if ( axis.length_squared() < 1e-12 )
                {
                    axis = from.cross( godot::Vector3( 0.0f, 1.0f, 0.0f ) );
                }
            }
            axis = axis.normalized();

            const double angle = std::acos( dot ) * p_t;
            // Rodrigues about `axis`.
            return ( from * static_cast<float>( std::cos( angle ) ) +
                     axis.cross( from ) * static_cast<float>( std::sin( angle ) ) )
                .normalized();
        }

        double clampd( const double value, const double low, const double high )
        {
            return value < low ? low : ( value > high ? high : value );
        }

        /// The reference `inertiaDecay`: exponential fade over time.
        double inertia_decay( const double time_seconds, const double coefficient )
        {
            return std::exp( -time_seconds / coefficient );
        }

        /// Depth-first search for a Globe3D anywhere under `node`.
        const Globe3D *find_globe_recursive( const godot::Node *node )
        {
            if ( const Globe3D *globe = Object::cast_to<Globe3D>( node ); globe != nullptr )
            {
                return globe;
            }
            for ( int i = 0; i < node->get_child_count(); ++i )
            {
                if ( const Globe3D *globe = find_globe_recursive( node->get_child( i ) );
                     globe != nullptr )
                {
                    return globe;
                }
            }
            return nullptr;
        }

        /// Nearest Georeference3D ancestor of `node`, or null.
        const Georeference3D *find_frame_ancestor( const godot::Node *node )
        {
            for ( const godot::Node *parent = node->get_parent(); parent != nullptr;
                  parent = parent->get_parent() )
            {
                if ( const Georeference3D *reference = Object::cast_to<Georeference3D>( parent ) )
                {
                    return reference;
                }
            }
            return nullptr;
        }
    } // namespace

    GlobeCameraController::GlobeCameraController() = default;

    GlobeCameraController::~GlobeCameraController() = default;

    void GlobeCameraController::_bind_methods()
    {
        ClassDB::bind_method( D_METHOD( "set_inertia_enabled", "p_enabled" ),
                              &GlobeCameraController::set_inertia_enabled );
        ClassDB::bind_method( D_METHOD( "get_inertia_enabled" ),
                              &GlobeCameraController::get_inertia_enabled );
        ClassDB::add_property( "GlobeCameraController",
                               PropertyInfo( Variant::BOOL, "inertia_enabled" ),
                               "set_inertia_enabled", "get_inertia_enabled" );

        ClassDB::bind_method( D_METHOD( "set_rotate_speed_scale", "p_scale" ),
                              &GlobeCameraController::set_rotate_speed_scale );
        ClassDB::bind_method( D_METHOD( "get_rotate_speed_scale" ),
                              &GlobeCameraController::get_rotate_speed_scale );
        ClassDB::add_property( "GlobeCameraController",
                               PropertyInfo( Variant::FLOAT, "rotate_speed_scale" ),
                               "set_rotate_speed_scale", "get_rotate_speed_scale" );

        ClassDB::bind_method( D_METHOD( "set_zoom_speed_scale", "p_scale" ),
                              &GlobeCameraController::set_zoom_speed_scale );
        ClassDB::bind_method( D_METHOD( "get_zoom_speed_scale" ),
                              &GlobeCameraController::get_zoom_speed_scale );
        ClassDB::add_property( "GlobeCameraController",
                               PropertyInfo( Variant::FLOAT, "zoom_speed_scale" ),
                               "set_zoom_speed_scale", "get_zoom_speed_scale" );

        ClassDB::bind_method( D_METHOD( "set_drag_inertia_coefficient", "p_seconds" ),
                              &GlobeCameraController::set_drag_inertia_coefficient );
        ClassDB::bind_method( D_METHOD( "get_drag_inertia_coefficient" ),
                              &GlobeCameraController::get_drag_inertia_coefficient );
        ClassDB::add_property(
            "GlobeCameraController",
            PropertyInfo( Variant::FLOAT, "drag_inertia_coefficient", godot::PROPERTY_HINT_RANGE,
                          "0,5,0.01" ),
            "set_drag_inertia_coefficient", "get_drag_inertia_coefficient" );

        ClassDB::bind_method( D_METHOD( "set_drag_inertia_max_time", "p_seconds" ),
                              &GlobeCameraController::set_drag_inertia_max_time );
        ClassDB::bind_method( D_METHOD( "get_drag_inertia_max_time" ),
                              &GlobeCameraController::get_drag_inertia_max_time );
        ClassDB::add_property(
            "GlobeCameraController",
            PropertyInfo( Variant::FLOAT, "drag_inertia_max_time", godot::PROPERTY_HINT_RANGE,
                          "0,10,0.05" ),
            "set_drag_inertia_max_time", "get_drag_inertia_max_time" );

        ClassDB::bind_method( D_METHOD( "set_zoom_inertia_damping", "p_damping" ),
                              &GlobeCameraController::set_zoom_inertia_damping );
        ClassDB::bind_method( D_METHOD( "get_zoom_inertia_damping" ),
                              &GlobeCameraController::get_zoom_inertia_damping );
        ClassDB::add_property(
            "GlobeCameraController",
            PropertyInfo( Variant::FLOAT, "zoom_inertia_damping", godot::PROPERTY_HINT_RANGE,
                          "0.5,30,0.1" ),
            "set_zoom_inertia_damping", "get_zoom_inertia_damping" );

        ClassDB::bind_method( D_METHOD( "use_reference_inertia" ),
                              &GlobeCameraController::use_reference_inertia );

        ClassDB::bind_method( D_METHOD( "set_manage_clip", "p_enabled" ),
                              &GlobeCameraController::set_manage_clip );
        ClassDB::bind_method( D_METHOD( "get_manage_clip" ),
                              &GlobeCameraController::get_manage_clip );
        ClassDB::add_property( "GlobeCameraController",
                               PropertyInfo( Variant::BOOL, "manage_clip" ),
                               "set_manage_clip", "get_manage_clip" );

        ClassDB::bind_method(
            D_METHOD( "set_camera_pose", "p_position", "p_direction", "p_up" ),
            &GlobeCameraController::set_camera_pose );
        ClassDB::bind_method( D_METHOD( "fly_to", "p_longitude_degrees", "p_latitude_degrees",
                                        "p_distance", "p_seconds" ),
                              &GlobeCameraController::fly_to,
                              DEFVAL( 1.2 ) );
        ClassDB::bind_method( D_METHOD( "is_flying" ), &GlobeCameraController::is_flying );
        ClassDB::bind_method( D_METHOD( "get_camera_direction" ),
                              &GlobeCameraController::get_camera_direction );
        ClassDB::bind_method( D_METHOD( "get_camera_up" ), &GlobeCameraController::get_camera_up );

        ClassDB::bind_method( D_METHOD( "orbit_to", "p_longitude_degrees", "p_latitude_degrees",
                                        "p_distance" ),
                              &GlobeCameraController::orbit_to );

        ClassDB::bind_method( D_METHOD( "get_distance" ), &GlobeCameraController::get_distance );
        ClassDB::bind_method( D_METHOD( "set_distance", "p_distance" ),
                              &GlobeCameraController::set_distance );

        ClassDB::bind_method( D_METHOD( "resolve_ellipsoid_center" ),
                              &GlobeCameraController::resolve_ellipsoid_center );
        ClassDB::bind_method( D_METHOD( "camera_height_above_ellipsoid" ),
                              &GlobeCameraController::camera_height_above_ellipsoid );
    }

    void GlobeCameraController::_notification( int p_what )
    {
        switch ( p_what )
        {
            case NOTIFICATION_READY:
                set_process( true );
                set_process_unhandled_input( true );
                sync_distance_from_camera();
                break;
            case NOTIFICATION_PROCESS:
                update_fly( get_process_delta_time() );
                update_zoom_easing( get_process_delta_time() );
                update_drag_inertia( get_process_delta_time() );
                // After the pose update, so the planes match the frame that is about to be
                // drawn rather than the previous one.
                update_clip_planes();
                break;
            default:
                break;
        }
    }

    // ---- interaction toggles ----

    void GlobeCameraController::set_inertia_enabled( const bool p_enabled )
    {
        inertia_enabled_ = p_enabled;
    }

    bool GlobeCameraController::get_inertia_enabled() const
    {
        return inertia_enabled_;
    }

    void GlobeCameraController::set_rotate_speed_scale( const double p_scale )
    {
        rotate_speed_scale_ = p_scale;
    }

    double GlobeCameraController::get_rotate_speed_scale() const
    {
        return rotate_speed_scale_;
    }

    void GlobeCameraController::set_zoom_speed_scale( const double p_scale )
    {
        zoom_speed_scale_ = p_scale;
    }

    double GlobeCameraController::get_zoom_speed_scale() const
    {
        return zoom_speed_scale_;
    }

    void GlobeCameraController::set_drag_inertia_coefficient( const double p_seconds )
    {
        drag_inertia_coefficient_ = p_seconds;
    }

    double GlobeCameraController::get_drag_inertia_coefficient() const
    {
        return drag_inertia_coefficient_;
    }

    void GlobeCameraController::set_drag_inertia_max_time( const double p_seconds )
    {
        drag_inertia_max_time_ = p_seconds;
    }

    double GlobeCameraController::get_drag_inertia_max_time() const
    {
        return drag_inertia_max_time_;
    }

    void GlobeCameraController::set_zoom_inertia_damping( const double p_damping )
    {
        zoom_inertia_damping_ = p_damping;
    }

    double GlobeCameraController::get_zoom_inertia_damping() const
    {
        return zoom_inertia_damping_;
    }

    void GlobeCameraController::set_manage_clip( const bool p_enabled )
    {
        manage_clip_ = p_enabled;
    }

    bool GlobeCameraController::get_manage_clip() const
    {
        return manage_clip_;
    }

    void GlobeCameraController::use_reference_inertia()
    {
        // The web reference's coefficients: a 0.9 s spin decay with no hard stop, and a
        // 2.5 /s zoom decay. Kept callable so the feel can be A/B'd against the page.
        drag_inertia_coefficient_ = 0.9;
        drag_inertia_max_time_ = 0.0; // 0 disables the cap
        zoom_inertia_damping_ = 2.5;
    }

    // ---- pivot / distance helpers ----

    double GlobeCameraController::min_distance()
    {
        // C * 1.01 in the reference; the polar radius is the smaller one, so this keeps the
        // camera outside the ellipsoid everywhere.
        return math::kWgs84SemiMinorAxis * 1.01;
    }

    double GlobeCameraController::max_distance()
    {
        return math::kWgs84MeanRadius * 8.0;
    }

    const GlobeFrame &GlobeCameraController::frame() const
    {
        // Re-resolved on every use because the carrier node can be reparented at any time;
        // resolve steps are cheap (the georeference caches its matrices).
        //
        // The frame MUST come from the same node the frame <-> parent conversions use. It
        // used to come from resolve(this), which only walks ancestors: as a sibling of
        // Georeference3D this node found nothing and fell back to "local space is Y-up
        // ECEF", while the pivot and the conversions came from the anchor's ENU frame. The
        // two differ by a rotation *and* a 6000 km translation, and a rotation preserves
        // every length - so the distance, the SSE and the height above the ellipsoid all
        // stayed right while the camera landed on the wrong side of the planet. Asking for
        // Philadelphia arrived at lon 145 lat 62.
        frame_ = GlobeFrame::from_carrier( resolve_frame_node() );
        frame_valid_ = true;
        return frame_;
    }

    // ---- frame space vs parent space ----
    //
    // The shared GlobeFrame speaks in its own local coordinates: an ENU frame anchored at the
    // georeference origin, Z-up. The camera, however, is a *sibling* of Georeference3D (and of
    // Globe3D), so get_position()/set_position() speak the parent space. When an explicit
    // Georeference3D is present those two differ by that node's own transform - the Z-up ->
    // Y-up flip lives there.
    //
    // Mixing them is nearly invisible: a rotation preserves lengths, so get_distance(),
    // camera_height_above_ellipsoid(), the screen-space error and every distance-shaped test
    // stayed correct. Only the *direction* came out wrong, which shows up as the camera
    // landing at the wrong longitude and latitude - "fly to Philadelphia" arriving somewhere
    // else entirely.

    // Depth-first search for a Georeference3D anywhere under `node`. The camera is usually a
    // *sibling* of the georeference rather than a descendant, so the ancestor walk in
    // GlobeFrame::resolve() cannot see it - and falling back to a Globe3D frame there is what
    // put flights on the wrong side of the planet (the two frames differ by a rotation, which
    // preserves every distance and therefore hid from every distance-shaped test).
    const Georeference3D *find_georeference_anywhere( const godot::Node *node )
    {
        if ( node == nullptr )
        {
            return nullptr;
        }
        if ( const Georeference3D *found = godot::Object::cast_to<Georeference3D>( node ) )
        {
            return found;
        }
        for ( int i = 0; i < node->get_child_count(); ++i )
        {
            if ( const Georeference3D *found =
                     find_georeference_anywhere( node->get_child( i ) ) )
            {
                return found;
            }
        }
        return nullptr;
    }

    const godot::Node3D *GlobeCameraController::resolve_frame_node() const
    {
        if ( const Georeference3D *reference = find_frame_ancestor( this ); reference != nullptr )
        {
            return reference;
        }

        // The camera is a sibling of Georeference3D in the supported scene layout, so the
        // ancestor walk above cannot find it. Search the whole tree before falling back to a
        // Globe3D frame: those two frames differ by a rotation and mixing them is the bug.
        if ( const godot::SceneTree *tree = get_tree(); tree != nullptr )
        {
            if ( const Georeference3D *reference =
                     find_georeference_anywhere( tree->get_root() ) )
            {
                return reference;
            }
        }

        godot::Node *root = get_parent();
        while ( root != nullptr && root->get_parent() != nullptr )
        {
            root = root->get_parent();
        }
        if ( root != nullptr )
        {
            if ( const Globe3D *globe = find_globe_recursive( root ); globe != nullptr )
            {
                return globe;
            }
        }
        return nullptr;
    }

    Vector3 GlobeCameraController::frame_point_to_world( const Vector3 &p_frame_local ) const
    {
        const godot::Node3D *carrier = resolve_frame_node();
        return carrier != nullptr ? carrier->to_global( p_frame_local ) : p_frame_local;
    }

    Vector3 GlobeCameraController::frame_point_to_parent( const Vector3 &p_frame_local ) const
    {
        const godot::Node3D *parent_3d = Object::cast_to<godot::Node3D>( get_parent() );
        const Vector3 world = frame_point_to_world( p_frame_local );
        return parent_3d != nullptr ? parent_3d->to_local( world ) : world;
    }

    Vector3 GlobeCameraController::frame_point_from_parent( const Vector3 &p_parent_local ) const
    {
        // Inverse of frame_point_to_parent(). The camera's own position lives in parent
        // space, so anything fed back into the frame's matrices (height above the
        // ellipsoid, the horizon test) has to come through here first. The two spaces
        // coincide only while the frame carrier's transform is identity - which is exactly
        // the assumption that silently breaks when Georeference3D carries the Z-up -> Y-up
        // flip.
        const godot::Node3D *carrier = resolve_frame_node();
        const godot::Node3D *parent_3d = Object::cast_to<godot::Node3D>( get_parent() );
        if ( carrier == nullptr || parent_3d == nullptr )
        {
            return p_parent_local;
        }
        return carrier->to_local( parent_3d->to_global( p_parent_local ) );
    }

    Vector3 GlobeCameraController::frame_direction_to_parent(
        const Vector3 &p_frame_direction ) const
    {
        const godot::Node3D *carrier = resolve_frame_node();
        const godot::Node3D *parent_3d = Object::cast_to<godot::Node3D>( get_parent() );
        if ( carrier == nullptr || parent_3d == nullptr )
        {
            return p_frame_direction;
        }
        // A direction has no translation, so carry a point one unit along it and keep the
        // delta. Composing the bases by hand would work too, but this goes through the same
        // Node3D transforms as everything else, so there is one code path to trust.
        const Vector3 origin_parent = parent_3d->to_local( carrier->to_global( Vector3() ) );
        const Vector3 tip_parent =
            parent_3d->to_local( carrier->to_global( p_frame_direction ) );
        return ( tip_parent - origin_parent ).normalized();
    }

    Vector3 GlobeCameraController::resolve_ellipsoid_center() const
    {
        // The Earth's centre is the origin of ECEF, so it is wherever the shared frame maps
        // (0,0,0) - about one radius from the frame origin, which sits on the ellipsoid.
        // Going through the same frame orbit_to()/fly_to() aim with is what guarantees the
        // pivot and the directions are expressed in one coordinate system; deriving them
        // separately is what made flights land in Siberia.
        const GlobeFrame &globe_frame = frame();
        const math::Vec3 center_local =
            globe_frame.to_local( math::Vec3( 0.0 ) );
        return frame_point_to_parent( Vector3( static_cast<float>( center_local.x ),
                                               static_cast<float>( center_local.y ),
                                               static_cast<float>( center_local.z ) ) );
    }

    Vector3 GlobeCameraController::resolve_pivot() const
    {
        return resolve_ellipsoid_center();
    }

    void GlobeCameraController::sync_distance_from_camera()
    {
        distance_ = ( get_position() - resolve_pivot() ).length();
    }

    double GlobeCameraController::get_distance() const
    {
        return distance_;
    }

    void GlobeCameraController::set_distance( const double p_distance )
    {
        const Vector3 pivot = resolve_pivot();
        Vector3 offset = get_position() - pivot;
        if ( offset.length() < 1e-6f )
        {
            offset = Vector3( 0.0f, 0.0f, 1.0f );
        }
        offset = offset.normalized();
        set_position( pivot + offset * static_cast<float>( p_distance ) );
        distance_ = p_distance;
        enforce_camera_above_ellipsoid();
    }

    void GlobeCameraController::update_clip_planes()
    {
        if ( !manage_clip_ )
        {
            return;
        }

        // One pair of clip planes cannot serve both ends of five orders of magnitude. The
        // scene's fixed near = 1000 m is fine for the whole planet and useless for the
        // dataset the Home button flies to: it puts a 400 m photogrammetry block entirely in
        // front of the near plane, so the flight lands correctly and shows nothing.
        //
        // near tracks the altitude: 2% of it keeps anything larger than a fiftieth of the
        // camera's height visible, which is comfortably outside every dataset while still
        // leaving the depth buffer a far plane it can survive.
        const double height = std::max( camera_height_above_ellipsoid(), 1.0 );
        const double near_value = clampd( height * 0.02, 1.0, 1.0e6 );

        // far only ever grows: the whole ellipsoid plus the atmosphere shell has to stay in
        // the frustum from wherever the camera is, and no Scene author should have to know
        // that number.
        const Vector3 pivot = resolve_pivot();
        const double centre_distance = static_cast<double>( ( get_position() - pivot ).length() );
        const double required_far =
            centre_distance + math::kWgs84SemiMajorAxis * kAtmosphereHeadroom;
        if ( static_cast<double>( get_far() ) < required_far )
        {
            set_far( static_cast<float>( required_far ) );
        }
        set_near( static_cast<float>( near_value ) );
    }

    GlobeCameraController::EllipsoidOffset GlobeCameraController::ellipsoid_offset() const
    {
        // The camera offset from the ellipsoid centre, in Y-up ECEF metres, plus the
        // scaled-space length that collapses the ellipsoid onto the unit sphere (the
        // reference's trick for turning "inside the ellipsoid" into a scalar test).
        //
        // The camera position is a *parent space* vector while the frame's matrices want
        // frame-local coordinates, so it is converted on the way in. Skipping that step is
        // invisible while the frame carrier's transform is identity and rotates the
        // measured direction by 90 degrees once it is not.
        EllipsoidOffset result;
        const GlobeFrame &globe_frame = frame();
        const Vector3 camera_local = frame_point_from_parent( get_position() );
        const math::Vec3 camera_ecef = globe_frame.to_ecef_z_up(
            math::Vec3( camera_local.x, camera_local.y, camera_local.z ) );

        // The pivot *is* the ECEF origin, so the offset from it needs no subtraction -
        // converting the camera back to ECEF already measures it.
        result.offset_y_up =
            math::Vec3( camera_ecef.x, camera_ecef.z, -camera_ecef.y ); // Z-up -> Y-up
        result.radius = glm::length( result.offset_y_up );
        result.scaled_length = std::sqrt(
            ( result.offset_y_up.x / math::kWgs84SemiMajorAxis ) *
                ( result.offset_y_up.x / math::kWgs84SemiMajorAxis ) +
            ( result.offset_y_up.y / math::kWgs84SemiMinorAxis ) *
                ( result.offset_y_up.y / math::kWgs84SemiMinorAxis ) +
            ( result.offset_y_up.z / math::kWgs84SemiMajorAxis ) *
                ( result.offset_y_up.z / math::kWgs84SemiMajorAxis ) );
        return result;
    }

    double GlobeCameraController::camera_height_above_ellipsoid() const
    {
        const EllipsoidOffset offset = ellipsoid_offset();
        if ( offset.scaled_length <= 0.0 )
        {
            return 0.0;
        }
        return offset.radius * ( 1.0 - 1.0 / offset.scaled_length );
    }

    void GlobeCameraController::enforce_camera_above_ellipsoid()
    {
        const EllipsoidOffset offset = ellipsoid_offset();
        const double scaledLength = offset.scaled_length;
        const double radius = offset.radius;
        if ( scaledLength <= 0.0 )
        {
            return;
        }

        // Push the camera back out along the same radial direction it is already on, so the
        // correction does not rotate the view.
        const Vector3 pivot = resolve_pivot();
        Vector3 relative = get_position() - pivot;
        const double height = radius * ( 1.0 - 1.0 / scaledLength );
        if ( height < kMinCameraHeight )
        {
            const double surfaceRadius = radius / scaledLength;
            const double target = surfaceRadius + kMinCameraHeight;
            if ( radius > 1e-6 )
            {
                relative = relative * static_cast<float>( target / radius );
                set_position( pivot + relative );
            }
            sync_distance_from_camera();
        }
    }

    // ---- camera manipulation ----

    void GlobeCameraController::rotate_camera_around( const Vector3 &p_pivot,
                                                      const Vector3 &p_axis, const double p_angle )
    {
        const Quaternion rotation( p_axis.normalized(), static_cast<float>( p_angle ) );

        // Position: rotate the offset from the pivot.
        const Vector3 offset = get_position() - p_pivot;
        set_position( p_pivot + rotation.xform( offset ) );

        // Orientation: premultiply, exactly like the reference quaternion.premultiply.
        set_basis( Basis( rotation ) * get_basis() );

        sync_distance_from_camera();
    }

    void GlobeCameraController::apply_orbit_drag( const double p_dx, const double p_dy )
    {
        const Vector3 pivot = resolve_pivot();
        const double distance = ( get_position() - pivot ).length();
        const double distance_ratio = distance / math::kWgs84MeanRadius;
        const double rotate_rate =
            clampd( distance_ratio - 1.0, 1.0 / 5000.0, 1.77 ) * rotate_speed_scale_;

        godot::Viewport *viewport = get_viewport();
        const double width =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.x ) : 1920.0;
        const double height =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.y ) : 1080.0;

        // Yaw about the camera's current up, then pitch about the resulting right vector,
        // with the polar angle clamped so the camera cannot flip over the pole.
        const Vector3 up = get_basis().get_column( 1 ).normalized();
        const double phi_ratio = std::min( -p_dx / width, kMaximumMovementRatio );
        const double delta_phi = kHorizontalSign * rotate_rate * phi_ratio * 2.0 * math::kPi;
        rotate_camera_around( pivot, up, delta_phi );

        const Vector3 relative = get_position() - pivot;
        const double current_el = std::asin(
            clampd( static_cast<double>( relative.y ) /
                        std::max( static_cast<double>( relative.length() ), 1e-9 ),
                    -1.0, 1.0 ) );
        const double theta_ratio = std::min( -p_dy / height, kMaximumMovementRatio );
        double delta_theta = kVerticalSign * rotate_rate * theta_ratio * math::kPi;
        delta_theta = clampd( current_el + delta_theta, -kElLimit, kElLimit ) - current_el;

        const Vector3 right = get_basis().get_column( 0 ).normalized();
        rotate_camera_around( pivot, right, delta_theta );
    }

    void GlobeCameraController::apply_tilt_drag( const double p_dx, const double p_dy )
    {
        // Right-drag tilts about the point under the cursor. Without a picking ray here, the
        // pivot is the ellipsoid centre; this keeps the same rotate-rate curve as the orbit
        // so the two feel alike.
        const Vector3 pivot = resolve_pivot();
        const double rho = ( get_position() - pivot ).length();
        const double rotate_rate =
            clampd( rho - 1.0, 1.0 / 5000.0, 1.77 ) * rotate_speed_scale_;

        godot::Viewport *viewport = get_viewport();
        const double width =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.x ) : 1920.0;
        const double height =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.y ) : 1080.0;

        const Vector3 up = get_basis().get_column( 1 ).normalized();
        const double phi_ratio = std::min( -p_dx / width, kMaximumMovementRatio );
        rotate_camera_around( pivot, up,
                              kTiltHorizontalSign * rotate_rate * phi_ratio * 2.0 * math::kPi );

        const Vector3 right = get_basis().get_column( 0 ).normalized();
        const double theta_ratio = std::min( -p_dy / height, kMaximumMovementRatio );
        rotate_camera_around( pivot, right,
                              kTiltVerticalSign * rotate_rate * theta_ratio * math::kPi );

        enforce_camera_above_ellipsoid();
    }

    // ---- wheel zoom ----

    void GlobeCameraController::update_zoom_easing( const double p_delta )
    {
        if ( !wheel_animating_ || !inertia_enabled_ )
        {
            return;
        }

        // The reference integrates in log-distance so zoom feels linear across five orders of
        // magnitude, and the process delta time plays the role of its clamped frame time.
        const double dt = std::min( p_delta, 0.05 );
        const double damping = std::max( zoom_inertia_damping_, 0.1 );
        wheel_log_distance_ += wheel_log_velocity_ * dt * zoom_speed_scale_;
        wheel_log_distance_ = std::max( wheel_log_distance_, std::log( min_distance() ) );
        wheel_log_velocity_ *= std::exp( -damping * dt );

        const double distance = std::exp( wheel_log_distance_ );
        const Vector3 pivot = resolve_pivot();
        set_position( pivot + wheel_radial_direction_ * static_cast<float>( distance ) );
        sync_distance_from_camera();

        if ( std::abs( wheel_log_velocity_ ) < kStopFlickLogTravel * damping )
        {
            wheel_animating_ = false;
        }
    }

    void GlobeCameraController::update_drag_inertia( const double p_delta )
    {
        (void)p_delta;
        if ( !inertia_enabled_ || left_dragging_ || mouse_up_time_ == 0.0 )
        {
            return;
        }

        const double now = Time::get_singleton()->get_ticks_msec() / 1000.0;
        const double click_duration = ( mouse_up_time_ - mouse_down_time_ );
        const double since_release = now - mouse_up_time_;
        if ( click_duration > kInertiaMaxClickTime || since_release < 0.0 )
        {
            return;
        }

        // A hard stop on top of the exponential tail. The tail alone would need
        // ln(1/0.001) x coefficient seconds to reach the old threshold - over six seconds at
        // the reference's 0.9 s coefficient, which reads as the globe refusing to stop.
        if ( drag_inertia_max_time_ > 0.0 && since_release > drag_inertia_max_time_ )
        {
            mouse_up_time_ = 0.0;
            return;
        }

        const double decay = inertia_decay( since_release, drag_inertia_coefficient_ );
        if ( decay < kInertiaStopFactor )
        {
            mouse_up_time_ = 0.0;
            return;
        }

        const Vector3 pivot = resolve_pivot();
        const double distance = ( get_position() - pivot ).length();
        const double rotate_rate = clampd( distance / math::kWgs84MeanRadius - 1.0, 1.0 / 5000.0, 1.77 ) *
                                   rotate_speed_scale_;

        godot::Viewport *viewport = get_viewport();
        const double width =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.x ) : 1920.0;
        const double height =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.y ) : 1080.0;

        const double phi_ratio = std::min( -last_drag_dx_ / width, kMaximumMovementRatio );
        const Vector3 up = get_basis().get_column( 1 ).normalized();
        rotate_camera_around( pivot, up,
                              decay * kHorizontalSign * rotate_rate * phi_ratio * 2.0 * math::kPi );

        const Vector3 relative = get_position() - pivot;
        const double current_el = std::asin(
            clampd( static_cast<double>( relative.y ) /
                        std::max( static_cast<double>( relative.length() ), 1e-9 ),
                    -1.0, 1.0 ) );
        const double theta_ratio = std::min( -last_drag_dy_ / height, kMaximumMovementRatio );
        double delta_theta = decay * kVerticalSign * rotate_rate * theta_ratio * math::kPi;
        delta_theta = clampd( current_el + delta_theta, -kElLimit, kElLimit ) - current_el;
        const Vector3 right = get_basis().get_column( 0 ).normalized();
        rotate_camera_around( pivot, right, delta_theta );
    }

    // ---- pose API ----

    void GlobeCameraController::set_camera_pose( const Vector3 &p_position,
                                                 const Vector3 &p_direction,
                                                 const Vector3 &p_up )
    {
        Vector3 direction = p_direction;
        if ( direction.length_squared() < 1e-12f )
        {
            return;
        }
        direction = direction.normalized();

        Vector3 up = p_up;
        up = up - direction * up.dot( direction );
        if ( up.length_squared() < 1e-12f )
        {
            return;
        }
        up = up.normalized();

        set_position( p_position );

        // Godot cameras look down -Z, and the basis columns are (right, up, back).
        const Vector3 back = -direction;
        const Vector3 right = up.cross( back ).normalized();
        set_basis( Basis( right, up, back ) );

        sync_distance_from_camera();
        wheel_animating_ = false;
        wheel_log_velocity_ = 0.0;
    }

    Vector3 GlobeCameraController::get_camera_direction() const
    {
        // Forward is -Z in the camera's own basis.
        return -get_basis().get_column( 2 ).normalized();
    }

    Vector3 GlobeCameraController::get_camera_up() const
    {
        return get_basis().get_column( 1 ).normalized();
    }

    void GlobeCameraController::orbit_to( const double p_longitude_degrees,
                                          const double p_latitude_degrees, const double p_distance )
    {
        const double longitude = p_longitude_degrees * ( math::kPi / 180.0 );
        const double latitude = p_latitude_degrees * ( math::kPi / 180.0 );

        // Position on the sphere of radius p_distance around the pivot, then look straight at
        // the pivot with the north pole as up.
        //
        // The direction comes from to_local(wgs84ToCartesian(lon, lat, 0)) - the same path the
        // globe mesh and every tile use to place themselves - rather than from
        // local_direction(). Those two disagreed, and measurement settled which one is right:
        // local_direction() put a camera asked for Philadelphia (75.6W 40N) at 145E 62N
        // (Siberia) while keeping the radius correct, and everything placed through to_local()
        // lands exactly where it should. Rather than trust a conversion that cannot be checked
        // by eye, reuse the one that already is.
        const GlobeFrame &globe_frame = frame();
        const Vector3 pivot = resolve_pivot();
        const math::Vec3 surface_local =
            globe_frame.to_local( math::wgs84ToCartesian( longitude, latitude, 0.0 ) );
        const math::Vec3 center_local = globe_frame.ellipsoid_center_local();
        const math::Vec3 direction_local =
            math::normalizeSafe( surface_local - center_local );
        const Vector3 surface_direction = frame_direction_to_parent(
            Vector3( static_cast<float>( direction_local.x ),
                     static_cast<float>( direction_local.y ),
                     static_cast<float>( direction_local.z ) ) );

        const Vector3 position = pivot + surface_direction * static_cast<float>( p_distance );
        set_position( position );

        // look_at takes a world-space target. `pivot` is expressed in this node's parent
        // space (that is the space resolve_ellipsoid_center works in), so convert through the
        // parent - calling to_global() on this node would treat pivot as camera-local and add
        // the camera's own position, which is wrong.
        const godot::Node3D *parent_3d = Object::cast_to<godot::Node3D>( get_parent() );
        const Vector3 world_pivot =
            parent_3d != nullptr ? parent_3d->to_global( pivot ) : pivot;

        // look_at needs an up vector that is not parallel to the view direction; at a pole the
        // default +Y up is degenerate, so fall back to +Z.
        Vector3 up( 0.0f, 1.0f, 0.0f );
        if ( std::abs( surface_direction.normalized().y ) > 0.99f )
        {
            up = Vector3( 0.0f, 0.0f, 1.0f );
        }
        look_at( world_pivot, up );
        distance_ = p_distance;
        fly_active_ = false;
    }

    void GlobeCameraController::fly_to( const double p_longitude_degrees,
                                       const double p_latitude_degrees, const double p_distance,
                                       const double p_seconds )
    {
        if ( p_seconds <= 0.0 )
        {
            orbit_to( p_longitude_degrees, p_latitude_degrees, p_distance );
            return;
        }

        const double longitude = p_longitude_degrees * ( math::kPi / 180.0 );
        const double latitude = p_latitude_degrees * ( math::kPi / 180.0 );
        // Same derivation as orbit_to: see the comment there for why not local_direction().
        const GlobeFrame &globe_frame = frame();
        const math::Vec3 surface_local =
            globe_frame.to_local( math::wgs84ToCartesian( longitude, latitude, 0.0 ) );
        const math::Vec3 direction_local =
            math::normalizeSafe( surface_local - globe_frame.ellipsoid_center_local() );

        const Vector3 pivot = resolve_pivot();
        const Vector3 offset = get_position() - pivot;
        const double current_distance = std::max( static_cast<double>( offset.length() ), 1.0 );

        fly_start_direction_ = offset / static_cast<float>( current_distance );
        fly_end_direction_ = frame_direction_to_parent(
            Vector3( static_cast<float>( direction_local.x ),
                     static_cast<float>( direction_local.y ),
                     static_cast<float>( direction_local.z ) ) );
        fly_start_distance_ = current_distance;
        // The floor is the *local* surface radius under the target, not min_distance(): the
        // latter is the polar radius times 1.01, which is 42 km of altitude everywhere on the
        // planet and would leave a 400 m dataset as a speck. enforce_camera_above_ellipsoid()
        // keeps the camera outside the ellipsoid wherever it ends up; this only has to stop
        // the flight from burying it.
        const double surface_distance =
            glm::length( math::geodeticToYUp( longitude, latitude, 0.0 ) );
        fly_end_distance_ = clampd( p_distance, surface_distance + kMinCameraHeight,
                                    max_distance() );
        fly_elapsed_ = 0.0;
        fly_duration_ = p_seconds;
        fly_active_ = true;

        // A flight owns the camera while it runs.
        wheel_animating_ = false;
        mouse_up_time_ = 0.0;
    }

    bool GlobeCameraController::is_flying() const
    {
        return fly_active_;
    }

    void GlobeCameraController::update_fly( const double p_delta )
    {
        if ( !fly_active_ )
        {
            return;
        }

        fly_elapsed_ += p_delta;
        const double t = clampd( fly_elapsed_ / std::max( fly_duration_, 1e-3 ), 0.0, 1.0 );
        // Smoothstep: zero velocity at both ends, so the flight does not start or stop with a
        // jerk the way a linear interpolation does.
        const double ease = t * t * ( 3.0 - 2.0 * t );

        const Vector3 direction =
            slerp_direction( fly_start_direction_, fly_end_direction_, ease );
        // Log-space distance: the same easing then reads as a constant zoom rate whether the
        // flight spans a hundred metres or ten thousand kilometres.
        const double distance = std::exp(
            std::log( fly_start_distance_ ) +
            ( std::log( fly_end_distance_ ) - std::log( fly_start_distance_ ) ) * ease );

        const Vector3 pivot = resolve_pivot();
        set_position( pivot + direction * static_cast<float>( distance ) );
        distance_ = distance;

        // Keep looking at the pivot; the up vector degenerates at the poles.
        const godot::Node3D *parent_3d = Object::cast_to<godot::Node3D>( get_parent() );
        const Vector3 world_pivot = parent_3d != nullptr ? parent_3d->to_global( pivot ) : pivot;
        Vector3 up( 0.0f, 1.0f, 0.0f );
        if ( std::abs( direction.y ) > 0.99f )
        {
            up = Vector3( 0.0f, 0.0f, 1.0f );
        }
        look_at( world_pivot, up );

        if ( t >= 1.0 )
        {
            fly_active_ = false;
        }
    }

    // ---- input ----

    void GlobeCameraController::_unhandled_input( const Ref<InputEvent> &p_event )
    {
        const InputEventMouseButton *button = Object::cast_to<InputEventMouseButton>( p_event.ptr() );
        const InputEventMouseMotion *motion_event =
            Object::cast_to<InputEventMouseMotion>( p_event.ptr() );
        // Only real interaction takes the camera back; a window that gains focus emits a
        // hover motion with a zero delta, and cancelling on that killed flights at random.
        if ( button != nullptr ||
             ( motion_event != nullptr && motion_event->get_relative().length_squared() > 0.0f ) )
        {
            fly_active_ = false;
        }

        if ( button != nullptr )
        {
            if ( button->get_button_index() == MOUSE_BUTTON_LEFT )
            {
                if ( button->is_pressed() )
                {
                    left_dragging_ = true;
                    last_mouse_position_ = button->get_position();
                    mouse_down_time_ = Time::get_singleton()->get_ticks_msec() / 1000.0;
                    wheel_animating_ = false;
                }
                else
                {
                    left_dragging_ = false;
                    mouse_up_time_ = Time::get_singleton()->get_ticks_msec() / 1000.0;
                }
            }
            else if ( button->get_button_index() == MOUSE_BUTTON_RIGHT )
            {
                if ( button->is_pressed() )
                {
                    right_dragging_ = true;
                    last_mouse_position_ = button->get_position();
                    wheel_animating_ = false;
                }
                else
                {
                    right_dragging_ = false;
                }
            }
            else if ( button->is_pressed() &&
                      ( button->get_button_index() == MOUSE_BUTTON_WHEEL_UP ||
                        button->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN ) )
            {
                const Vector3 pivot = resolve_pivot();
                const Vector3 radial = ( get_position() - pivot ).normalized();

                if ( !inertia_enabled_ )
                {
                    const double step = button->get_button_index() == MOUSE_BUTTON_WHEEL_UP
                                            ? 1.0
                                            : -1.0;
                    const double distance = ( get_position() - pivot ).length();
                    const double next =
                        clampd( distance * std::exp( -step * 0.15 * zoom_speed_scale_ ),
                                min_distance(), max_distance() );
                    set_position( pivot + radial * static_cast<float>( next ) );
                    sync_distance_from_camera();
                    return;
                }

                const double current = ( get_position() - pivot ).length();
                if ( !wheel_animating_ )
                {
                    wheel_log_distance_ = std::log( std::max( current, min_distance() ) );
                    wheel_log_velocity_ = 0.0;
                    wheel_animating_ = true;
                }
                wheel_radial_direction_ = radial;

                const double direction = button->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? -1.0 : 1.0;
                // Scaling by the damping is what keeps the travelled distance independent of
                // it: the integrator divides velocity back out over the exponential tail.
                const double damping = std::max( zoom_inertia_damping_, 0.1 );
                const double max_velocity = kMaxFlickLogTravel * damping;
                wheel_log_velocity_ += direction * kLogDistancePerNotch * damping;
                wheel_log_velocity_ =
                    clampd( wheel_log_velocity_, -max_velocity, max_velocity );
                wheel_last_time_ = Time::get_singleton()->get_ticks_msec() / 1000.0;
            }
            return;
        }

        const InputEventMouseMotion *motion = Object::cast_to<InputEventMouseMotion>( p_event.ptr() );
        if ( motion == nullptr )
        {
            return;
        }

        const Vector2 position = motion->get_position();
        const double dx = position.x - last_mouse_position_.x;
        const double dy = position.y - last_mouse_position_.y;
        last_mouse_position_ = position;

        if ( left_dragging_ )
        {
            last_drag_dx_ = dx;
            last_drag_dy_ = dy;
            apply_orbit_drag( dx, dy );
        }
        else if ( right_dragging_ )
        {
            apply_tilt_drag( dx, dy );
        }
    }

} // namespace tiles3d
