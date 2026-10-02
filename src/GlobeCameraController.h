// SPDX-License-Identifier: Unlicense
//
// GlobeCameraController: the trackball-style camera the globe reference page uses.
//
// Attach this to a Camera3D (docs/GLOBE_PLAN.md P3) - the camera is the thing that moves,
// not the globe, which is the same split Cesium for Unity draws between its origin-shift
// behaviour and the georeference.
//
// Ported from the reference implementation:
//   web-spatial-examples/apps/main/src/views/globe/renderers/QuadtreeGlobe.ts
//   (initEvents / updateZoomEasing / updateDragInertia / enforceCameraAboveEllipsoid /
//    getCameraPose / setCameraPose)
//
// Coordinate difference from the reference: the reference camera orbits the ECEF origin
// (0, 0, 0) because its scene is centred on the Earth's centre. Here the scene origin is the
// georeference frame, which normally sits ON the ellipsoid. The controller therefore has to
// know where the Earth's centre is in the camera's parent space; resolve_ellipsoid_center()
// derives it from a sibling Globe3D / Georeference3D, and falls back to the frame origin.
//
// All the geometry below is Y-up, matching Globe3D and the kernel's geodeticToYUp.

#ifndef GLOBE_CAMERA_CONTROLLER_H
#define GLOBE_CAMERA_CONTROLLER_H

#include "GlobeFrame.h"
#include "core/math/Types.h"

#include "godot_cpp/classes/camera3d.hpp"
#include "godot_cpp/classes/input_event.hpp"
#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/variant/vector2.hpp"
#include "godot_cpp/variant/vector3.hpp"

namespace tiles3d
{
    class GlobeCameraController : public godot::Camera3D
    {
        GDCLASS( GlobeCameraController, godot::Camera3D )

    public:
        GlobeCameraController();
        ~GlobeCameraController() override;

        // ---- interaction toggles ----

        void set_inertia_enabled( bool p_enabled );
        bool get_inertia_enabled() const;

        /// Mouse/orbit sensitivity multiplier applied on top of the reference curve.
        void set_rotate_speed_scale( double p_scale );
        double get_rotate_speed_scale() const;

        void set_zoom_speed_scale( double p_scale );
        double get_zoom_speed_scale() const;

        // ---- inertia feel ----
        //
        // Every coefficient below is a time constant or a cap, so the *amount* of motion a
        // gesture produces is independent of how crisp it feels. That is deliberate: the
        // reference values (0.9 s spin decay, 2.5 zoom damping) let a drag keep spinning the
        // globe for over six seconds and a wheel flick keep zooming for about two, which is
        // unusable for inspecting a specific place.

        /// Drag spin decay, seconds. Larger = longer glide after release.
        void set_drag_inertia_coefficient( double p_seconds );
        double get_drag_inertia_coefficient() const;

        /// Hard stop for the drag spin, seconds after release. The exponential tail is cut
        /// here regardless of how fast the drag was.
        void set_drag_inertia_max_time( double p_seconds );
        double get_drag_inertia_max_time() const;

        /// Wheel zoom velocity decay, 1/seconds. Larger = the zoom settles sooner.
        void set_zoom_inertia_damping( double p_damping );
        double get_zoom_inertia_damping() const;

        /// Restores the reference page's coefficients (0.9 / infinite / 2.5), for comparing
        /// feel against the web implementation.
        void use_reference_inertia();

        /// Keeps near/far matched to the camera's altitude every frame. Without it a single
        /// fixed pair has to serve both the whole planet and a 400 m dataset, and the
        /// dataset loses: it ends up entirely in front of the near plane.
        void set_manage_clip( bool p_enabled );
        bool get_manage_clip() const;

        /// Camera pose API, for view synchronisation and automated tests.
        /// position/direction/up are in the camera's parent space; direction and up need
        /// not be orthogonal (they are orthonormalised here).
        void set_camera_pose( const godot::Vector3 &p_position, const godot::Vector3 &p_direction,
                              const godot::Vector3 &p_up );
        godot::Vector3 get_camera_direction() const;
        godot::Vector3 get_camera_up() const;

        /// Snaps the camera to a standard pose looking at the globe: `distance` metres from
        /// the ellipsoid centre, `longitude`/`latitude` degrees, looking straight down.
        void orbit_to( double p_longitude_degrees, double p_latitude_degrees, double p_distance );

        /// Same target as orbit_to(), but flown to over `seconds` instead of snapped: the
        /// direction is slerped and the distance eased in log space, so a flight across five
        /// orders of magnitude looks even. `seconds <= 0` falls back to a snap.
        void fly_to( double p_longitude_degrees, double p_latitude_degrees, double p_distance,
                     double p_seconds = 1.2 );

        /// True while a fly_to() is in progress. Any user input cancels it.
        bool is_flying() const;

        /// Distance from the ellipsoid centre, metres.
        double get_distance() const;
        void set_distance( double p_distance );

        /// The Earth centre in the camera's parent space. Public so tests and other nodes can
        /// align themselves with the same pivot.
        godot::Vector3 resolve_ellipsoid_center() const;

        /// Height of the camera above the ellipsoid surface, metres.
        double camera_height_above_ellipsoid() const;

        // ---- Node overrides ----
        // Public because godot-cpp's register_virtuals template has to reach them (the same
        // constraint that applies to _get_configuration_warnings).
        void _unhandled_input( const godot::Ref<godot::InputEvent> &p_event ) override;

    protected:
        static void _bind_methods();
        void _notification( int p_what );

    private:
        /// The shared globe frame, re-resolved whenever the pivot is (an ancestor
        /// Georeference3D or a sibling Globe3D may appear or move at any time).
        mutable GlobeFrame frame_{};
        mutable bool frame_valid_ = false;

        const GlobeFrame &frame() const;

        bool inertia_enabled_ = true;
        bool manage_clip_ = true;
        double rotate_speed_scale_ = 1.0;
        double zoom_speed_scale_ = 1.0;
        double drag_inertia_coefficient_ = 0.25;
        double drag_inertia_max_time_ = 1.0;
        double zoom_inertia_damping_ = 6.0;

        double distance_ = 0.0;

        bool left_dragging_ = false;
        bool right_dragging_ = false;
        godot::Vector2 last_mouse_position_;

        // Drag inertia window between release and the last motion.
        double last_drag_dx_ = 0.0;
        double last_drag_dy_ = 0.0;
        double mouse_down_time_ = 0.0;
        double mouse_up_time_ = 0.0;

        // Wheel zoom easing state (log-distance integrator, same as the reference).
        bool wheel_animating_ = false;
        double wheel_log_distance_ = 0.0;
        double wheel_log_velocity_ = 0.0;
        double wheel_last_time_ = 0.0;
        godot::Vector3 wheel_radial_direction_;

        godot::Vector3 resolve_pivot() const;

        // ---- frame space vs parent space (see the definitions for the full story) ----
        const godot::Node3D *resolve_frame_node() const;
        godot::Vector3 frame_point_to_world( const godot::Vector3 &p_frame_local ) const;
        godot::Vector3 frame_point_to_parent( const godot::Vector3 &p_frame_local ) const;
        godot::Vector3 frame_point_from_parent( const godot::Vector3 &p_parent_local ) const;
        godot::Vector3 frame_direction_to_parent( const godot::Vector3 &p_frame_direction ) const;

        /// Camera offset from the ellipsoid centre in Y-up ECEF metres, with the scaled
        /// length that turns the ellipsoid into the unit sphere. Shared by the height read
        /// and the "keep the camera outside the planet" clamp so they cannot disagree.
        struct EllipsoidOffset
        {
            math::Vec3 offset_y_up{};
            double radius = 0.0;
            double scaled_length = 0.0;
        };
        EllipsoidOffset ellipsoid_offset() const;
        void sync_distance_from_camera();
        void update_clip_planes();
        void enforce_camera_above_ellipsoid();
        void rotate_camera_around( const godot::Vector3 &p_pivot, const godot::Vector3 &p_axis,
                                   double p_angle );
        void apply_orbit_drag( double p_dx, double p_dy );
        void apply_tilt_drag( double p_dx, double p_dy );
        void update_zoom_easing( double p_delta );
        void update_drag_inertia( double p_delta );
        void update_fly( double p_delta );

        // Fly-to state. Directions are unit vectors in the camera's parent space, the same
        // space resolve_pivot() works in.
        bool fly_active_ = false;
        double fly_elapsed_ = 0.0;
        double fly_duration_ = 0.0;
        double fly_start_distance_ = 0.0;
        double fly_end_distance_ = 0.0;
        godot::Vector3 fly_start_direction_;
        godot::Vector3 fly_end_direction_;

        /// Closest the camera may get to the ellipsoid centre (just above the polar radius).
        static double min_distance();

        /// Furthest the camera may orbit: eight earth radii, the reference MAX_DIST.
        static double max_distance();
    };

} // namespace tiles3d

#endif
