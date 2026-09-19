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

#include "core/tiles/Tile.h"
#include "core/tiles/TilesetJson.h"

#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/variant/string.hpp"

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
        };

    private:
        godot::String url;
        double maximum_screen_space_error = 16.0;

        /// Content loads started per frame. Loading is synchronous on the main thread here,
        /// so this is the knob that keeps a cold start from stalling a single frame.
        int maximum_simultaneous_loads = 8;

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

        /// Set after load; cleared once frame_camera() has run (editor framing only).
        bool needs_framing = false;

        /// Cached model matrix from load(). Needed because the implicit (no-georeference)
        /// frame is computed from the root bounding volume, which is rewritten from region to
        /// box during load - so it cannot be recomputed correctly afterwards.
        mutable std::optional<math::Mat4> model_matrix_;

        godot::MeshInstance3D *debug_mesh = nullptr;

        // ---- traversal state, rebuilt every frame ----

        std::vector<core::Tile *> render_list;
        std::vector<core::Tile *> load_queue;

        /// Tiles whose content node is currently attached, so visibility can be toggled
        /// without walking the whole tree.
        std::vector<core::Tile *> loaded_tiles;

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

        /// The Georeference3D this node ultimately descends from, or null. A null result
        /// means the tileset uses its own implicit georeference (origin centred on its ECEF
        /// centre) - valid for a single tileset, forbidden when several share a scene.
        const Georeference3D *find_georeference() const;

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
        void request_content( core::Tile &tile );
        void process_load_queue();
        void sync_content_visibility();

        godot::String content_path( const core::Tile &tile ) const;
        godot::PackedByteArray read_tile_payload( const core::Tile &tile ) const;
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
