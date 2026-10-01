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
// When no Georeference3D is found the globe uses the Earth's centre as its local origin, so
// the drawn ellipsoid and the geography API describe the same points with no offset. Under a
// Georeference3D both move to that frame's anchor instead, which is what lines the surface up
// with sibling Tileset3D content.

#ifndef GLOBE_3D_H
#define GLOBE_3D_H

#include "Georeference3D.h"

#include "GlobeFrame.h"
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

        // ---- atmosphere ----

        /// Draws a rim-glow shell around the ellipsoid. This is the cheap, Godot-native
        /// stand-in for the reference implementation's raymarched atmosphere: a back-face
        /// shell a few percent larger than the globe, shaded by a fresnel term so it is
        /// brightest at the limb and transparent when viewed head-on. See
        /// docs/GLOBE_PLAN.md P4 for why the raymarch was rejected.
        void set_show_atmosphere( bool p_value );
        bool get_show_atmosphere() const;

        /// Atmosphere shell radius, as a fraction of the ellipsoid radius. 1.02 reads as a
        /// thin shell; the reference's optical depth corresponds to roughly this.
        void set_atmosphere_scale( double p_value );
        double get_atmosphere_scale() const;

        /// Colour at the limb (horizon). Defaults to a Rayleigh-ish blue.
        void set_atmosphere_color( const godot::Color &p_color );
        godot::Color get_atmosphere_color() const;

        /// Multiplies the fresnel emission. Raise for a thicker-looking atmosphere.
        void set_atmosphere_intensity( double p_value );
        double get_atmosphere_intensity() const;

        /// How tightly the glow hugs the limb. Higher values keep the rim thinner and the
        /// disc clearer; lower values wash the whole globe.
        void set_atmosphere_falloff( double p_value );
        double get_atmosphere_falloff() const;

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

        /// The shared world frame (see GlobeFrame): georeference frame when one is an
        /// ancestor, otherwise Y-up ECEF with the flip baked in. Everything this node
        /// draws and every geography answer is expressed through it, which is what keeps
        /// the globe aligned with sibling Tileset3D content.
        mutable GlobeFrame frame_{};
        mutable bool frame_resolved_ = false;

        const GlobeFrame &frame() const;

        /// The Georeference3D this globe shares a frame with, or null when it is using
        /// its own fallback frame. Mirrors Tileset3D::find_georeference.
        const Georeference3D *find_georeference() const;

    protected:
        static void _bind_methods();
        void _notification( int p_what );

    private:
        int radial_segments_ = 64;
        int rings_ = 32;
        // A mid blue that reads as an ocean-covered planet under a single directional light.
        // Earlier values around (0.09, 0.16, 0.32) looked nearly black once the tonemapper
        // and the terminator were applied; this keeps the lit side legible.
        godot::Color base_color_{ 0.22f, 0.42f, 0.72f, 1.0f };
        godot::Ref<godot::Texture2D> albedo_texture_;
        bool show_graticule_ = true;

        bool show_atmosphere_ = true;
        double atmosphere_scale_ = 1.06;
        godot::Color atmosphere_color_{ 0.30f, 0.55f, 1.0f, 1.0f };
        double atmosphere_intensity_ = 2.4;
        // High enough that the glow reads as a band on the limb rather than a haze over the
        // whole disc. Lower values wash the night side grey, since the shell is additive.
        double atmosphere_falloff_ = 8.0;

        godot::MeshInstance3D *surface_ = nullptr;
        godot::MeshInstance3D *graticule_ = nullptr;
        godot::MeshInstance3D *atmosphere_ = nullptr;

        /// Creates the Surface and Graticule children on first use. Idempotent.
        void ensure_children();

        void rebuild_surface();
        void rebuild_graticule();
        void rebuild_atmosphere();
        void update_materials();
    };

} // namespace tiles3d

#endif
