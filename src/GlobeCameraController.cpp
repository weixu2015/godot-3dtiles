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

        constexpr double kWheelImpulse = 0.5;
        constexpr double kWheelMaxLogVelocity = 2.5;
        constexpr double kWheelDamping = 2.5;
        constexpr double kWheelStopThreshold = 0.02;

        constexpr double kInertiaSpinCoef = 0.9;
        constexpr double kInertiaMaxClickTime = 0.4;
        constexpr double kInertiaStopFactor = 0.001;

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

        ClassDB::bind_method(
            D_METHOD( "set_camera_pose", "p_position", "p_direction", "p_up" ),
            &GlobeCameraController::set_camera_pose );
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
                update_zoom_easing( get_process_delta_time() );
                update_drag_inertia( get_process_delta_time() );
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
        // Re-resolve whenever the node moves in the tree; resolve() itself is cheap (the
        // georeference caches its matrices) and keeps reparenting correct.
        frame_ = GlobeFrame::resolve( this );
        frame_valid_ = true;
        return frame_;
    }

    Vector3 GlobeCameraController::resolve_ellipsoid_center() const
    {
        // The Earth's centre is the origin of ECEF. Find the frame that maps ECEF into this
        // node's parent space, then ask where (0,0,0) lands.
        //
        // Both Globe3D and Tileset3D resolve their frame the same way, so walking the scene
        // root for one of them and asking for its ecef_to_local is enough. The frame origin
        // is on the ellipsoid, so ecef_to_local * (0,0,0) is the centre, roughly one Earth
        // radius away.
        godot::Node *root = get_parent();
        while ( root != nullptr && root->get_parent() != nullptr )
        {
            root = root->get_parent();
        }

        // Prefer a Georeference3D ancestor (explicit multi-scene setup).
        if ( const Georeference3D *reference = find_frame_ancestor( this ); reference != nullptr )
        {
            const math::Vec3 center =
                math::transformPoint( reference->ecef_to_local(), math::Vec3( 0.0 ) );
            return Vector3( static_cast<float>( center.x ), static_cast<float>( center.y ),
                            static_cast<float>( center.z ) );
        }

        // Otherwise look for a Globe3D anywhere in the scene and use its frame.
        if ( root != nullptr )
        {
            if ( const Globe3D *globe = find_globe_recursive( root ); globe != nullptr )
            {
                // Globe3D::ecef_to_local maps Y-up ECEF into the globe's local frame, whose
                // origin is the ellipsoid surface point the frame was anchored at. Passing
                // (0,0,0) (the Earth's centre in ECEF) therefore already yields the centre
                // expressed in local coordinates - about one radius from the local origin.
                // No negation: ecef_to_local already returns the frame-relative position.
                return globe->ecef_to_local( Vector3( 0.0f, 0.0f, 0.0f ) );
            }
        }

        // No globe in the scene: treat the parent origin as the pivot. Correct for a scene
        // whose root is the Earth's centre, which is how the fallback frame is laid out when
        // there is no georeference at all.
        return Vector3( 0.0f, 0.0f, 0.0f );
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

    double GlobeCameraController::camera_height_above_ellipsoid() const
    {
        // Height above the ellipsoid via the reference's scaled-space trick: express the
        // camera offset from the ellipsoid centre in Y-up ECEF metres, then divide by the
        // per-axis radii so the ellipsoid becomes the unit sphere.
        const GlobeFrame &globe_frame = frame();
        const Vector3 pivot = resolve_pivot();
        const Vector3 relative = get_position() - pivot;
        const math::Vec3 centre_ecef =
            globe_frame.to_ecef_z_up( math::Vec3( pivot.x, pivot.y, pivot.z ) );
        const math::Vec3 camera_ecef = globe_frame.to_ecef_z_up( math::Vec3(
            pivot.x + relative.x, pivot.y + relative.y, pivot.z + relative.z ) );
        const math::Vec3 offset_y_up(
            camera_ecef.x - centre_ecef.x,
            camera_ecef.z - centre_ecef.z, // Z-up -> Y-up along the way
            -( camera_ecef.y - centre_ecef.y ) );
        const double scaledLength = std::sqrt(
            ( offset_y_up.x / math::kWgs84SemiMajorAxis ) *
                ( offset_y_up.x / math::kWgs84SemiMajorAxis ) +
            ( offset_y_up.y / math::kWgs84SemiMinorAxis ) *
                ( offset_y_up.y / math::kWgs84SemiMinorAxis ) +
            ( offset_y_up.z / math::kWgs84SemiMajorAxis ) *
                ( offset_y_up.z / math::kWgs84SemiMajorAxis ) );
        if ( scaledLength <= 0.0 )
        {
            return 0.0;
        }

        const double radius = glm::length( offset_y_up );
        return radius * ( 1.0 - 1.0 / scaledLength );
    }

    void GlobeCameraController::enforce_camera_above_ellipsoid()
    {
        const GlobeFrame &globe_frame = frame();
        const Vector3 pivot = resolve_pivot();
        Vector3 relative = get_position() - pivot;
        const math::Vec3 centre_ecef =
            globe_frame.to_ecef_z_up( math::Vec3( pivot.x, pivot.y, pivot.z ) );
        const math::Vec3 camera_ecef = globe_frame.to_ecef_z_up( math::Vec3(
            pivot.x + relative.x, pivot.y + relative.y, pivot.z + relative.z ) );
        const math::Vec3 offset_y_up(
            camera_ecef.x - centre_ecef.x,
            camera_ecef.z - centre_ecef.z, // Z-up -> Y-up along the way
            -( camera_ecef.y - centre_ecef.y ) );
        const double scaledLength = std::sqrt(
            ( offset_y_up.x / math::kWgs84SemiMajorAxis ) *
                ( offset_y_up.x / math::kWgs84SemiMajorAxis ) +
            ( offset_y_up.y / math::kWgs84SemiMinorAxis ) *
                ( offset_y_up.y / math::kWgs84SemiMinorAxis ) +
            ( offset_y_up.z / math::kWgs84SemiMajorAxis ) *
                ( offset_y_up.z / math::kWgs84SemiMajorAxis ) );
        if ( scaledLength <= 0.0 )
        {
            return;
        }

        const double radius = glm::length( offset_y_up );
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
        wheel_log_distance_ += wheel_log_velocity_ * dt * zoom_speed_scale_;
        wheel_log_distance_ = std::max( wheel_log_distance_, std::log( min_distance() ) );
        wheel_log_velocity_ *= std::exp( -kWheelDamping * dt );

        const double distance = std::exp( wheel_log_distance_ );
        const Vector3 pivot = resolve_pivot();
        set_position( pivot + wheel_radial_direction_ * static_cast<float>( distance ) );
        sync_distance_from_camera();

        if ( std::abs( wheel_log_velocity_ ) < kWheelStopThreshold )
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

        const double decay = inertia_decay( since_release, kInertiaSpinCoef );
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
        // the pivot with the north pole as up. The lon/lat direction is a Y-up ECEF
        // direction; the shared frame rotates it into whatever the scene is using.
        const GlobeFrame &globe_frame = frame();
        const Vector3 pivot = resolve_pivot();
        const math::Vec3 direction_ecef(
            std::cos( latitude ) * std::cos( longitude ),
            std::sin( latitude ),
            -std::cos( latitude ) * std::sin( longitude ) );
        const math::Vec3 direction_local = globe_frame.local_direction( direction_ecef );
        const Vector3 surface_direction( static_cast<float>( direction_local.x ),
                                         static_cast<float>( direction_local.y ),
                                         static_cast<float>( direction_local.z ) );

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
    }

    // ---- input ----

    void GlobeCameraController::_unhandled_input( const Ref<InputEvent> &p_event )
    {
        const InputEventMouseButton *button = Object::cast_to<InputEventMouseButton>( p_event.ptr() );
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
                wheel_log_velocity_ += direction * kWheelImpulse;
                wheel_log_velocity_ =
                    clampd( wheel_log_velocity_, -kWheelMaxLogVelocity, kWheelMaxLogVelocity );
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
