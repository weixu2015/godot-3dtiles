// SPDX-License-Identifier: Unlicense
//
// Globe3D: a virtual Earth. Draws the WGS84 ellipsoid so 3D Tiles and other
// geographically placed content have a surface to sit on.
//
// Architecture (docs/GLOBE_PLAN.md section 3): Globe3D does NOT own the coordinate
// system. It resolves a Georeference3D upward, exactly the way Tileset3D does, so a
// Globe3D and any number of Tileset3D nodes end up sharing one ECEF-to-local frame
// simply by being siblings. There is no parent/child requirement in either direction -
// placing the Globe3D as the Tileset3D's parent would work but is not needed, and
// requiring it would break the standalone and Georeference3D-parented layouts that
// already exist.
//
// When no Georeference3D is found the globe falls back to its own frame anchored at
// (longitude 0, latitude 0), which is the same default the EarthCenteredEarthFixed
// resource uses. That keeps a lone Globe3D useful in an empty scene.

#ifndef GLOBE_3D_H
#define GLOBE_3D_H

#include "Georeference3D.h"

#include "core/math/Types.h"

#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/classes/texture2d.hpp"
#include "godot_cpp/variant/color.hpp"
#include "godot_cpp/variant/string.hpp"

namespace tiles3d
{
    class Globe3D : public godot::Node3D
    {
        GDCLASS( Globe3D, godot::Node3D )

    public:
        Globe3D();
        ~Globe3D() override;

        // ---- appearance ----

        /// Latitude/longitude subdivisions of the ellipsoid mesh. Higher values are
        /// smoother at the cost of vertices; 64 x 128 is a reasonable default for a
        /// globe that is mostly viewed from orbit.
        void set_radial_segments( int p_value );
        int get_radial_segments() const;

        /// Vertical subdivisions (pole to pole).
        void set_rings( int p_value );
        int get_rings() const;

        /// Uniform surface colour, used when no texture is set.
        void set_base_color( const godot::Color &p_color );
        godot::Color get_base_color() const;

        /// Equirectangular texture (world map). When set it replaces the base colour.
        void set_albedo_texture( const godot::Ref<godot::Texture2D> &p_texture );
        godot::Ref<godot::Texture2D> get_albedo_texture() const;

        /// Draws latitude/longitude lines. Useful while the texture pipeline is absent.
        void set_show_graticule( bool p_value );
        bool get_show_graticule() const;

        /// Rebuilds the ellipsoid mesh. Called automatically when segments, rings or the
        /// graticule toggle change; exposed for scripted use.
        void rebuild();

        // ---- geography ----

        /// Geodetic (degrees, metres above the ellipsoid) to a position in this node's
        /// local space. GDScript uses this to place objects on the globe.
        godot::Vector3 geodetic_to_local( double p_longitude_degrees, double p_latitude_degrees,
                                          double p_height ) const;

        /// Converts a mesh-space (ECEF, Y-up) position into this node's local frame.
        godot::Vector3 ecef_to_local( const godot::Vector3 &p_ecef ) const;

        /// ECEF (Y-up, metres) to geodetic. Returns (longitude, latitude, height) in
        /// degrees and metres.
        godot::Vector3 local_to_geodetic( const godot::Vector3 &p_local ) const;

        /// The Georeference3D this globe shares a frame with, or null when it is using
        /// its own fallback frame. Mirrors Tileset3D::find_georeference.
        const Georeference3D *find_georeference() const;

    protected:
        static void _bind_methods();
        void _notification( int p_what );

    private:
        int radial_segments_ = 64;
        int rings_ = 32;
        godot::Color base_color_{ 0.09f, 0.16f, 0.32f, 1.0f };
        godot::Ref<godot::Texture2D> albedo_texture_;
        bool show_graticule_ = true;

        godot::MeshInstance3D *surface_ = nullptr;
        godot::MeshInstance3D *graticule_ = nullptr;

        /// Cached fallback frame, built lazily. Only consulted when there is no
        /// Georeference3D ancestor.
        mutable bool fallback_frame_built_ = false;
        mutable math::Mat4 fallback_ecef_to_local_{};

        /// ECEF -> this node's local frame. Georeference3D when present, else the
        /// fallback. This is the only place the two paths differ.
        math::Mat4 ecef_to_local_matrix() const;

        /// Creates the Surface and Graticule children on first use. Idempotent.
        void ensure_children();

        void rebuild_surface();
        void rebuild_graticule();
        void update_materials();
    };

} // namespace tiles3d

#endif
