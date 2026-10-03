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
#include "godot_cpp/classes/quad_mesh.hpp"
#include "godot_cpp/classes/texture2d.hpp"
#include "godot_cpp/variant/color.hpp"
#include "godot_cpp/variant/string.hpp"
#include "godot_cpp/variant/vector2.hpp"
#include "godot_cpp/variant/vector3.hpp"

#include <limits>

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

        /// Draws latitude/longitude lines. A build-time debug aid for the bare ellipsoid:
        /// once real tiles stream, the lines sit at a fixed +1 km altitude and fight the
        /// tile surfaces for depth, showing up as dashes along tile boundaries that read
        /// as LOD cracks. Default off; turn on only for a bare-globe sanity check.
        void set_show_graticule( bool p_value );
        bool get_show_graticule() const;

        /// Whether the built-in ellipsoid mesh is drawn.
        ///
        /// This exists because that mesh and a streaming GlobeTileLayer are *the same
        /// surface*. Both are placed on the WGS84 ellipsoid, so wherever a tile covers a
        /// pixel the two are coplanar and z-fight: the render is the flat base_color blue
        /// with a lattice of tile fragments punching through it, and the imagery is only
        /// visible in the gaps. That is not a subtle colour error - it hides the entire
        /// dataset behind an opaque shell - and no intensity or exposure change can fix
        /// it, because the pixels are being decided by depth, not by shading.
        ///
        /// AUTO (the default) draws the mesh only while no tile layer is actually
        /// rendering, which is the one case where the ellipsoid *is* the visible surface:
        /// a bare globe with no imagery. That decision is re-evaluated per frame, so
        /// attaching a GlobeTileLayer hides the shell with no further configuration, and
        /// removing it brings the shell back.
        void set_show_surface( int p_value );
        int get_show_surface() const;

        // ---- atmosphere ----
        //
        // The atmosphere is a real single-scattering integral, ported from the reference
        // implementation (`atmosphere.ts` GLSL_ATMO_COMMON). It used to be a back-face fresnel
        // shell, which cannot work: a shell at a fixed radius has no fragments beyond its own
        // silhouette, so the glow necessarily ends on a hard geometric line. The screenshot
        // that prompted this rewrite showed exactly that - a flat blue annulus painted around
        // the limb, with the planet lit from an unrelated direction.
        //
        // The raymarch integrates instead of painting: 16 view samples x 4 light samples of
        // Rayleigh + Mie density, with the optical depth feeding an exponential attenuation.
        // Because the integrand is the *air* rather than a surface, the glow thins out
        // continuously as the view ray leaves the shell, and the night side falls off on its
        // own. The shell only has to be large enough to contain the integration volume.

        /// Draws the raymarched atmosphere shell around the ellipsoid.
        void set_show_atmosphere( bool p_value );
        bool get_show_atmosphere() const;

        /// Atmosphere shell radius, as a fraction of the ellipsoid radius. This is *not* the
        /// atmosphere's physical thickness any more: the integral always spans
        /// ATMOSPHERE_THICKNESS (111 km) from the surface, and this only has to be large
        /// enough to contain that volume - 1.025 is the reference's value. Raising it past
        /// what is needed simply moves the shell out of the way; lowering it below the
        /// required radius clips the integral.
        void set_atmosphere_scale( double p_value );
        double get_atmosphere_scale() const;

        /// Daylight scattering tint. The raymarch derives its own colour from the Rayleigh/Mie
        /// coefficients, so this is a *multiplier* on the integrated result rather than the
        /// limb colour: (1,1,1) leaves the physical blue untouched.
        void set_atmosphere_color( const godot::Color &p_color );
        godot::Color get_atmosphere_color() const;

        /// Exposure applied to the scattering integral. The reference value is deliberately
        /// huge (uAtmosphereLightIntensity = 50) because the coefficients are ~1e-6 per metre;
        /// 45.0 here reproduces the reference look at the demo's camera distances.
        void set_atmosphere_intensity( double p_value );
        double get_atmosphere_intensity() const;

        /// Sunset tint. A pure single-scattering integral never goes orange, because the sun
        /// direction is a uniform - the light does not bend as it grazes the atmosphere, and
        /// the Rayleigh 1/lambda^4 term alone cannot redden the limb. The reference does not
        /// need this because it renders the sun as an HDR emitter and lets the
        /// (non-tone-mapped) Mie forward lobe bloom; without that, the terminator reads as a
        /// flat grey fade. This warms the scattering by the angle between the view ray and
        /// the sun: 0 = physically neutral, 1 = full sunset. Set 0.0 for a strictly unmodified
        /// integral.
        void set_atmosphere_sunset_tint( double p_value );
        double get_atmosphere_sunset_tint() const;

        /// Diagnostic hook: forces the atmosphere to integrate with the Rayleigh phase alone
        /// (no Mie forward lobe, no sunset tint), which is what a naive reading of the
        /// reference would produce. Exists because "it still looks wrong" is otherwise
        /// impossible to attribute to a specific term.
        void set_atmosphere_debug_pure( bool p_value );
        bool get_atmosphere_debug_pure() const;

        void set_ground_atmosphere( bool p_value );
        bool get_ground_atmosphere() const;
        void set_ground_atmosphere_intensity( double p_value );
        double get_ground_atmosphere_intensity() const;
        void set_atmosphere_ground_fade( bool p_value );
        bool get_atmosphere_ground_fade() const;

        /// Diagnostic: how much of the ground pass's output chain to run.
        ///
        /// "The imagery is missing", "the veil is too strong" and "the whole planet is too dark"
        /// all look like the same muddy blue ball in a screenshot, and they have completely
        /// different causes - a UV/binding fault in the material this shader replaced, a magnitude
        /// problem in the scattering integral, and a colour-space round trip that is not
        /// happening. A single mode per run cannot tell them apart, so this is a level, not a
        /// switch:
        ///
        ///   0 - normal. The full ground pass.
        ///   1 - the raw texture fetch, nothing else. The floor.
        ///   2 - the raw fetch *and* the sRGB -> linear decode, still no lighting and no
        ///       scattering.
        ///
        /// Mode 2 is what settled the colour-space question. Decoding and re-encoding is the
        /// identity, so if the renderer re-encoded ALBEDO the two images would match; they differ
        /// by 5x on the day side, which is how we know it does not. The consequence - the ground
        /// pass must work in display space and encode explicitly on the way out - is written up at
        /// length on kGlslAtmoCommon in GlobeAtmosphereShading.cpp. Kept here as well because this
        /// is the property someone reaches for when the planet looks dim, and the instinct to add a
        /// decode here is exactly the bug that was removed.
        int ground_debug_albedo_ = 0;

    public:
        void set_ground_debug_albedo( int p_value );
        int get_ground_debug_albedo() const;

        /// Rebuilds the ellipsoid mesh. Called automatically when segments, rings or the
        /// graticule toggle change; exposed for scripted use.
        void rebuild();

        /// Resolves show_surface_ against the current tile state and applies it. Called
        /// every frame, because whether a tile layer is rendering can change at any time.
        void update_surface_visibility();

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
        bool show_graticule_ = false;
        // -1 = auto (hide while tiles render), 0 = never, 1 = always. See set_show_surface.
        int show_surface_ = -1;
        // Last applied visibility, so the per-frame auto check only touches the node on a
        // change instead of every frame.
        bool surface_visible_applied_ = true;

        bool show_atmosphere_ = true;
        double atmosphere_scale_ = 1.025;
        godot::Color atmosphere_color_{ 1.0f, 1.0f, 1.0f, 1.0f };
        // The reference's own value is 50.0 (atmosphere.ts, uAtmosphereLightIntensity); the
        // first port shipped 45.0, measured bit-identical to the reference at globe-view
        // distances. 45 also drives the limb halo into saturated white from the day-facing
        // views (Mie forward lobe over the Rayleigh blue), which is the "atmosphere glow too
        // bright" report - so the shipped value is now 25.0: the rim keeps its colour and
        // falls off before it blows out. Tunable per scene; the *arithmetic* stays the
        // reference's.
        double atmosphere_intensity_ = 25.0;
        double atmosphere_sunset_tint_ = 0.65;
        bool atmosphere_debug_pure_ = false;

        // ---- ground atmosphere: the blue veil over the disc ----
        //
        // Separate from the shell above, because it is a separate pass in the reference too
        // (applyDayNightShading, injected into the tile material). The shell can only brighten
        // the ring *outside* the silhouette; the air column between the camera and the ground
        // is what actually tints the planet, and it is view-angle dependent - invisible
        // straight down, hundreds of km deep at the limb. That gradient is most of what makes
        // the sphere read as a sphere with air on it.
        bool ground_atmosphere_ = true;
        // The reference drives the ground pass from uGroundLightIntensity = 10.0; reusing the
        // shell value washes the disc out. 6.0 rather than the reference's 10.0: at grazing
        // angles (the limb) the veil's exposure curve saturates the imagery to white, which
        // read as a "radiating" pole/rim on day-facing views. Tunable per scene.
        double ground_atmosphere_intensity_ = 6.0;
        // When true, the effect fades in over camDist 6.5e6-9.0e6 m like the reference's
        // uLightingFade. Exposed because a close-up of the dataset is a legitimate thing to
        // want, and at 300 m altitude the reference would render no atmosphere at all.
        bool atmosphere_ground_fade_ = true;

        // ---- sun (drives the atmosphere's lighting direction) ----
        //
        // One direction feeds the scattering integral and anything that wants to agree with
        // it. A DirectionalLight3D in the scene is NOT consulted: a light's position is free
        // and its orientation is invisible in the .tscn, so reading it back would make the
        // terminator depend on how someone happened to drag the light in the editor. The
        // reference derives the sub-solar point from the clock instead, which is also what
        // makes day/night reproducible between runs and between the web and Godot builds.
        //
        // The accessors below are part of the public API (they are bound to ClassDB), so the
        // access specifier is re-opened here rather than the whole sun block being pushed down
        // into the private section.

    public:
        /// Draws a sun glow billboard in the sub-solar direction, pinned to the camera so its
        /// angular size is constant and it never goes behind the far plane.
        ///
        /// This is the scene's master sun switch, not just the billboard's: unchecked means
        /// "no sun", so the atmosphere's day/night shading is disabled with it (flat-lit
        /// globe, no terminator). The night side cannot be toggled independently of the
        /// billboard because both come from the same sub-solar direction.
        void set_show_sun( bool p_value );
        bool get_show_sun() const;

        /// Sun glow angular radius, as a multiple of the real solar angular radius
        /// (0.00465 deg). The reference draws the glow, not the photosphere, so 1.0 already
        /// spans the real disc plus its glow skirt.
        void set_sun_angular_scale( double p_value );
        double get_sun_angular_scale() const;

        /// Overrides the sub-solar point, in degrees. Both default to NaN, which means
        /// "derive from the clock" (the reference's ?sunlon/?sunlat). Used by the audit
        /// scenes to freeze the terminator somewhere reproducible.
        void set_sun_longitude_degrees( double p_value );
        double get_sun_longitude_degrees() const;
        void set_sun_latitude_degrees( double p_value );
        double get_sun_latitude_degrees() const;

        /// The sub-solar point (longitude, latitude) in degrees, for display.
        godot::Vector2 get_sub_solar_point() const;

        /// The sub-solar direction, as a unit vector in this node's local space. This is the
        /// direction from the Earth's centre towards the sun, which is what a shader wants;
        /// `sun_position_local()` is the same ray extended to a point.
        godot::Vector3 get_sun_direction() const;

        /// A point 400,000 km along the sun direction, in this node's local space, for
        /// placing a visual sun. Named for what it is: the sun is not at a meaningful
        /// distance here, only a direction.
        godot::Vector3 get_sun_position() const;

        /// Sub-solar direction in Y-up ECEF (Godot's axis convention, as
        /// `core::math::geodeticToYUp` emits it). Recomputed on the wall clock unless an
        /// override is set. Exposed so the demo can feed the same vector to a
        /// DirectionalLight3D instead of inventing its own.
        godot::Vector3 compute_sun_direction_y_up() const;

    private:
        // ---- per-frame atmosphere / sun update ----
        //
        // The marcher needs the viewer position in the same *local* space the shell mesh is
        // built in, plus the height above the ellipsoid; both are pushed into the material
        // each frame from NOTIFICATION_PROCESS. Reading CAMERA_POSITION_WORLD in the shader
        // instead would be free, but the mesh is local while that built-in is world space, and
        // the two differ by the Georeference3D's Z-up -> Y-up flip. Mixing them rotates the
        // whole integral by 90 degrees.
        godot::Node3D *find_camera() const;

        /// The active camera's position in Y-up ECEF metres from the Earth's centre. The
        /// atmosphere's integral is carried out in that space (see
        /// update_atmosphere_uniforms), so this is the one place the camera is converted into
        /// it. Returns the ellipsoid centre when there is no camera, which is a degenerate but
        /// finite answer rather than a NaN.
        godot::Vector3 get_camera_ecef_y_up() const;

        /// Positions the Atmosphere child so that geometry emitted in Y-up ECEF lands
        /// correctly, given the shared frame this node resolved. Re-run on every rebuild and
        /// whenever the frame may have moved.
        void apply_ecef_y_up_placement();

        void update_atmosphere_uniforms();
        void update_sun_pose();

        /// Solar geometry for a given instant, as the reference `computeSubSolarPoint`.
        /// Returns the sub-solar longitude/latitude in radians. The low-precision solar
        /// position algorithm is accurate to ~0.01 deg, which is far past what a terminator
        /// on a globe needs.
        static godot::Vector2 sub_solar_point_radians( double p_unix_seconds );

        bool show_sun_ = true;
        double sun_angular_scale_ = 1.0;
        // NaN means "derive from the clock". A sentinel rather than a separate bool because
        // the override is read every frame and must not need a rebuild to toggle.
        double sun_longitude_degrees_ = std::numeric_limits<double>::quiet_NaN();
        double sun_latitude_degrees_ = std::numeric_limits<double>::quiet_NaN();

        godot::MeshInstance3D *surface_ = nullptr;
        godot::MeshInstance3D *graticule_ = nullptr;
        godot::MeshInstance3D *atmosphere_ = nullptr;
        godot::MeshInstance3D *sun_ = nullptr;
        /// Kept alive so the sun billboard can be rescaled without rebuilding the mesh.
        godot::Ref<godot::QuadMesh> sun_quad_;

        /// Sun glow texture, generated once. See make_sun_glow_texture().
        godot::Ref<godot::Texture2D> sun_texture_;

        /// Creates the Surface and Graticule children on first use. Idempotent.
        void ensure_children();

        void rebuild_surface();
        void rebuild_graticule();
        void rebuild_atmosphere();
        void rebuild_sun();
        void update_materials();
    };

} // namespace tiles3d

#endif
