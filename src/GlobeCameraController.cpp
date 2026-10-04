// SPDX-License-Identifier: Unlicense

#include "GlobeCameraController.h"

#include "Georeference3D.h"
#include "Globe3D.h"
#include "GlobeTileLayer.h"
#include "Tileset3D.h"

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
        /// The ease stops once the remaining velocity is worth this little travel. It has to be
        /// well under kLogDistancePerNotch: at 0.15 (the reference's value) the *stop threshold*
        /// was 0.15 x damping = 0.9 against an impulse of 0.2 x damping = 1.2, so the integrator
        /// quit after two or three frames and one notch delivered 0.061 of the 0.2 it promises -
        /// and, because the number of frames that fit in a time constant is 1/dt, that shortfall
        /// moved with the frame rate (0.055 at 60 fps, 0.073 at 30). Measured before: ratio 0.941
        /// per notch instead of 0.819.
        constexpr double kStopFlickLogTravel = 0.02;

        /// The wheel zoom bottoms out this far from the screen-centre ground focus (the
        /// map-cesium wrapper's WHEEL_MIN_DIST, its comment names the goal: 防止穿地).
        /// Distance measured to a point ON the surface never dips below it, and zoom-in
        /// is refused outright below this altitude - with the ellipsoid clamp that is
        /// three agreeing nets under the camera.
        constexpr double kWheelMinFocusDistance = 10.0;

        /// Ceiling on the orbit transfer function, in units of "camera altitude / planet radius".
        /// The reference uses the same 1.77 as the top of its clamp; above it the drag stops
        /// speeding up, which is what keeps a far-out viewpoint from spinning the globe wildly.
        constexpr double kMaximumRotateRate = 1.77;

        /// The reference's MINIMUM_TRACKBALL_HEIGHT: above this altitude a tilt whose
        /// screen-centre ray misses the planet still pivots on the ray's closest approach to the
        /// ellipsoid centre, so the gesture never silently changes meaning with distance.
        constexpr double kMinimumTrackballHeight = 7500000.0;

        /// The most poleward latitude an ENU anchor may be moved to. East and north are both
        /// undefined at a pole, so the derived basis is degenerate - with the anchor at lat 90 a
        /// single frame ran for twenty seconds (Icospheres declares its centre there) instead of
        /// the usual millisecond. Refusing keeps the previous anchor, which is always a valid
        /// frame; the caller reports why it did not move.
        constexpr double kMaximumAnchorLatitude = 89.5;

        constexpr double kInertiaMaxClickTime = 0.4;
        constexpr double kInertiaStopFactor = 0.001;

        /// How much of an atmosphere shell the far plane has to cover past the ellipsoid.
        /// Globe3D defaults to 1.06 radii, so 1.2 has room to spare.
        constexpr double kAtmosphereHeadroom = 1.2;

        /// Below this the camera is already sitting on the frame origin and a shift would be a
        /// no-op. Without it the threshold-0 ("every frame") mode would rebuild the globe
        /// surface sixty times a second for a parked camera, moving the origin by nothing.
        constexpr double kOriginShiftEpsilon = 1e-3;

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

        ClassDB::bind_method( D_METHOD( "set_origin_shift_enabled", "p_enabled" ),
                              &GlobeCameraController::set_origin_shift_enabled );
        ClassDB::bind_method( D_METHOD( "get_origin_shift_enabled" ),
                              &GlobeCameraController::get_origin_shift_enabled );
        ClassDB::add_property( "GlobeCameraController",
                               PropertyInfo( Variant::BOOL, "origin_shift_enabled" ),
                               "set_origin_shift_enabled", "get_origin_shift_enabled" );

        ClassDB::bind_method( D_METHOD( "set_origin_shift_threshold", "p_metres" ),
                              &GlobeCameraController::set_origin_shift_threshold );
        ClassDB::bind_method( D_METHOD( "get_origin_shift_threshold" ),
                              &GlobeCameraController::get_origin_shift_threshold );
        ClassDB::add_property( "GlobeCameraController",
                               PropertyInfo( Variant::FLOAT, "origin_shift_threshold" ),
                               "set_origin_shift_threshold", "get_origin_shift_threshold" );

        ClassDB::bind_method( D_METHOD( "shift_origin_now" ),
                              &GlobeCameraController::shift_origin_now );

        ClassDB::bind_method( D_METHOD( "reanchor_preserving_view", "p_longitude_degrees",
                                        "p_latitude_degrees" ),
                              &GlobeCameraController::reanchor_preserving_view );
        ClassDB::bind_method( D_METHOD( "get_origin_shift_count" ),
                              &GlobeCameraController::get_origin_shift_count );
        ClassDB::bind_method( D_METHOD( "get_origin_shift_milliseconds" ),
                              &GlobeCameraController::get_origin_shift_milliseconds );
        ClassDB::bind_method( D_METHOD( "get_origin_shift_distance" ),
                              &GlobeCameraController::get_origin_shift_distance );
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
                // After every pose writer, so the origin moves from - and the camera is put
                // back at - the pose this frame will actually be drawn with. A shift never
                // changes that pose, only the numbers that express it.
                update_origin_shift();
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

        // far tracks the altitude too, and - unlike the old version - it is allowed to COME
        // BACK DOWN. A grow-only far is a one-way ratchet: a scene authored with far = 2e8
        // (the "whole planet and then some" number) keeps it forever, and at 2e8 the engine's
        // light culler cannot build its frustum in float32, which is the
        //   create_frustum_points: Condition "!res" is true
        // spam - hundreds of lines per second. The plane has to be sized for what is actually
        // in front of the camera, not for the worst case that ever existed.
        //
        // Everything past the planet's silhouette is behind the planet, so "what is actually
        // in front" is: the distance to the limb, plus the air the grazing ray crosses on its
        // way in and out of the shell (that last stretch is the horizon glow, and it is real
        // pixels, so it cannot be clipped).
        const Vector3 pivot = resolve_pivot();
        const double centre_distance = static_cast<double>( ( get_position() - pivot ).length() );
        const double radius = math::kWgs84SemiMajorAxis;
        const double shell_radius = radius * kAtmosphereHeadroom;
        const auto squared = []( const double v ) { return v * v; };
        // Distance from the camera to the planet's silhouette (the tangent point). Zero for a
        // camera on the ground.
        const double limb =
            std::sqrt( std::max( squared( centre_distance ) - squared( radius ), 0.0 ) );
        // How much further the same grazing ray runs before it leaves the shell: it is at its
        // closest at the limb, so the exit is one shell thickness beyond it (~4200 km at WGS84
        // with this headroom). That stretch is the horizon glow, and it is real pixels.
        const double air =
            std::sqrt( std::max( squared( shell_radius ) - squared( radius ), 0.0 ) );
        const double required_far = clampd( limb + air + radius * 0.01, 1.0e4, 1.0e8 );
        set_far( static_cast<float>( required_far ) );
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

    // ---- floating origin ----

    void GlobeCameraController::set_origin_shift_enabled( const bool p_enabled )
    {
        origin_shift_enabled_ = p_enabled;
    }

    bool GlobeCameraController::get_origin_shift_enabled() const
    {
        return origin_shift_enabled_;
    }

    void GlobeCameraController::set_origin_shift_threshold( const double p_metres )
    {
        origin_shift_threshold_ = p_metres < 0.0 ? 0.0 : p_metres;
    }

    double GlobeCameraController::get_origin_shift_threshold() const
    {
        return origin_shift_threshold_;
    }

    int GlobeCameraController::get_origin_shift_count() const
    {
        return origin_shift_count_;
    }

    double GlobeCameraController::get_origin_shift_milliseconds() const
    {
        return origin_shift_milliseconds_;
    }

    double GlobeCameraController::get_origin_shift_distance() const
    {
        return origin_shift_distance_;
    }

    Vector3 GlobeCameraController::delta_in_parent_space( const godot::Node3D *p_carrier,
                                                          const godot::Node3D *p_node,
                                                          const math::Vec3 &p_local_delta ) const
    {
        const godot::Node3D *parent_3d =
            p_node != nullptr ? Object::cast_to<godot::Node3D>( p_node->get_parent() ) : nullptr;
        if ( p_carrier == nullptr || parent_3d == nullptr )
        {
            return Vector3();
        }

        // A delta is a vector, so carry two points through the same transforms and keep the
        // difference - the idiom frame_direction_to_parent() already uses, and for the same
        // reason: one conversion path to trust rather than a hand-composed basis.
        const Vector3 origin_world = p_carrier->to_global( Vector3() );
        const Vector3 tip_world = p_carrier->to_global(
            Vector3( static_cast<float>( p_local_delta.x ), static_cast<float>( p_local_delta.y ),
                     static_cast<float>( p_local_delta.z ) ) );
        return parent_3d->to_local( tip_world ) - parent_3d->to_local( origin_world );
    }

    void GlobeCameraController::rebase_content( const math::Vec3 &p_local_delta )
    {
        // Walking the carrier's children rather than keeping a registry is deliberate: the
        // list is short, a shift is rare, and a registry is one more thing to forget to update
        // when a new content node type lands. Everything that authors geometry in the frame's
        // local space hangs off the Georeference3D - that is the whole point of the shared
        // frame (docs/GLOBE_PLAN.md 3).
        godot::Node3D *carrier = const_cast<godot::Node3D *>( resolve_frame_node() );
        if ( carrier == nullptr )
        {
            return;
        }

        for ( int index = 0; index < carrier->get_child_count(); ++index )
        {
            godot::Node *child = carrier->get_child( index );

            if ( GlobeTileLayer *layer = Object::cast_to<GlobeTileLayer>( child ); layer != nullptr )
            {
                // Re-derives each tile's placement from its cached ECEF centre: one transform
                // write per live tile, no vertex work.
                layer->rebase();
                continue;
            }
            if ( Globe3D *globe = Object::cast_to<Globe3D>( child ); globe != nullptr )
            {
                // The expensive one: the surface is baked in local space, so it is rebuilt.
                globe->rebase();
                continue;
            }
            if ( Tileset3D *tileset = Object::cast_to<Tileset3D>( child ); tileset != nullptr )
            {
                // Only the already-attached content needs this; the next traversal re-derives
                // every matrix from the moved frame on its own.
                tileset->rebase( delta_in_parent_space( carrier, tileset, p_local_delta ) );
                continue;
            }
        }
    }

    bool GlobeCameraController::shift_origin_now()
    {
        // Only a Georeference3D can carry a moved origin. The fallback frame *is* Y-up ECEF, so
        // "move the origin" would mean "move the Earth's centre" - and a scene that falls back
        // to it has no dataset in it to jitter anyway.
        godot::Node3D *carrier = const_cast<godot::Node3D *>( resolve_frame_node() );
        Georeference3D *reference = Object::cast_to<Georeference3D>( carrier );
        if ( reference == nullptr )
        {
            return false;
        }

        godot::Time *clock = Time::get_singleton();
        const double started_usec =
            clock != nullptr ? static_cast<double>( clock->get_ticks_usec() ) : 0.0;

        // 1. Where the camera is, taken all the way out to ECEF through the frame that is about
        //    to be replaced. This is the invariant the whole operation preserves: the *numbers*
        //    describing the camera change, the camera's place on the planet does not. Going via
        //    ECEF rather than translating by a running delta is also what stops a long run of
        //    shifts from accumulating a drift.
        const GlobeFrame old_frame = frame();
        const Vector3 camera_local = frame_point_from_parent( get_position() );
        const math::Vec3 camera_ecef = old_frame.to_ecef_z_up(
            math::Vec3( static_cast<double>( camera_local.x ),
                        static_cast<double>( camera_local.y ),
                        static_cast<double>( camera_local.z ) ) );
        if ( glm::length( camera_ecef ) < 1.0 )
        {
            // The camera is at the Earth's centre: no usable origin there, and the georeference
            // refuses a degenerate one anyway.
            return false;
        }

        const Vector3 declared_origin = reference->get_frame_origin_ecef();
        const math::Vec3 old_origin( static_cast<double>( declared_origin.x ),
                                     static_cast<double>( declared_origin.y ),
                                     static_cast<double>( declared_origin.z ) );

        // 2. Move the origin onto the camera. The frame's basis stays frozen on the *declared*
        //    anchor (Georeference3D::rebase_origin_ecef), so this is a pure translation and
        //    every orientation in the scene survives untouched - the camera's included. That is
        //    not a convenience: re-deriving the ENU basis at the camera instead would rotate
        //    the world by the meridian convergence between the two points, 0.009 deg per km.
        reference->rebase_origin_ecef( Vector3( static_cast<float>( camera_ecef.x ),
                                               static_cast<float>( camera_ecef.y ),
                                               static_cast<float>( camera_ecef.z ) ) );

        // 3. Put the camera back where it was, re-expressed through the frame that just moved.
        //    Together with the content move below this is the whole mechanism: content and
        //    camera are translated by the same world vector, so the rendered image is identical
        //    and only the magnitudes changed.
        const GlobeFrame new_frame = frame();
        const math::Vec3 new_local = new_frame.to_local( camera_ecef );
        set_position( frame_point_to_parent( Vector3( static_cast<float>( new_local.x ),
                                                      static_cast<float>( new_local.y ),
                                                      static_cast<float>( new_local.z ) ) ) );

        // 4. Re-place the content. `to_local(old_origin)` under the new frame is exactly how far
        //    local coordinates moved, which is the delta every consumer needs - and the one
        //    number worth reporting, since it is the magnitude that just stopped being quantised.
        const math::Vec3 local_delta = new_frame.to_local( old_origin );
        rebase_content( local_delta );

        origin_shift_distance_ = glm::length( local_delta );
        origin_shift_count_ += 1;
        if ( clock != nullptr )
        {
            origin_shift_milliseconds_ =
                ( static_cast<double>( clock->get_ticks_usec() ) - started_usec ) / 1000.0;
        }
        return true;
    }

    bool GlobeCameraController::reanchor_preserving_view( const double p_longitude_degrees,
                                                          const double p_latitude_degrees )
    {
        godot::Node3D *carrier = const_cast<godot::Node3D *>( resolve_frame_node() );
        Georeference3D *reference = Object::cast_to<Georeference3D>( carrier );
        if ( reference == nullptr )
        {
            return false;
        }
        const godot::Ref<godot::Resource> authority = reference->get_origin_authority();
        if ( authority.is_null() || !authority->has_method( "set_longitude" ) )
        {
            return false;
        }

        // A pole has no ENU frame. See kMaximumAnchorLatitude: the anchor is left where it was
        // rather than moving it somewhere degenerate.
        if ( std::abs( p_latitude_degrees ) > kMaximumAnchorLatitude )
        {
            return false;
        }

        const auto to_vec3 = []( const Vector3 &p_v ) {
            return math::Vec3( static_cast<double>( p_v.x ), static_cast<double>( p_v.y ),
                               static_cast<double>( p_v.z ) );
        };
        const auto to_godot = []( const math::Vec3 &p_v ) {
            return Vector3( static_cast<float>( p_v.x ), static_cast<float>( p_v.y ),
                            static_cast<float>( p_v.z ) );
        };

        // 1. Where the camera is and where it looks, all the way out to ECEF in doubles. That is
        //    the only representation of the view that survives the frame being replaced: a
        //    parent-space position or a world-space direction belongs to one particular anchor.
        const GlobeFrame old_frame = frame();
        const math::Vec3 camera_ecef =
            old_frame.to_ecef_z_up( to_vec3( frame_point_from_parent( get_position() ) ) );
        // A direction rides along as a point one unit further out, so the translation cancels.
        const math::Vec3 forward_ecef =
            old_frame.to_ecef_z_up( to_vec3( frame_point_from_parent(
                                        get_position() + get_camera_direction() ) ) ) -
            camera_ecef;
        const math::Vec3 up_ecef =
            old_frame.to_ecef_z_up( to_vec3( frame_point_from_parent(
                                        get_position() + get_camera_up() ) ) ) -
            camera_ecef;

        // 2. Move the anchor. The authority's changed signal refreshes the frame, which is what
        //    re-places every content node - the jump this function exists to hide.
        authority->set( "longitude", p_longitude_degrees );
        authority->set( "latitude", p_latitude_degrees );
        authority->set( "height", 0.0 );

        // 3. Put the camera back where it was, re-expressed through the frame that just moved.
        const GlobeFrame new_frame = frame();
        const math::Vec3 new_local = new_frame.to_local( camera_ecef );
        const Vector3 position = frame_point_to_parent( to_godot( new_local ) );
        const Vector3 direction = frame_direction_to_parent(
            to_godot( new_frame.to_local( camera_ecef + forward_ecef ) - new_local ) );
        const Vector3 up = frame_direction_to_parent(
            to_godot( new_frame.to_local( camera_ecef + up_ecef ) - new_local ) );
        set_camera_pose( position, direction, up );

        // This is a rebase by another name as far as the content is concerned.
        origin_shift_count_ += 1;
        return true;
    }

    void GlobeCameraController::update_origin_shift()
    {
        // How far the camera has strayed from the frame's origin, measured in the frame's own
        // local space - which is precisely the space whose coordinates are about to be rounded
        // to float32. This is the number the threshold is compared against, and the one a HUD
        // should show, because it *is* the quantisation scale divided by 2^-24.
        //
        // Computed even while the feature is off: with the shift disabled this readout is the
        // measurement that says how much precision is being left on the table.
        const Vector3 camera_local = frame_point_from_parent( get_position() );
        origin_shift_distance_ = static_cast<double>( camera_local.length() );

        if ( !origin_shift_enabled_ )
        {
            return;
        }

        // With the camera already on the origin there is nothing to regain. This is what keeps
        // threshold 0 ("every frame") from rebuilding the globe surface sixty times a second
        // for a parked camera.
        if ( origin_shift_distance_ < kOriginShiftEpsilon )
        {
            return;
        }

        // Strictly less-than, so a threshold of 0 shifts on every frame that moves at all -
        // the strictest form of the technique, and the most expensive.
        if ( origin_shift_distance_ < origin_shift_threshold_ )
        {
            return;
        }

        // Deliberately not gated on is_flying(): a flight across the planet is exactly when the
        // coordinates need keeping in check. Nothing in the flight state depends on the origin -
        // the stored directions are parent-space unit vectors and the distances are scalars -
        // and update_fly() re-derives the camera position from the pivot every frame, so a
        // shift mid-flight is invisible to it.
        shift_origin_now();
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

    double GlobeCameraController::clamp_polar_pitch( const Vector3 &p_pivot,
                                                   const double p_delta ) const
    {
        (void)p_pivot;
        // The elevation is the camera's GEOCENTRIC LATITUDE, read in the globe frame - Y-up ECEF,
        // where Y really is the polar axis - because that is the frame the reference reads it in:
        //   camera.position.y / camera.position.length()      (QuadtreeGlobe.ts:437)
        //
        // Reading it in the camera's parent frame instead is what made the left drag jump. The
        // parent frame's Y is the local up at the anchor, so a camera anywhere near the anchor
        // measures relative.y / |relative| ~ 1 whatever its latitude, asin() saturates at exactly
        // pi/2, and kElLimit (pi/2 - 0.01) then reports the camera as 0.01 rad outside a limit it
        // is nowhere near. The reference's clamp(currentEl + delta) - currentEl turns that reading
        // into a -0.01 rad pitch - 0.01 x 6371 km = 63.7 km of ground - for every drag event, and
        // pins the elevation to the limit, so the camera also cannot keep rotating. (In JS the same
        // expression is harmless: the ratio is a float64 that never quite reaches 1.)
        const Vector3 camera_local = frame_point_from_parent( get_position() );
        const math::Vec3 camera_ecef = frame().camera_ecef_y_up(
            math::Vec3( static_cast<double>( camera_local.x ),
                        static_cast<double>( camera_local.y ),
                        static_cast<double>( camera_local.z ) ) );
        const double length = glm::length( camera_ecef );
        if ( length < 1e-9 )
        {
            return p_delta;
        }
        const double current_el = std::asin( clampd( camera_ecef.y / length, -1.0, 1.0 ) );

        // An elevation already outside the limit is left alone rather than dragged back to it:
        // that only happens over the frame's pole, and the clamp is meant to stop the drag from
        // TAKING the camera past the limit, not to answer every event with the margin itself.
        if ( std::abs( current_el ) >= kElLimit )
        {
            return p_delta;
        }
        return clampd( current_el + p_delta, -kElLimit, kElLimit ) - current_el;
    }

    void GlobeCameraController::rotate_camera_around( const Vector3 &p_pivot,
                                                      const Vector3 &p_axis, const double p_angle )
    {
        // Position: rotate the offset from the pivot, then add it back.
        //
        // The arithmetic is DOUBLED on purpose, and it is not a micro-optimisation. The left
        // drag's pivot is the planet centre, so the offset is ~6.4e6 m and the result is
        // `pivot + rotated_offset` - two 6.4e6 m values cancelling down to a camera position of a
        // few hundred metres. In float32 that cancellation leaves the 0.38 m tick of a 6.4e6 m
        // value, so a ground-level drag advances in half-metre steps however small the rotation
        // is. The reference does not have the problem (its numbers are float64); a port onto a
        // float32 scene graph has to ask for the precision explicitly at exactly this line.
        const godot::Vector3 offset = get_position() - p_pivot;
        const math::Vec3 offset_d( static_cast<double>( offset.x ), static_cast<double>( offset.y ),
                                   static_cast<double>( offset.z ) );
        const math::Vec3 pivot_d( static_cast<double>( p_pivot.x ),
                                  static_cast<double>( p_pivot.y ),
                                  static_cast<double>( p_pivot.z ) );
        const math::Vec3 axis_d( static_cast<double>( p_axis.x ), static_cast<double>( p_axis.y ),
                                 static_cast<double>( p_axis.z ) );
        const math::Vec3 unit_axis = glm::length( axis_d ) > 1e-12
                                         ? glm::normalize( axis_d )
                                         : math::Vec3( 0.0, 1.0, 0.0 );
        const double cos_angle = std::cos( p_angle );
        const double sin_angle = std::sin( p_angle );
        // Rodrigues: v cos + (a x v) sin + a (a . v)(1 - cos).
        const math::Vec3 rotated = offset_d * cos_angle +
                                   glm::cross( unit_axis, offset_d ) * sin_angle +
                                   unit_axis * ( glm::dot( unit_axis, offset_d ) * ( 1.0 - cos_angle ) );
        const math::Vec3 moved = pivot_d + rotated;
        set_position( Vector3( static_cast<float>( moved.x ), static_cast<float>( moved.y ),
                               static_cast<float>( moved.z ) ) );

        // Orientation: premultiply, exactly like the reference quaternion.premultiply.
        const Quaternion rotation( p_axis.normalized(), static_cast<float>( p_angle ) );
        set_basis( Basis( rotation ) * get_basis() );

        sync_distance_from_camera();
    }

    void GlobeCameraController::apply_orbit_drag( const double p_dx, const double p_dy )
    {
        // Pivot: the planet centre, exactly as the reference does
        // (initEvents: rotateCameraAround(new Vector3(0, 0, 0), up, deltaPhi)). This is the
        // "spin the globe" gesture, and it behaves the same whether the cursor is over the globe
        // or over empty space - dragging in space turns the Earth rather than swinging it around
        // whatever happens to be under the pointer. The float32 cost of a 6.4e6 m pivot is paid
        // inside rotate_camera_around, which does that arithmetic in double.
        const Vector3 pivot = resolve_pivot();
        const double distance = static_cast<double>( ( get_position() - pivot ).length() );
        const double altitude = camera_height_above_ellipsoid();

        // Cursor-anchored transfer function.
        //
        // A rotation of dphi about the pivot slides the GROUND under the camera by
        // r_ground * dphi, and the drag should slide it by the cursor's own screen fraction of the
        // visible ground width, (dx/W) * 2*h*tan(fov/2)*aspect. So
        //   dphi = (dx/W) * 2*h*tan(fov/2)*aspect / r_ground
        // and the whole tuning problem is "what is r_ground". The reference hard-codes
        // r_ground = R and approximates the viewport factor with 2*pi:
        //   clamp(distance/MEAN_R - 1, 1/5000, 1.77) * ratio * 2*pi
        // which is 2.1x the cursor at altitude, 7.8x at 49 m (distance - MEAN_R is not the
        // altitude: at 34 N the geocentric surface radius sits 335 m above the mean) and 305x at
        // 1 m, where the 1/5000 floor freezes the rate at a 5 km "virtual altitude" and stops
        // tracking the camera at all. r_ground = R_local = distance - altitude and the real
        // viewport angle are what make the ground track the cursor.
        const double ground_radius = std::max( distance - altitude, 1.0 );
        const double half_tan =
            std::tan( static_cast<double>( get_fov() ) * ( 0.5 * math::kPi / 180.0 ) );
        const double rate =
            clampd( altitude / ground_radius, 0.0, kMaximumRotateRate ) * rotate_speed_scale_;

        godot::Viewport *viewport = get_viewport();
        const double width =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.x ) : 1920.0;
        const double height =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.y ) : 1080.0;

        // Yaw about the camera's current up, then pitch about the resulting right vector,
        // with the polar angle clamped so the camera cannot flip over the pole.
        const Vector3 up = get_basis().get_column( 1 ).normalized();
        const double phi_ratio = std::min( -p_dx / width, kMaximumMovementRatio );
        const double delta_phi =
            kHorizontalSign * rate * ( width / height ) * half_tan * 2.0 * phi_ratio;
        rotate_camera_around( pivot, up, delta_phi );

        const double theta_ratio = std::min( -p_dy / height, kMaximumMovementRatio );
        const double delta_theta = clamp_polar_pitch(
            pivot, kVerticalSign * rate * half_tan * 2.0 * theta_ratio );

        const Vector3 right = get_basis().get_column( 0 ).normalized();
        rotate_camera_around( pivot, right, delta_theta );

        // The rotation preserves the distance to the CENTRE, but the surface radius
        // changes with latitude on an oblate ellipsoid (~21 km pole-to-equator): a
        // ground-level drag toward the equator drives the camera kilometres underground
        // while every centre-distance invariant stays satisfied. The reference calls
        // enforceCameraAboveEllipsoid() after the same drag (QuadtreeGlobe.ts:478).
        enforce_camera_above_ellipsoid();
    }

    void GlobeCameraController::apply_tilt_drag( const double p_dx, const double p_dy )
    {
        // Right-drag tilts about the ellipsoid point picked at mousedown (the reference's
        // middlePivot). A missed pick downgraded this drag to a free look.
        if ( !tilt_pivot_valid_ )
        {
            apply_look_drag( p_dx, p_dy );
            return;
        }

        // The pivot in the camera's parent space, re-derived per event: the frame can be
        // rebased (origin shift) between the press and this drag, and the ECEF doubles
        // survive that unchanged while a cached parent-space point would not.
        const GlobeFrame &globe_frame = frame();
        const math::Vec3 pivot_ecef_z_up(
            tilt_pivot_ecef_.x, -tilt_pivot_ecef_.z, tilt_pivot_ecef_.y ); // Y-up -> Z-up
        const math::Vec3 pivot_local = globe_frame.to_local( pivot_ecef_z_up );
        const Vector3 pivot = frame_point_to_parent(
            Vector3( static_cast<float>( pivot_local.x ), static_cast<float>( pivot_local.y ),
                     static_cast<float>( pivot_local.z ) ) );

        const double rho = static_cast<double>( ( get_position() - pivot ).length() );
        // The reference's tilt rate, verbatim (QuadtreeGlobe.ts:470):
        //   clamp(rho - 1.0, 1/5000, 1.77)
        // rho is in metres, so the clamp is at its 1.77 ceiling for anything further than 2.77 m
        // away - which is to say always. No altitude scaling is applied on top: the port used to
        // multiply in an invented altitude_motion_scale(), and this gesture's speed is not
        // something to invent.
        const double rotate_rate =
            clampd( rho - 1.0, 1.0 / 5000.0, 1.77 ) * rotate_speed_scale_;

        godot::Viewport *viewport = get_viewport();
        const double width =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.x ) : 1920.0;
        const double height =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.y ) : 1080.0;

        // Yaw about the pivot's geodetic surface normal (not the radial direction: on an
        // oblate ellipsoid they differ by up to ~0.19 deg, and the geodetic normal is what
        // keeps the horizon level while tilting). The surface normal of
        // x^2/a^2 + y^2/b^2 + z^2/a^2 = 1 in Y-up scaled space is (x/a^2, y/b^2, z/a^2) - Y is
        // the polar axis here, exactly as in ellipsoid_offset().
        const double a = math::kWgs84SemiMajorAxis;
        const double b = math::kWgs84SemiMinorAxis;
        const glm::dvec3 normal_y_up =
            glm::normalize( glm::dvec3( tilt_pivot_ecef_.x / ( a * a ),
                                        tilt_pivot_ecef_.y / ( b * b ),
                                        tilt_pivot_ecef_.z / ( a * a ) ) );
        // Y-up -> Z-up, then out to the parent space through the frame's linear part.
        const math::Vec3 normal_z_up( normal_y_up.x, -normal_y_up.z, normal_y_up.y );
        const math::Vec3 normal_local = globe_frame.rotate_local( normal_z_up );
        const Vector3 yaw_axis = frame_direction_to_parent(
            Vector3( static_cast<float>( normal_local.x ), static_cast<float>( normal_local.y ),
                     static_cast<float>( normal_local.z ) ) )
                                         .normalized();

        const double phi_ratio = std::min( -p_dx / width, kMaximumMovementRatio );
        const double theta_ratio = std::min( -p_dy / height, kMaximumMovementRatio );

        // The one clamp this gesture should have: the ground.
        //
        // A tilt pivots on a point ON the surface, so a long enough drag swings the camera under
        // it. enforce_camera_above_ellipsoid() then pins the camera at 1 m, the distance to the
        // pivot collapses from 800 m to a metre, and any further drag spins the view about that
        // point instead of tilting it - which is what "it cannot keep rotating, the range is
        // clamped" looks like from the outside. Measured before this: dragging up 320 px left the
        // camera at 103 m and 640 px at 0.8 m, with the pivot off screen.
        //
        // So a step that would bury the camera is dropped rather than applied, and the gesture
        // stops against the surface. By construction this can only ever bind near the ground - from
        // altitude the same drag has thousands of metres to spend first - which is exactly where
        // the user expects a limit and where the reference, never closer than its 42 km MIN_DIST,
        // never has to think about it.
        const auto step_guarded = [this, &pivot]( const Vector3 &p_axis, const double p_angle ) {
            if ( p_angle == 0.0 )
            {
                return;
            }
            const Vector3 before_position = get_position();
            const Basis before_basis = get_basis();
            rotate_camera_around( pivot, p_axis, p_angle );
            if ( camera_height_above_ellipsoid() < kMinCameraHeight )
            {
                set_position( before_position );
                set_basis( before_basis );
                sync_distance_from_camera();
            }
        };

        step_guarded( yaw_axis, kTiltHorizontalSign * rotate_rate * phi_ratio * 2.0 * math::kPi );

        // Vertical: about the camera's OWN RIGHT vector (QuadtreeGlobe.ts:477):
        //   this.rotateCameraAround(center, this.getCameraRight(), deltaTheta);
        //
        // This is the line that decides the feel of the gesture. The port had been rotating about
        // the surface tangent cross(up, pivot -> camera) instead, which is what the HTML prototype
        // in cesium-middle-tilt-study.md §7.3 does - and that tangent is degenerate for a nadir
        // camera (up and pivot->camera are parallel, the most common pose here) and flips sign as
        // the view tilts, so the gesture read as inverted in some poses and correct in others. The
        // camera's right is frame-independent and has one sign.
        //
        // With TILT_VERTICAL_SIGN = +1 and dy < 0 for an upward drag the angle is positive, and a
        // positive rotation about the camera's right pitches the view up: the ground in front of
        // the camera moves down the screen and the sphere tips away, which is the reference's
        // behaviour and what the sign is chosen for.
        step_guarded( get_basis().get_column( 0 ).normalized(),
                      kTiltVerticalSign * rotate_rate * theta_ratio * math::kPi );

        enforce_camera_above_ellipsoid();
    }

    void GlobeCameraController::apply_look_drag( const double p_dx, const double p_dy )
    {
        // Free look for a tilt drag that started on empty space: the camera spins in
        // place, yaw about the geodetic normal at the camera (keeps the horizon level the
        // way the reference's applyLook3D does), pitch about the camera's own right.
        const EllipsoidOffset offset = ellipsoid_offset();
        const double a = math::kWgs84SemiMajorAxis;
        const double b = math::kWgs84SemiMinorAxis;
        const glm::dvec3 normal_y_up =
            offset.radius > 1e-6
                ? glm::normalize( glm::dvec3( offset.offset_y_up.x / ( a * a ),
                                              offset.offset_y_up.y / ( b * b ),
                                              offset.offset_y_up.z / ( a * a ) ) )
                : glm::dvec3( 0.0, 1.0, 0.0 );
        const GlobeFrame &globe_frame = frame();
        const math::Vec3 normal_z_up( normal_y_up.x, -normal_y_up.z, normal_y_up.y );
        const math::Vec3 normal_local = globe_frame.rotate_local( normal_z_up );
        const Vector3 yaw_axis = frame_direction_to_parent(
            Vector3( static_cast<float>( normal_local.x ), static_cast<float>( normal_local.y ),
                     static_cast<float>( normal_local.z ) ) )
                                         .normalized();

        godot::Viewport *viewport = get_viewport();
        const double width =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.x ) : 1920.0;
        const double height =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.y ) : 1080.0;

        // A look rotates the orientation only - the pivot is the camera itself, so
        // rotate_camera_around's position term degenerates to a no-op.
        const double rotate_rate = 0.5 * rotate_speed_scale_;
        const double phi_ratio = std::min( -p_dx / width, kMaximumMovementRatio );
        rotate_camera_around( get_position(), yaw_axis,
                              kTiltHorizontalSign * rotate_rate * phi_ratio * 2.0 * math::kPi );

        const Vector3 right = get_basis().get_column( 0 ).normalized();
        const double theta_ratio = std::min( -p_dy / height, kMaximumMovementRatio );
        rotate_camera_around( get_position(), right,
                              kTiltVerticalSign * rotate_rate * theta_ratio * math::kPi );
    }

    bool GlobeCameraController::screen_ray( const godot::Vector2 &p_screen,
                                            math::Vec3 &out_origin_ecef_y_up,
                                            math::Vec3 &out_direction_ecef_y_up ) const
    {
        const godot::Node3D *carrier = resolve_frame_node();
        if ( carrier == nullptr )
        {
            return false;
        }

        // Pick ray from the camera's own projection, world space. godot-cpp exposes the
        // normalised direction as project_ray_normal (GDScript's project_ray_direction);
        // normalised is fine - the scaled-space solve does not care about the ray's length.
        const Vector3 world_origin = project_ray_origin( p_screen );
        const Vector3 world_tip = world_origin + project_ray_normal( p_screen );

        // World -> frame local -> Z-up ECEF. The direction rides along as a point pair
        // difference so it never picks up a translation.
        const Vector3 origin_local = carrier->to_local( world_origin );
        const Vector3 tip_local = carrier->to_local( world_tip );
        const GlobeFrame &globe_frame = frame();
        const math::Vec3 origin_ecef = globe_frame.to_ecef_z_up(
            math::Vec3( static_cast<double>( origin_local.x ),
                        static_cast<double>( origin_local.y ),
                        static_cast<double>( origin_local.z ) ) );
        const math::Vec3 tip_ecef = globe_frame.to_ecef_z_up(
            math::Vec3( static_cast<double>( tip_local.x ), static_cast<double>( tip_local.y ),
                        static_cast<double>( tip_local.z ) ) );
        // Z-up -> Y-up (the same flip ellipsoid_offset uses), so the quadric is the
        // axis-aligned WGS84 ellipsoid.
        out_origin_ecef_y_up = math::Vec3( origin_ecef.x, origin_ecef.z, -origin_ecef.y );
        out_direction_ecef_y_up =
            math::Vec3( tip_ecef.x - origin_ecef.x, tip_ecef.z - origin_ecef.z,
                        -( tip_ecef.y - origin_ecef.y ) );
        return true;
    }

    bool GlobeCameraController::pick_ellipsoid_point( const godot::Vector2 &p_screen,
                                                      math::Vec3 &out_ecef_y_up ) const
    {
        math::Vec3 origin;
        math::Vec3 direction;
        if ( !screen_ray( p_screen, origin, direction ) )
        {
            return false;
        }

        // Unit-sphere intersection in scaled space: |o' + t*d'| = 1. The t is the same in
        // both spaces because scaling is linear, but d' is NOT a unit vector (its length
        // is the world-space ray length scaled by 1/R ~ 1.5e-7), so the quadratic has to
        // keep the full |d'|^2 coefficient - dividing it out was exactly the bug that made
        // every pick miss: with |d'| ~ 1e-7 the discriminant od^2 - |d'|^2*c collapsed to
        // -|d'|^2*c < 0 for any camera above the ellipsoid.
        //
        // The point is Y-UP (the permutation above), so its polar axis is Y and Y is the axis
        // that pairs with the semi-minor radius b - the pairing ellipsoid_offset() uses. This
        // used to pair Z with b (the Z-up ECEF convention) while the vector it was applied to had
        // already been flipped to Y-up, which inflated the scaled radius by up to 1.2e-3 - 6.3 km
        // of altitude at this latitude. t therefore came out as (camera altitude + 6269 m) at
        // EVERY altitude, so the wheel zoom orbited a point 6.3 km away: at 20 m altitude one
        // notch moved the camera 1.4 km, and the lateral component dragged it off the cursor.
        // Measured before: focus distance = altitude + 6269 m, exactly, from 20 m to 200 km.
        const double a = math::kWgs84SemiMajorAxis;
        const double b = math::kWgs84SemiMinorAxis;
        const math::Vec3 os( origin.x / a, origin.y / b, origin.z / a );
        const math::Vec3 ds( direction.x / a, direction.y / b, direction.z / a );
        const double ds_len2 = glm::dot( ds, ds );
        const double od = glm::dot( os, ds );
        const double c = glm::dot( os, os ) - 1.0;
        if ( ds_len2 <= 0.0 )
        {
            return false;
        }
        const double discriminant = od * od - ds_len2 * c;
        if ( discriminant <= 0.0 )
        {
            return false; // the ray misses the planet entirely
        }
        const double t = ( -od - std::sqrt( discriminant ) ) / ds_len2; // near intersection
        if ( t <= 0.0 )
        {
            return false; // at or inside the ellipsoid, looking out
        }

        out_ecef_y_up = math::Vec3( origin.x + direction.x * t, origin.y + direction.y * t,
                                    origin.z + direction.z * t );
        return true;
    }

    bool GlobeCameraController::tilt_pivot( math::Vec3 &out_ecef_y_up ) const
    {
        // The reference's getTiltCenter: the pivot is the ground point under the SCREEN CENTRE -
        // it intersects the ellipsoid along the camera's VIEW DIRECTION, not along the cursor.
        // Reading the cursor instead is what made the globe swing off the centre of the screen
        // while tilting, because the pivot was then a point the user had not aimed at.
        const godot::Viewport *viewport = get_viewport();
        const Vector2 screen_centre =
            viewport != nullptr ? viewport->get_visible_rect().size * 0.5f
                                : Vector2( 960.0f, 540.0f );

        if ( pick_ellipsoid_point( screen_centre, out_ecef_y_up ) )
        {
            return true;
        }

        // Ray misses the planet. The reference only gives up below MINIMUM_TRACKBALL_HEIGHT
        // (7.5e6 m); above it - the whole-globe viewpoint - it pivots on the ray's closest
        // approach to the centre, projected onto the surface. Without that fallback a tilt from
        // far out would silently become a free look, which is the one drag whose feel changes
        // depending on how far away the camera happens to be.
        if ( camera_height_above_ellipsoid() <= kMinimumTrackballHeight )
        {
            return false;
        }

        math::Vec3 origin;
        math::Vec3 direction;
        if ( !screen_ray( screen_centre, origin, direction ) )
        {
            return false;
        }
        const double a = math::kWgs84SemiMajorAxis;
        const double b = math::kWgs84SemiMinorAxis;
        const math::Vec3 os( origin.x / a, origin.y / b, origin.z / a );
        const math::Vec3 ds( direction.x / a, direction.y / b, direction.z / a );
        const double ds_len2 = glm::dot( ds, ds );
        if ( ds_len2 <= 0.0 )
        {
            return false;
        }
        // Closest approach of the ray to the centre, then back out onto the ellipsoid along the
        // same direction. Degenerate only if the ray passes through the centre itself.
        const double t = std::max( 0.0, -glm::dot( os, ds ) / ds_len2 );
        const math::Vec3 approach = os + ds * t;
        const double length = glm::length( approach );
        if ( length < 1e-12 )
        {
            return false;
        }
        const math::Vec3 surface = approach / length;
        out_ecef_y_up = math::Vec3( surface.x * a, surface.y * b, surface.z * a );
        return true;
    }

    // ---- wheel zoom ----

    Vector3 GlobeCameraController::wheel_focus_parent() const
    {
        if ( !wheel_focus_valid_ )
        {
            return resolve_pivot();
        }
        // The same conversion apply_tilt_drag uses for tilt_pivot_ecef_: the ECEF doubles
        // survive an origin shift unchanged, a cached parent-space point would not.
        const GlobeFrame &globe_frame = frame();
        const math::Vec3 focus_z_up( wheel_focus_ecef_.x, -wheel_focus_ecef_.z,
                                     wheel_focus_ecef_.y ); // Y-up -> Z-up
        const math::Vec3 focus_local = globe_frame.to_local( focus_z_up );
        return frame_point_to_parent( Vector3( static_cast<float>( focus_local.x ),
                                               static_cast<float>( focus_local.y ),
                                               static_cast<float>( focus_local.z ) ) );
    }

    void GlobeCameraController::update_zoom_easing( const double p_delta )
    {
        if ( !wheel_animating_ || !inertia_enabled_ )
        {
            return;
        }

        // The wrapper integrates in log-distance so zoom feels linear across five orders
        // of magnitude, and the process delta time plays the role of its clamped frame
        // time.
        const double dt = std::min( p_delta, 0.05 );
        const double damping = std::max( zoom_inertia_damping_, 0.1 );
        wheel_log_distance_ += wheel_log_velocity_ * dt * zoom_speed_scale_;
        wheel_log_distance_ = std::max( wheel_log_distance_, std::log( kWheelMinFocusDistance ) );
        wheel_log_velocity_ *= std::exp( -damping * dt );

        // The camera orbits the FOCUS, not the planet centre: the focus sits on the
        // surface, so the 10 m floor is 10 m from the ground at every altitude.
        const double distance = std::exp( wheel_log_distance_ );
        const Vector3 focus = wheel_focus_parent();
        set_position( focus + wheel_radial_direction_ * static_cast<float>( distance ) );
        // The floor is measured along the focus->camera ray, and a tilted view grazes
        // the surface sooner than that ray does - the ellipsoid clamp gets the final
        // word.
        enforce_camera_above_ellipsoid();
        sync_distance_from_camera();

        if ( std::abs( wheel_log_velocity_ ) < kStopFlickLogTravel * damping )
        {
            wheel_animating_ = false;
        }
    }

    void GlobeCameraController::update_drag_inertia( const double p_delta )
    {
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

        // The glide replays the last drag event once per frame, so its total is `decay` summed
        // over the frames that fit in one time constant - which is 1/dt of them. Multiplying by
        // dt/tau makes the sum 1 whatever the frame rate: the glide is bounded by one more drag
        // event's worth of travel. Without it the release of a 100 px flick carried the camera
        // 15x the gesture at 60 fps and 36x at 146 fps (measured: 100 px at 5 km altitude moved
        // 1.6 km while dragging and another 59 km after the button came up), which is the other
        // half of "drag a tile and it is instantly far away".
        const double tau = std::max( drag_inertia_coefficient_, 0.05 );
        const double glide = decay * std::min( p_delta, 0.05 ) / tau;

        // The same cursor-anchored transfer function and the same planet-centre pivot
        // apply_orbit_drag uses: the release tail has to continue the gesture, not switch to a
        // different law.
        const Vector3 pivot = resolve_pivot();
        const double distance = static_cast<double>( ( get_position() - pivot ).length() );
        const double altitude = camera_height_above_ellipsoid();
        const double ground_radius = std::max( distance - altitude, 1.0 );
        const double half_tan =
            std::tan( static_cast<double>( get_fov() ) * ( 0.5 * math::kPi / 180.0 ) );
        const double rotate_rate =
            clampd( altitude / ground_radius, 0.0, kMaximumRotateRate ) * rotate_speed_scale_;

        godot::Viewport *viewport = get_viewport();
        const double width =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.x ) : 1920.0;
        const double height =
            viewport != nullptr ? static_cast<double>( viewport->get_visible_rect().size.y ) : 1080.0;

        const double phi_ratio = std::min( -last_drag_dx_ / width, kMaximumMovementRatio );
        const Vector3 up = get_basis().get_column( 1 ).normalized();
        rotate_camera_around( pivot, up,
                              glide * kHorizontalSign * rotate_rate * ( width / height ) * half_tan *
                                  2.0 * phi_ratio );

        const double theta_ratio = std::min( -last_drag_dy_ / height, kMaximumMovementRatio );
        const double delta_theta = clamp_polar_pitch(
            pivot, glide * kVerticalSign * rotate_rate * half_tan * 2.0 * theta_ratio );
        const Vector3 right = get_basis().get_column( 0 ).normalized();
        rotate_camera_around( pivot, right, delta_theta );

        // Same latitude-drift undergrounding as apply_orbit_drag: the decay replay
        // rotates around the centre too, so the same clamp has to run after it.
        enforce_camera_above_ellipsoid();
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

        // Clamp like fly_to() does. A raw set_position() here let a distance below the local
        // surface radius park the camera *inside* the planet - orbit_to bypassed the clamp
        // that set_distance() and enforce_camera_above_ellipsoid() apply on other paths.
        const double surface_distance =
            glm::length( math::geodeticToYUp( longitude, latitude, 0.0 ) );
        const double clamped_distance =
            clampd( p_distance, surface_distance + kMinCameraHeight, max_distance() );

        const Vector3 position = pivot + surface_direction * static_cast<float>( clamped_distance );
        set_position( position );

        // The clamp above measures the surface with geodeticToYUp()'s geocentric radius; the
        // keep-above test measures the scaled ellipsoid. At mid-latitudes those differ by ~31 m,
        // which is enough for orbit_to to park the camera under the terrain - the demo's
        // underground probe measured it. Run the clamp before aiming, so the aim is the one the
        // drawn pose has.
        enforce_camera_above_ellipsoid();

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
        sync_distance_from_camera();
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
            // The target distance is expressed through geodeticToYUp()'s geocentric radius while
            // the keep-above test uses the scaled ellipsoid, and the two disagree by up to ~31 m
            // at mid-latitudes - so a flight that lands exactly where its own arithmetic says can
            // still finish under the terrain. That is what "it flew below the ground" was. The
            // clamp only runs here, on the last frame: running it every frame would fight the
            // interpolation.
            enforce_camera_above_ellipsoid();
            sync_distance_from_camera();
        }
    }

    // ---- input ----

    void GlobeCameraController::_unhandled_input( const Ref<InputEvent> &p_event )
    {
        const InputEventMouseButton *button = Object::cast_to<InputEventMouseButton>( p_event.ptr() );
        const InputEventMouseMotion *motion_event =
            Object::cast_to<InputEventMouseMotion>( p_event.ptr() );
        // A flight is cancelled by a camera GESTURE, not by the mouse merely moving. The dataset
        // picker starts its flight seconds after the click, once the load lands, and by then the
        // hand is usually still drifting a pixel at a time - cancelling on any non-zero relative
        // killed the flight the frame it began, which is what made switching datasets "often" not
        // fly at all. A button (press or release) or a drag in progress is a real gesture; a
        // window that gains focus emits a hover motion with a zero delta, which is not.
        const bool dragging = left_dragging_ || right_dragging_;
        if ( button != nullptr ||
             ( motion_event != nullptr && dragging &&
               motion_event->get_relative().length_squared() > 0.0f ) )
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
                    // The reference picks the tilt pivot once per drag, along the camera's view
                    // direction (see tilt_pivot). Missing it - the ray misses the planet and the
                    // camera is too low for the grazing fallback - means the whole drag is a free
                    // look instead.
                    tilt_pivot_valid_ = tilt_pivot( tilt_pivot_ecef_ );
                    tilt_looking_ = !tilt_pivot_valid_;
                }
                else
                {
                    right_dragging_ = false;
                    tilt_pivot_valid_ = false;
                    tilt_looking_ = false;
                }
            }
            else if ( button->is_pressed() &&
                      ( button->get_button_index() == MOUSE_BUTTON_WHEEL_UP ||
                        button->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN ) )
            {
                const bool zoom_in = button->get_button_index() == MOUSE_BUTTON_WHEEL_UP;

                // The wrapper's input guard: close enough is close enough. Everything
                // below measures distance to a ground focus, so without this the ease
                // would keep nibbling at the floor forever.
                if ( zoom_in && camera_height_above_ellipsoid() < kWheelMinFocusDistance )
                {
                    return;
                }

                // The zoom focus is the ground point under the screen centre (the
                // wrapper's getCameraFocus). A miss keeps the previous focus mid-burst;
                // a fresh burst without one falls back to the planet centre, which is
                // the pre-port behaviour for a camera looking at space.
                const bool fresh_burst = !wheel_animating_;
                math::Vec3 focus_ecef{};
                const godot::Viewport *viewport = get_viewport();
                const Vector2 screen_size = viewport != nullptr
                                                ? viewport->get_visible_rect().size
                                                : Vector2( 1920.0f, 1080.0f );
                if ( pick_ellipsoid_point( screen_size * 0.5f, focus_ecef ) )
                {
                    wheel_focus_ecef_ = focus_ecef;
                    wheel_focus_valid_ = true;
                }
                else if ( fresh_burst )
                {
                    wheel_focus_valid_ = false;
                }

                const Vector3 focus = wheel_focus_parent();
                const Vector3 radial_vector = get_position() - focus;
                const double focus_distance = static_cast<double>( radial_vector.length() );
                if ( focus_distance < 1e-6 )
                {
                    return;
                }
                wheel_radial_direction_ = radial_vector / radial_vector.length();

                if ( !inertia_enabled_ )
                {
                    // One notch multiplies the camera-to-focus distance - the same space
                    // the easing integrates in, so both paths agree on the step size.
                    const double step =
                        ( zoom_in ? -1.0 : 1.0 ) * kLogDistancePerNotch * zoom_speed_scale_;
                    const double next =
                        std::max( focus_distance * std::exp( step ), kWheelMinFocusDistance );
                    Vector3 next_position =
                        focus + wheel_radial_direction_ * static_cast<float>( next );
                    // Zooming out is still bounded by the orbit ceiling, measured to the
                    // centre like before.
                    const Vector3 pivot = resolve_pivot();
                    const double centre_distance =
                        static_cast<double>( ( next_position - pivot ).length() );
                    if ( centre_distance > max_distance() )
                    {
                        next_position =
                            pivot + ( next_position - pivot ) *
                                        static_cast<float>( max_distance() / centre_distance );
                    }
                    set_position( next_position );
                    enforce_camera_above_ellipsoid();
                    sync_distance_from_camera();
                    return;
                }

                // Re-based on EVERY notch - the wrapper does the same (its state.logDist
                // assignment sits outside the animating check): the animation follows the
                // camera as it moves, and only a fresh burst starts from zero velocity.
                wheel_log_distance_ =
                    std::log( std::max( focus_distance, kWheelMinFocusDistance ) );
                if ( !wheel_animating_ )
                {
                    wheel_log_velocity_ = 0.0;
                    wheel_animating_ = true;
                }

                const double direction = zoom_in ? -1.0 : 1.0;
                // Scaling by the damping is what keeps the travelled distance independent of
                // it: the integrator divides velocity back out over the exponential tail.
                // No altitude scale on top - in focus space one notch is already a fixed
                // RATIO of the distance to the ground, which is exactly the uniformity the
                // old centre-distance integrator needed the scale to approximate.
                const double damping = std::max( zoom_inertia_damping_, 0.1 );
                const double max_velocity = kMaxFlickLogTravel * damping;
                // zoom_speed_scale_ is deliberately NOT folded in here: the integrator
                // multiplies it out every frame (update_zoom_easing), so adding it here too
                // would square the user's multiplier.
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
