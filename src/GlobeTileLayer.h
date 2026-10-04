// SPDX-License-Identifier: Unlicense
//
// GlobeTileLayer: the globe's imagery quadtree. Streams Web Mercator image tiles onto the
// ellipsoid, four-way subdividing by screen space error - the same scheme, traversal and
// appearance fallback as the reference implementation:
//   web-spatial-examples/apps/main/src/views/globe/renderers/QuadtreeGlobe.ts
//
// Layout: a sibling of Globe3D (and of Tileset3D under a Georeference3D). The layer walks
// its ancestors for a Georeference3D exactly the way Globe3D and Tileset3D do, so the
// three nodes always share one frame and tiles land on the globe they belong to.
//
// Editor behaviour mirrors Tileset3D: the traversal reads the *editor* viewport camera and
// the layer can fly it to frame the globe once on load, so dropping the node into a scene
// shows a textured planet immediately, without pressing Play.

#ifndef GLOBE_TILE_LAYER_H
#define GLOBE_TILE_LAYER_H

#include "GlobeFrame.h"
#include "GlobeTile.h"

#include "core/math/TileScheme.h"
#include "core/math/Types.h"

#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/variant/color.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/packed_string_array.hpp"
#include "godot_cpp/variant/string.hpp"

#include <unordered_map>
#include <vector>

namespace godot
{
    class Camera3D;
    class HTTPRequest;
}

namespace tiles3d
{
    class Georeference3D;

    class GlobeTileLayer : public godot::Node3D
    {
        GDCLASS( GlobeTileLayer, godot::Node3D )

    public:
        GlobeTileLayer();
        ~GlobeTileLayer() override;

        // ---- toggles ----

        /// Master switch. False freezes the traversal (already loaded tiles stay visible).
        void set_enabled( bool p_value );
        bool get_enabled() const;

        /// URL pattern with {z}/{x}/{y} (XYZ) or {q} (quadkey) placeholders. The default is
        /// the reference page's local Bing imagery endpoint, quadkey addressing and all:
        ///   web-spatial-examples: TILE_URL_PREFIX = "/api/tiles/bing/"
        ///                         TILE_URL_SUFFIX = ".jpeg?n=z&g=11404"
        /// The reference reaches it through the Vite dev proxy (/api -> http://localhost:9090),
        /// which rewrites the /api prefix away. Those files are a static cache rooted at
        /// E:/GISData, so Godot skips the proxy entirely and hits 9090 directly; the cache is
        /// the same quadkey-named .jpeg files the reference downloads.
        void set_url_template( const godot::String &p_value );
        godot::String get_url_template() const;

        /// Screen space error threshold for refinement, pixels (reference: 2.0).
        void set_maximum_screen_space_error( double p_value );
        double get_maximum_screen_space_error() const;

        /// Deepest quadtree level that may be requested.
        void set_maximum_level( int p_value );
        int get_maximum_level() const;

        /// LRU budget; the reference keeps 600.
        void set_tile_cache_size( int p_value );
        int get_tile_cache_size() const;

        /// Parallel texture downloads; the reference keeps 10.
        void set_max_concurrent_requests( int p_value );
        int get_max_concurrent_requests() const;

        /// Echo the traversal state (editor/run, framing pending, camera position, rendered
        /// tile count) every three seconds. On while debugging a scene that looks empty.
        void set_print_telemetry( bool p_value );
        bool get_print_telemetry() const;

        /// Keep the editor viewport camera's near/far planes usable for a planetary scene.
        ///
        /// The editor's 3D viewport camera ships with near/far = 0.1 / 4000 m, and the
        /// Viewport Settings dialog cannot express more than 1000000 m - less than the
        /// Earth's 6375000 m radius. Without this the planet is always clipped away, and the
        /// editor viewport stays black. The planes are re-asserted from the
        /// RenderingServer frame_pre_draw hook rather than only from _process, because the
        /// editor writes its own values during navigation and would otherwise hide the globe
        /// for the whole inertia tail.
        void set_manage_editor_clip( bool p_value );
        bool get_manage_editor_clip() const;

        /// Deterministic editor framing: longitude/latitude (degrees) and distance (metres)
        /// of the opening pose. Changing any of them re-frames the editor viewport.
        ///
        /// The editor's free camera cannot navigate a planet: its navigation state is private
        /// and its wheel step stays ~0.2 m no matter how far away the camera is (measured:
        /// 5 wheel notches moved the camera 1 m from a 16000 km framing distance, so the size
        /// on screen does not change). The layer learned the pose by writing the camera, the
        /// editor did not. These properties are therefore the supported way to change the
        /// editor view - run the scene to navigate with GlobeCameraController instead.
        void set_editor_view_longitude( double p_value );
        double get_editor_view_longitude() const;
        void set_editor_view_latitude( double p_value );
        double get_editor_view_latitude() const;
        void set_editor_view_distance( double p_value );
        double get_editor_view_distance() const;

        /// Re-apply the editor framing pose right now, using the current editor_view_*
        /// values. No-op outside the editor.
        void reframe_editor_view();

        /// Debug outline around every rendered tile (the reference's tile overlay).
        void set_show_tile_bounds( bool p_value );
        bool get_show_tile_bounds() const;

        /// Fly the editor camera to frame the globe once, after the layer enters the tree.
        /// What makes the node visible at a glance in the editor, the way Tileset3D does.
        void set_frame_editor_on_ready( bool p_value );
        bool get_frame_editor_on_ready() const;

        /// The credit line shown for the imagery. Deliberately *not* the Cesium logo the
        /// reference page borrows: this project ships its own branding here. Empty by
        /// default; the imagery provider's terms (Esri asks for attribution) decide what
        /// belongs in it once the in-world logo/credit view lands.
        void set_attribution( const godot::String &p_value );
        godot::String get_attribution() const;

        /// Frees every tile resource and rebuilds the four roots. Also re-reads the URL.
        void reload_tiles();

        /// Re-places every live tile after the shared frame's origin moved.
        ///
        /// The tile meshes are authored relative to their own centres (GlobeTile::
        /// rtc_center_ecef), so an origin shift only changes where those centres land in the
        /// layer's local space. That makes this one `set_position` per tile with a mesh - no
        /// vertex rewrite, no GPU upload, no allocation - which is what keeps a small rebase
        /// threshold affordable. The alternative (re-authoring the vertices) needs a new
        /// ArrayMesh, ShaderMaterial and MeshInstance3D per tile, and measures in the tens of
        /// milliseconds.
        void rebase();

        // ---- stats (read-only, for the HUD and tests) ----

        int get_rendered_tile_count() const;
        int get_loading_tile_count() const;
        int get_cached_tile_count() const;
        int get_max_selected_level() const;

    protected:
        static void _bind_methods();
        void _notification( int p_what );

    private:
        struct PendingRequest
        {
            GlobeTile *tile = nullptr;
            godot::HTTPRequest *request = nullptr;
        };

        bool enabled_ = true;
        // Mirrors the reference's Bing endpoint (quadkey + ".jpeg?n=z&g=11404"), minus the
        // /api prefix the Vite proxy strips, pointing straight at the local static cache.
        godot::String url_template_{
            "http://127.0.0.1:9090/tiles/bing/{q}.jpeg?n=z&g=11404" };
        double maximum_screen_space_error_ = 2.0;
        // Bing's service ceiling is 19, but this machine's static cache is the real limit
        // (audited 2026-10-04: levels 1-8 global, 9-11 dense only in some regions, 12+
        // empty). request()-time 404s mark tiles imagery_missing and the traversal stops
        // there, so a generous ceiling costs nothing; 16 keeps the worst-case request
        // count bounded.
        int maximum_level_ = 16;
        int tile_cache_size_ = 600;
        int max_concurrent_requests_ = 10;
        bool show_tile_bounds_ = false;
        bool frame_editor_on_ready_ = true;
        godot::String attribution_{};

        GlobeTile *roots_[4] = { nullptr, nullptr, nullptr, nullptr };
        TileReplacementQueue replacement_queue_;

        int frame_number_ = 0;
        int texture_version_ = 0;
        int inflight_count_ = 0;
        double last_failure_report_ = -1e30;
        int failure_report_count_ = 0;

        /// Circuit breaker for an unreachable tile server, counted over *connection* failures
        /// only - a 404 is an answer, not an outage, and a partially populated cache legitimately
        /// 404s a lot.
        ///
        /// It exists because of what a dead server looks like from inside the editor: the engine's
        /// own HTTPRequest logs `Condition "status != STATUS_BODY" is true` once per failed
        /// request, and that message cannot be throttled from here. With the layer dispatching
        /// every visible tile every frame, a stopped local cache server turns the output into
        /// hundreds of identical lines. After kConnectFailureLimit the layer stops dispatching
        /// for kConnectPauseSeconds and says so once; the next dispatch after that is the probe
        /// that finds the server back.
        int consecutive_connect_failures_ = 0;
        double connect_pause_until_ = -1e30;
        static constexpr int kConnectFailureLimit = 3;
        static constexpr double kConnectPauseSeconds = 5.0;
        double last_telemetry_stamp_ = -1e30;
        bool print_telemetry_ = false;
        bool manage_editor_clip_ = true;
        int next_request_id_ = 0;
        int rendered_count_ = 0;
        int max_selected_level_ = 0;
        bool needs_framing_ = false;
        // The framing pose is re-applied for several frames: the editor recreates clip planes
        // when the 3D viewport is rebuilt, a scene may contain another node that moves the
        // camera, and the editor resets the camera's near/far every frame - so a single shot
        // leaves a planet 6371 km across clipped away by a 4000 m far plane.
        int framing_frames_ = 0;
        // Held until the first frame actually draws tiles: that is the point where the pose is
        // proven to look at the globe.
        bool tiles_rendered_once_ = false;

        std::vector<GlobeTile *> high_queue_;
        std::vector<GlobeTile *> medium_queue_;
        std::unordered_map<int, PendingRequest> pending_;

        // Per-frame camera state, expressed in this node's local frame (the mesh space).
        // Opening pose for the editor viewport, also the demo's default viewpoint.
        double editor_view_longitude_ = 105.0;
        double editor_view_latitude_ = 25.0;
        double editor_view_distance_ = 1.6e7;

        /// Late re-assertion of the editor clip planes (see set_manage_editor_clip).
        void _on_frame_pre_draw();
        bool pre_draw_connected_ = false;

        /// The editor's 3D viewport camera, or nullptr outside the editor / before it exists.
        godot::Camera3D *resolve_editor_camera() const;
        /// Writes the editor_view_* pose onto the editor camera. False when there is none.
        bool apply_editor_framing();
        /// Re-asserts near/far around the camera's current distance to the ellipsoid centre.
        void assert_editor_clip( godot::Camera3D *p_camera );
        /// Detaches the frame_pre_draw hook. RenderingServer outlives the scene, so a stale
        /// connection would keep firing (and reporting errors) after the node is freed.
        void disconnect_pre_draw();

        math::Vec3 camera_local_{ 0.0 };
        // Ellipsoid centre in this layer's local frame, refreshed each traversal so the
        // telemetry can say whether the camera is actually pointed at the planet.
        math::Vec3 center_cache_{ 0.0 };
        math::Vec3 camera_ecef_{ 0.0 }; // Y-up ECEF, metres from the ellipsoid centre
        GlobeFrame frame_{};
        double viewport_height_ = 0.0;
        double fov_radians_ = 1.0;
        math::Vec4 frustum_planes_[5]; // left, right, bottom, top, near (ax + by + cz + d)
        // ---- traversal (§6 in the reference) ----

        void update_tiles();
        godot::Camera3D *resolve_camera() const;
        void resolve_frame();
        void compute_frustum( godot::Camera3D *camera );
        bool sphere_intersects_frustum( const math::Vec3 &center_world, double radius ) const;

        double compute_distance_to_tile( GlobeTile *tile );
        double screen_space_error( const GlobeTile *tile ) const;
        bool compute_tile_visibility( GlobeTile *tile );
        void select_tiles();
        void visit_if_visible( GlobeTile *tile );
        void visit_tile( GlobeTile *tile );
        void show_tile_this_frame( GlobeTile *tile );
        void hide_stale_tiles( GlobeTile *tile );

        // ---- loading ----

        void queue_tile_load( GlobeTile *tile, std::vector<GlobeTile *> &queue );
        void process_load_queue();
        void dispatch_load( GlobeTile *tile, int request_id );
        void _on_tile_request_completed( int p_result, int p_response_code,
                                         godot::PackedStringArray p_headers,
                                         godot::PackedByteArray p_body, int p_request_id );
        void cancel_pending_requests();

        // ---- resources ----

        void create_tile_mesh( GlobeTile *tile );
        /// Recursive helper for rebase(): re-derives the node placement of `tile` and its
        /// descendants from their cached ECEF centres.
        void reapply_tile_placement( GlobeTile *tile );
        void update_tile_appearance( GlobeTile *tile );
        GlobeTile *find_appearance_source( GlobeTile *tile );
        bool subtree_used_this_frame( const GlobeTile *tile ) const;
        void free_subtree_resources( GlobeTile *tile );
        void trim_tiles();
        void destroy_roots();

        /// Kernel Z-up ECEF to this node's local (mesh) space, through the shared frame.
        math::Vec3 mesh_point( const math::Vec3 &ecef_z_up ) const;
        godot::String tile_url( const GlobeTile &tile ) const;
    };

} // namespace tiles3d

#endif
