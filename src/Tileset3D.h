// SPDX-License-Identifier: Unlicense
//
// Tileset3D: a 3D Tiles tileset node.
//
// Current stage: reads a tileset.json, builds the tile tree with the engine-agnostic
// kernel, and visualises the tile bounding volumes as wireframes. Content (b3dm / glb
// meshes and textures) is NOT rendered yet - that needs the traversal scheduler (phase 3)
// and the content pipeline (phase 4) from docs/REFACTOR_PLAN.md.
//
// The wireframe is deliberate and not just a placeholder: it makes the whole frame chain
// observable - parse, georeference, region conversion, world matrix accumulation - without
// depending on any content pipeline. It is the cheapest possible check that a dataset is
// interpreted correctly, and it exercises the D1 invariant for real.

#ifndef TILESET_3D_H
#define TILESET_3D_H

#include "Georeference3D.h"
#include "TilesetContentLoader.h"

#include "core/tiles/Tile.h"
#include "core/tiles/TilesetJson.h"

#include "godot_cpp/classes/camera3d.hpp"
#include "godot_cpp/classes/http_client.hpp"
#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/classes/ref.hpp"
#include "godot_cpp/classes/sub_viewport.hpp"
#include "godot_cpp/variant/string.hpp"
#include "godot_cpp/variant/vector3.hpp"

#include <array>
#include <deque>
#include <memory>
#include <vector>

namespace tiles3d
{
    class Tileset3D : public godot::Node3D
    {
        GDCLASS( Tileset3D, godot::Node3D )

    public:
        /// Everything the traversal needs from the camera.
        struct ViewState
        {
            math::Vec3 position{ 0.0 };
            double viewportHeight = 1080.0;
            double fovDegrees = 70.0;

            /// Unit vector the camera looks along, in this node's space. Used to rank loads
            /// by how central they are to the view - see LoadPriority.
            math::Vec3 forward{ 0.0, 0.0, -1.0 };
        };

    private:
        godot::String url;
        double maximum_screen_space_error = 16.0;

        /// Cap on requests in flight at once - fetching AND decoding combined.
        ///
        /// With the loader running on worker threads this is a throughput knob rather than a
        /// stall guard: the work no longer happens on the main thread, so raising it costs
        /// memory and bandwidth instead of frame time. The reference scheduler uses 20.
        int maximum_simultaneous_loads = 20;

        /// How many decoded tiles may be turned into Godot resources and attached per frame.
        ///
        /// This is the actual stall guard now. Assembly has to run on the main thread, so
        /// this bounds main-thread work per frame; the loader keeps filling the queue behind
        /// it. Sized so a cold start ramps up quickly without dropping a frame.
        int maximum_uploads_per_frame = 4;

        /// Hard cap on refinement depth; -1 = no cap beyond what the tileset's own geometric
        /// errors (and an implicit subtree's level cap) already impose.
        int maximum_level = -1;

        /// Freeze the selection traversal without tearing anything down.
        bool suspend_update = false;

        /// Print the per-frame LOD diagnostic line. On by default: this node is meant to be
        /// watched from the console while it is being tuned, and a shipping game turns it off.
        bool debug_print_lod = true;

        /// Draws the tile bounding volumes. On by default while there is no content
        /// rendering: it is the only thing that shows up in the editor.
        bool debug_show_bounding_volume = true;

        /// Multiplies the drawn bounding volumes, purely for visibility.
        double debug_bounding_volume_scale = 1.0;

        /// Web/cesium parity: master visibility toggle for the whole tileset subtree.
        bool show = true;

        /// Move the editor camera to frame the dataset once after it loads. Mirrors the
        /// web "flyTo" behaviour; only acts in the editor.
        bool auto_frame_on_load = true;

        /// Editor-only: keep the editor camera's near/far around the dataset, every frame.
        /// The editor writes its own clip planes during navigation (0.1/4000), which is fine
        /// for a scene measured in metres and clips away a dataset framed from kilometres
        /// away - and because this node may be the only thing in the scene, there is no
        /// globe layer to do it the way GlobeTileLayer::manage_editor_clip does.
        bool manage_editor_clip = true;

        /// True while connected to RenderingServer::frame_pre_draw; see assert_editor_clip.
        bool pre_draw_connected_ = false;

        /// How many frames the editor framing pose has been re-applied for, and whether any
        /// tile has been submitted yet. See the kFramingFrames note in the .cpp.
        int framing_frames_ = 0;
        bool tiles_rendered_once_ = false;

        /// One-shot latch for framing_released(), reset on every load.
        bool framing_released_emitted_ = false;

        /// The dataset's own up, as a direction in this node's local space, used only to
        /// orient the editor's opening view. (0, 0, 0) means "work it out": the geodetic up
        /// when the implicit frame left the content in ECEF, and +Y otherwise. A dataset
        /// authored in some other space sets this instead. It cannot move content - it only
        /// ever steers a camera - so unlike the placement rules it may fall back on a
        /// measurement of the coordinate magnitude.
        godot::Vector3 dataset_up_axis = godot::Vector3( 0.0f, 0.0f, 0.0f );

        /// The root bounding volume's eight corners in this node's local space, cached with
        /// dataset_center_ for the same reason. Framing a sphere of the same radius is not
        /// enough: a cube inscribed in it has corners sqrt(3) further out.
        std::array<math::Vec3, 8> dataset_corners_{};
        bool dataset_corners_valid_ = false;

        std::unique_ptr<core::Tile> root;
        godot::String asset_version;
        double root_geometric_error = 0.0;

        /// Content up axis, resolved from the tileset's `asset.gltfUpAxis`. Decides whether
        /// tile content needs an axis correction before it lines up with its bounding
        /// volume - see ContentFactory::createContentNode. Read once at the root and reused
        /// for every tile, external tilesets included.
        core::ModelUpAxis model_up_axis_ = core::ModelUpAxis::Y;

        /// Next free tile id. Seeded from the parser so implicit children materialised later
        /// keep the pre-order numbering unique.
        std::size_t next_tile_id_ = 0;

        std::size_t tile_count = 0;
        int maximum_depth = 0;
        godot::String last_error;

        /// Directory of the tileset document, used to resolve relative content URIs.
        godot::String base_directory;

        /// True when the tileset descends from a Georeference3D ancestor, false when it
        /// uses the implicit per-tileset georeference (origin centred on its own ECEF centre).
        bool placed_by_georeference = false;

        /// Colour the debug wireframe by tile depth; when false it draws a single colour.
        bool debug_colorize_tiles = true;

        /// Bounding-sphere radius of the whole dataset, used for camera framing.
        double dataset_radius = 0.0;
        /// Centre of the whole dataset in this node's own local space, i.e. already through
        /// `compute_model_matrix()`. NOT the node origin: with the implicit per-tileset frame
        /// the content stays where the tileset authored it, and for a dataset whose root is a
        /// `sphere` on the ellipsoid with no `transform` that is true ECEF, some 4.6e6 m from
        /// the node. Cached with the radius because the region conversion has already run by
        /// the time either can be measured.
        math::Vec3 dataset_center_ = math::Vec3( 0.0 );
        /// The same centre in the tileset's OWN space, before any frame is applied, plus the
        /// branch that decides how it reaches the render frame. Cached because the frame is not
        /// fixed: the demo re-anchors the Georeference3D onto a dataset when it loads, which
        /// re-expresses every node-local coordinate in the scene - so a node-local centre goes
        /// stale the instant the anchor moves, while this one does not.
        math::Vec3 dataset_center_raw_ = math::Vec3( 0.0 );
        bool dataset_root_is_region_ = false;
        // Filled by report_georeference() from the root transform; see the getters above.
        double dataset_longitude_ = 0.0;
        double dataset_latitude_ = 0.0;
        double dataset_height_ = 0.0;
        double anchor_separation_ = -1.0;

        /// Set after load; cleared once frame_camera() has run (editor framing only).
        bool needs_framing = false;

        /// Cached model matrix from load(). Needed because the implicit (no-georeference)
        /// frame is computed from the root bounding volume, which is rewritten from region to
        /// box during load - so it cannot be recomputed correctly afterwards.
        mutable std::optional<math::Mat4> model_matrix_;

        godot::MeshInstance3D *debug_mesh = nullptr;

        // ---- traversal state, rebuilt every frame ----

        std::vector<core::Tile *> render_list;

        /// Tiles the traversal wants loaded, with the priority they were ranked at. Sorted
        /// before dispatch so the visible centre of the screen is fetched first - the queue
        /// can hold well over a thousand entries on a dense dataset, and a FIFO would spend
        /// its bandwidth on tiles the camera has already flown past.
        struct QueuedLoad
        {
            core::Tile *tile = nullptr;
            LoadPriority priority;
        };

        std::vector<QueuedLoad> load_queue;

        /// Decoded, adoptable loads that did not fit last frame's upload budget. Moving them
        /// here instead of re-requesting the tile keeps the decode from being paid for twice.
        std::vector<CompletedLoad> deferred_loads;

        /// Off-thread fetch + decode. Owned as a child node so it is torn down with this one.
        TilesetContentLoader *loader = nullptr;

        /// Tiles requested last frame, so the ones the camera has stopped looking at can be
        /// cancelled. A load nobody can see is pure wasted bandwidth: the reference scheduler
        /// aborts any request not touched for a full frame, and a fast fly-through otherwise
        /// saturates the budget with tiles that are already behind the camera.
        std::vector<core::Tile *> requested_last_frame;

        /// Tiles whose content node is currently attached, so visibility can be toggled
        /// without walking the whole tree.
        std::vector<core::Tile *> loaded_tiles;

        /// Total bytes handed to the renderer so far, for the stats overlay.
        std::size_t loaded_bytes = 0;

        /// Bumped whenever the tile tree is thrown away. Loads still in flight carry the
        /// generation they were started under and are dropped when it no longer matches, so
        /// a reload never adopts content decoded from the previous dataset.
        std::uint64_t loader_generation = 0;

        std::int64_t frame_number = 0;
        std::size_t last_rendered_count = 0;

        void clear_loaded();
        void count_tiles();
        void build_debug_mesh();

        /// Replaces every tile whose `content.uri` points at another tileset document with
        /// that document's tile subtree, recursively.
        ///
        /// 3D Tiles signals "this tile is an external tileset" only through the URI
        /// extension, so without this step the nested document is fetched as if it were
        /// renderable content and fails with "unsupported tile content container". A dataset
        /// built from nested tilesets - taiwan, with 2791 of them - then shows nothing at
        /// all while still reporting a handful of tiles as loaded.
        /// Walks the tree once, with no I/O, and queues every tile whose content is a nested
        /// tileset document. Cheap enough to run inside load().
        void queue_external_tilesets();

        /// Drains that queue within a per-frame time budget. This is the difference between a
        /// node that opens instantly and one that blocks the editor for a minute: see the
        /// definition for the whole story.
        void process_external_tileset_queue();

        /// Queues the external-tileset documents found under `p_tile`, without reading any.
        void enqueue_external_descendants( core::Tile &p_tile );

        /// Reads and splices one tile's nested tileset document into it.
        void merge_external_tileset( core::Tile &p_tile );


        /// Rewrites the relative content URIs of a freshly merged subtree so they resolve
        /// against `directory`, the document that declared them, rather than against the
        /// root tileset's directory.
        void rebase_content_uris( core::Tile &tile, const godot::String &directory );

        /// Materialises a tile's implicit children from its subtree file, once.
        ///
        /// Implicit tiles have no children until the `.subtree` that describes their level is
        /// decoded, so the traversal calls this the first time a tile actually wants to
        /// refine. Doing it here - rather than expanding the whole tree at load - is what
        /// keeps a deep implicit root from materialising everything up front.
        void ensure_implicit_children( core::Tile &tile );

        /// The Georeference3D this node ultimately descends from, or null. A null result
        /// means the tileset uses its own implicit georeference (origin centred on its ECEF
        /// centre) - valid for a single tileset, forbidden when several share a scene.
        const Georeference3D *find_georeference() const;

        /// Prints where the dataset's own transform places it (longitude/latitude/height)
        /// and how far that is from the Georeference3D anchor, loudly when the two disagree.
        /// A mis-georeferenced dataset is indistinguishable from a rendering bug otherwise:
        /// it loads, every tile reports loaded, and the screen stays empty.
        void report_georeference();

        /// ECEF -> render frame. With a Georeference3D ancestor this is that node's
        /// ecef_to_local(); without one it is the implicit frame (ENU at the dataset's own
        /// ECEF centre, Z-up flipped to Godot Y-up). See docs/REFACTOR_PLAN.md D1.
        math::Mat4 compute_model_matrix() const;

        /// Strips a file:// prefix so Godot's FileAccess can open the result.
        godot::String resolved_path() const;

        /// Current camera, or null when there is none.
        ViewState current_view() const;

        void update_tiles();
        void traverse_tile( core::Tile &tile, const math::Mat4 &parent_world, const ViewState &view,
                            double nearest_conditional_ge );
        void request_content( core::Tile &tile, const LoadPriority &priority );

        /// Sorts the frame's requests by priority and hands what fits to the loader.
        void dispatch_loads();

        /// Cancels loads requested a frame ago that the traversal did not ask for again.
        void cancel_stale_loads();

        /// Adopts everything the loader finished: builds the Godot node on the main thread
        /// and attaches it, up to `maximum_uploads_per_frame`.
        void adopt_completed_loads();

        void sync_content_visibility();

        /// Resolves a document URL (tileset.json, external tileset, subtree) against this
        /// node's `url`, so the same code serves a local path and an http URL.
        godot::String document_url( const godot::String &reference ) const;

        godot::String content_path( const core::Tile &tile ) const;

        /// Reads a whole document into memory. Synchronous, and only used for the small
        /// control files (tileset.json, .subtree) that have to be parsed before the tree
        /// exists; payloads go through the loader.
        /// Forgets the kept-alive client, forcing the next document to reconnect. Const because the
        /// reuse state is mutable and read_document() is const - the reuse is a cache, not part of
        /// the node's value.
        void drop_http_reuse() const;

        bool read_document( const godot::String &path, godot::String &out_text,
                            godot::String &out_error ) const;

        /// The byte-level half of read_document, covering both a local path and an http(s)
        /// URL. Kept separate because .subtree files are binary.
        /// `p_retried` is the one-shot guard for the fresh-connection retry: a document is read
        /// again at most once when the kept-alive socket turned out to be dead, so a genuinely
        /// missing file costs two attempts instead of looping.
        bool read_binary_document( const godot::String &path, godot::PackedByteArray &out_bytes,
                                   godot::String &out_error, bool p_retried = false ) const;

        // Reused HTTP connection for the synchronous control-document fetches above. A
        // dataset switch reads the root document plus one document per external tileset
        // (31 on the weinan set); a fresh TCP connection per document measured ~9 ms each
        // (~270 ms of the switch hitch) against ~2 ms over a kept-alive one. mutable
        // because read_binary_document is const; invalidated by host/port change or by a
        // dead status, rebuilt on the next call.
        mutable godot::Ref<godot::HTTPClient> http_reuse_;
        mutable godot::String http_reuse_host_;
        mutable int http_reuse_port_ = -1;

        /// Tiles whose nested tileset document still has to be read, in discovery order.
        /// A worklist rather than a recursive walk because each entry costs a blocking HTTP
        /// round trip: doing them all inside load() put a minute of them between the editor
        /// opening a scene and the editor being usable.
        std::deque<core::Tile *> external_queue_;
        static constexpr double kExternalQueueBudgetSeconds = 0.006;
        /// When the kept-alive socket was last used, so a socket the server has probably closed in
        /// the meantime is not reused. See the reuse check in read_document.
        mutable double http_reuse_last_use_ = -1e30;

        /// Ceiling on one control-document request. A request written into a socket the server has
        /// already closed never gets a response and the client stays in STATUS_REQUESTING for
        /// ever, so the poll loop needs a wall-clock bound rather than only a state test.
        static constexpr double kDocumentRequestTimeoutSeconds = 8.0;
        /// How long a kept-alive socket may sit unused before the next document reconnects
        /// instead. Comfortably under a typical server keepalive timeout.
        static constexpr double kReuseIdleLimitSeconds = 30.0;

        void release_content( core::Tile &tile );

        /// Editor-only: flies the editor camera to frame the loaded dataset once.
        void frame_camera();

        /// Editor-only: the editor's 3D viewport camera, or null outside the editor, when it
        /// is one this node may drive (see has_globe_in_scene: a Globe3D in the same scene
        /// owns the camera instead).
        godot::Camera3D *resolve_editor_camera() const;

        /// The camera automatic framing is allowed to move, or null when there is none this
        /// node should touch: the editor's free camera while editing, otherwise the scene's
        /// own camera at runtime - and null whenever a Globe3D owns the camera, or when
        /// auto_frame_on_load is off.
        godot::Camera3D *resolve_framing_camera() const;

        /// Clip planes that bracket the dataset, from wherever the camera is. Not editor-only:
        /// a runtime camera framed onto a dataset 6.4e6 m from the world origin needs the
        /// same treatment as the editor's.
        void apply_framing_clip( godot::Camera3D *p_camera );

        /// Editor-only: the editor's 3D viewport, or null outside the editor.
        godot::SubViewport *editor_viewport() const;

        /// The dataset's bounding box corners in world space; false when nothing is loaded.
        bool dataset_box_corners( godot::Vector3 r_corners[8] ) const;

        void set_dataset_up_axis( const godot::Vector3 &p_value );
        godot::Vector3 get_dataset_up_axis() const;

        /// Editor-only: keeps the editor camera's clip planes around the dataset. The editor
        /// writes its own near/far during navigation, so a single write from the load frame
        /// does not survive; this runs from RenderingServer::frame_pre_draw instead, which is
        /// the last point before the frame is drawn.
        void assert_editor_clip( godot::Camera3D *p_camera );

        /// Bound so the RenderingServer frame_pre_draw Callable can resolve it by name.
        void _on_frame_pre_draw();

        void disconnect_pre_draw();

        /// Validates the node-tree rules (single implicit tileset vs shared georeference,
        /// no nesting). Returns true when the configuration is allowed to load.
        bool configuration_is_valid() const;

    protected:
        static void _bind_methods();
        void _notification( int p_what );

    public:
        Tileset3D();
        ~Tileset3D() override;

        void set_url( const godot::String &p_url );
        godot::String get_url() const;

        void set_maximum_screen_space_error( double p_value );
        double get_maximum_screen_space_error() const;

        void set_maximum_simultaneous_loads( int p_value );
        int get_maximum_simultaneous_loads() const;

        /// How many decoded tile meshes may be handed to the RenderingServer per frame. Loading
        /// (network + decode) is off the main thread, but creating the meshes is not, so this is
        /// the knob that decides how much of a frame a burst of arrivals is allowed to take.
        void set_maximum_uploads_per_frame( int p_value );
        int get_maximum_uploads_per_frame() const;

        /// Hard cap on the refinement depth. -1 (the default) means "whatever the tileset's own
        /// geometric errors ask for", which for implicit tiling is the subtree's level cap.
        /// Setting it lower is the quickest way to see how much of a frame's cost is the deep
        /// levels rather than the near ones.
        void set_maximum_level( int p_value );
        int get_maximum_level() const;

        /// Freeze the selection traversal without unloading anything: the camera keeps moving,
        /// the tree stops being re-evaluated. For poking at what is already on screen without
        /// the LOD churn underneath it.
        void set_suspend_update( bool p_value );
        bool get_suspend_update() const;

        /// Print the per-frame LOD line (camera, screen space errors, counts). On by default
        /// because this node exists to be inspected from the console, and off is the first thing
        /// a shipping game wants.
        void set_debug_print_lod( bool p_value );
        bool get_debug_print_lod() const;

        void set_debug_show_bounding_volume( bool p_value );
        bool get_debug_show_bounding_volume() const;

        void set_debug_bounding_volume_scale( double p_value );
        double get_debug_bounding_volume_scale() const;

        /// Master visibility toggle for the whole tileset subtree (wireframe + content).
        void set_show( bool p_value );
        bool get_show() const;

        /// Fly the editor camera to frame the dataset once after it loads (editor only).
        void set_auto_frame_on_load( bool p_value );
        bool get_auto_frame_on_load() const;
        void set_manage_editor_clip( bool p_value );
        bool get_manage_editor_clip() const;

        /// Colour the debug wireframe by tile depth; a single colour is used when false.
        void set_debug_colorize_tiles( bool p_value );
        bool get_debug_colorize_tiles() const;

        /// Reads and parses the tileset named by `url`. Does nothing when already loaded.
        void load();

        /// Drops everything and loads again.
        void reload();

        /// Drops everything without loading.
        void unload();

        /// Re-places already-attached content after the shared frame's *origin* moved - the
        /// floating-origin primitive (see GlobeCameraController::shift_origin_now).
        ///
        /// The traversal rebuilds every tile matrix from `compute_model_matrix()` each frame,
        /// so the next frame is correct on its own; what this fixes is the rest of the
        /// *current* one. The camera controller is the last sibling in the demo scene, so by
        /// the time it moves the origin this frame's traversal has already run and every
        /// attached content node is holding a matrix in the old local space. Left alone, the
        /// whole dataset would be drawn one origin-shift away from the camera for exactly one
        /// frame - which is the flash a naive floating origin is known for, and the one
        /// artifact a frame-difference probe can actually see.
        ///
        /// `p_parent_delta` is the shift expressed in this node's *parent* space, i.e. the
        /// georeference frame the traversal's model matrix produces.
        void rebase( const godot::Vector3 &p_parent_delta );

        // ---- where the dataset actually is ----
        //
        // Read from the tileset's own root transform, so they answer "where does this file
        // place itself", not "where did the scene put it". That is the difference between a
        // dataset that renders and one that loads every tile and shows nothing: fly to these
        // and you land on the data whether or not the Georeference3D agrees.
        //
        // Valid after `tileset_loaded`. Zero before it.

        double get_dataset_longitude() const;
        double get_dataset_latitude() const;
        /// Metres above the ellipsoid. Can be far from zero in a mis-georeferenced file.
        double get_dataset_height() const;
        /// Bounding sphere radius of the dataset, metres. 0 before load.
        double get_dataset_radius() const;
        /// Dataset centre in this node's local space, for scripts that orbit around it.
        godot::Vector3 get_dataset_center_local() const;
        /// The dataset centre through the frame as it is NOW; see the note on dataset_center_raw_.
        math::Vec3 current_dataset_center_local() const;
        /// Distance from the Georeference3D anchor to the dataset centre, metres. -1 when
        /// there is no Georeference3D.
        double get_anchor_separation() const;

        std::size_t get_tile_count() const;
        int get_maximum_depth() const;
        godot::String get_asset_version() const;
        double get_root_geometric_error() const;
        godot::String get_last_error() const;
        bool is_placed_by_georeference() const;

        /// Tiles whose content is currently attached, and how many were selected for
        /// rendering on the last traversal.
        std::size_t get_loaded_tile_count() const;
        std::size_t get_last_rendered_count() const;

        /// Payload bytes handed to the renderer so far, and the number of fetches/decode jobs
        /// currently in flight. Exposed for throughput diagnostics: a healthy concurrent
        /// loader keeps in-flight near `maximum_simultaneous_loads` while it streams and
        /// drains it as the view settles.
        std::size_t get_loaded_bytes() const;
        int get_in_flight_count() const;

        /// Prints the tile tree to the Godot console, `max_depth` levels deep.
        void dump_tree( int max_depth ) const;

        /// Editor configuration warnings for rules ③ and ④ (see docs/REFACTOR_PLAN.md):
        /// multiple Tileset3D without a shared Georeference3D, and nested Tileset3D. Must be
        /// public - the base Node declares it public and godot-cpp's register_virtuals needs
        /// access to bind it.
        godot::PackedStringArray _get_configuration_warnings() const override;
    };

} // namespace tiles3d

#endif
