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

#include "godot_cpp/classes/http_client.hpp"
#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/classes/ref.hpp"
#include "godot_cpp/variant/string.hpp"
#include "godot_cpp/variant/vector3.hpp"

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
        void expand_external_tilesets();

        void expand_external_tileset( core::Tile &tile, int depth );

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
        bool read_document( const godot::String &path, godot::String &out_text,
                            godot::String &out_error ) const;

        /// The byte-level half of read_document, covering both a local path and an http(s)
        /// URL. Kept separate because .subtree files are binary.
        bool read_binary_document( const godot::String &path, godot::PackedByteArray &out_bytes,
                                   godot::String &out_error ) const;

        // Reused HTTP connection for the synchronous control-document fetches above. A
        // dataset switch reads the root document plus one document per external tileset
        // (31 on the weinan set); a fresh TCP connection per document measured ~9 ms each
        // (~270 ms of the switch hitch) against ~2 ms over a kept-alive one. mutable
        // because read_binary_document is const; invalidated by host/port change or by a
        // dead status, rebuilt on the next call.
        mutable godot::Ref<godot::HTTPClient> http_reuse_;
        mutable godot::String http_reuse_host_;
        mutable int http_reuse_port_ = -1;

        void release_content( core::Tile &tile );

        /// Editor-only: flies the editor camera to frame the loaded dataset once.
        void frame_camera();

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
