// SPDX-License-Identifier: Unlicense
//
// GlobeTile: one node of the globe's imagery quadtree, plus the LRU replacement queue
// that decides which tiles keep their GPU resources.
//
// Ported from the reference globe implementation:
//   web-spatial-examples/apps/main/src/views/globe/renderers/globe3d/QuadtreeTile.ts
//
// The tile is deliberately engine-free: geometry, textures and MeshInstance3D nodes are
// owned by GlobeTileLayer, which fills in the `mesh`/`texture` fields below. Keeping the
// tree itself inert makes it testable headlessly and makes resource teardown explicit.

#ifndef GLOBE_TILE_H
#define GLOBE_TILE_H

#include "GlobeFrame.h"
#include "core/math/TileScheme.h"
#include "core/math/Types.h"

#include "godot_cpp/variant/packed_vector2_array.hpp"
#include "godot_cpp/classes/ref.hpp"

#include <cstdint>
#include <functional>

namespace godot
{
    class ImageTexture;
    class MeshInstance3D;
}

namespace tiles3d
{
    class GlobeTile
    {
    public:
        // Ordered so that `state < DONE` means "not finished" (same trick as the
        // reference's needsLoading).
        enum class LoadState : int
        {
            START = 0,
            LOADING = 1,
            DONE = 2,
            FAILED = 3
        };

        GlobeTile( std::int64_t p_x, std::int64_t p_y, int p_level, GlobeTile *p_parent );
        ~GlobeTile();

        std::int64_t x = 0;
        std::int64_t y = 0;
        int level = 0;
        GlobeTile *parent = nullptr;

        /// The imagery rectangle (Web Mercator, so up to +/-85.05 degrees).
        math::TileRectangle rectangle{};

        /// The rectangle the *mesh* spans: outermost rows stretch to the poles so the
        /// caps are not holes. Equivalent to the reference tileGeometryRectangle.
        math::TileRectangle geometry_rectangle() const;

        // The four children, created lazily on first access (nw, ne, sw, se order).
        GlobeTile *child( int index );
        bool has_children() const { return nw != nullptr; }
        int child_count() const;

        // ---- load state ----

        LoadState state = LoadState::START;
        bool renderable = false;

        /// Wall-clock seconds ( Godot's Time::get_ticks_msec ) of the last failure, for
        /// the reference's 15-second retry cool-down.
        double fail_time = 0.0;

        bool needs_loading() const { return state < LoadState::DONE; }

        // ---- resources, owned by the layer ----

        godot::Ref<godot::ImageTexture> texture;
        godot::MeshInstance3D *mesh = nullptr;

        /// The tile's own centre in Z-up ECEF metres, as chosen when the mesh was built.
        ///
        /// The mesh vertices are authored *relative to this point* (RTC: relative to centre),
        /// which is what keeps them small - a level-16 tile is a few hundred metres across, so
        /// its vertices stay in the micro-metre float32 range instead of the 0.4 m range they
        /// would have if they were absolute ECEF. `mesh`'s own position carries this offset,
        /// and because the offset is re-derived from this ECEF point every time the frame
        /// origin moves, **re-basing the origin costs one transform write per live tile and no
        /// vertex work at all.**
        math::Vec3 rtc_center_ecef{ 0.0 };

        /// Tile-local mercator UVs, aligned with the mesh vertices (including skirt ring).
        /// Stored so appearance can be re-mapped to an ancestor texture without rebuilding.
        godot::PackedVector2Array base_uvs;

        /// The ancestor whose texture is currently displayed, or null for the flat
        /// placeholder colour. Used to skip redundant UV rewrites.
        GlobeTile *appearance_source = nullptr;
        int texture_version = -1;

        // ---- selection bookkeeping ----

        double distance = 0.0;
        double load_priority = 0.0;
        int selection_frame = -1;

        /// Bounding sphere in ECEF (Y-up) metres, computed lazily from nine surface
        /// samples. `occludee` is the horizon-culling point in *scaled* space.
        bool bounds_computed = false;
        math::Vec3 bounds_center{ 0.0 };
        double bounds_radius = 0.0;
        math::Vec3 occludee_point{ 0.0 };
        bool occludee_valid = false;

        void compute_bounds( const GlobeFrame &frame );

        // ---- LRU bookkeeping (TileReplacementQueue) ----

        GlobeTile *replacement_previous = nullptr;
        GlobeTile *replacement_next = nullptr;
        bool in_replacement_queue = false;

        /// Drops GPU resources but keeps the tree, so the selection can re-request the
        /// tile later. Equivalent to the reference freeResources.
        void free_resources();

    private:
        GlobeTile *nw = nullptr;
        GlobeTile *ne = nullptr;
        GlobeTile *sw = nullptr;
        GlobeTile *se = nullptr;
    };

    /// LRU list over live tiles. Ported from TileReplacementQueue.js (via QuadtreeTile.ts).
    class TileReplacementQueue
    {
    public:
        GlobeTile *head = nullptr;
        GlobeTile *tail = nullptr;
        int count = 0;

        void mark_start_of_render_frame();
        void mark_tile_rendered( GlobeTile *tile );
        void remove( GlobeTile *tile );

        /// Drops the least-recently used tiles beyond `maximum_tiles`. `is_protected`
        /// lets the caller keep subtrees that are on screen this frame; `on_trim` is
        /// invoked for every tile whose resources are released.
        int trim_tiles( int maximum_tiles, const std::function<bool( GlobeTile * )> &is_protected,
                        const std::function<void( GlobeTile * )> &on_trim );

        void clear();

    private:
        GlobeTile *last_before_start_of_frame = nullptr;

        void unlink( GlobeTile *tile );
    };

} // namespace tiles3d

#endif
