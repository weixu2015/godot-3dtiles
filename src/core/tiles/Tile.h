// SPDX-License-Identifier: Unlicense
//
// The tile tree: one node per 3D Tiles tile, plus the region-to-local-frame conversion
// that has to happen once before any traversal.
//
// Ported from the Tile class, getTileBvRadius and convertRegionBoundingVolumes in
// threeDTiles/index.ts.

#ifndef TILES3D_CORE_TILES_TILE_H
#define TILES3D_CORE_TILES_TILE_H

#include "math/BoundingVolume.h"
#include "math/Mat4.h"
#include "math/Types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tiles3d::core
{
    /// Refinement strategy. A tile that omits it inherits the parent's, which the 3D Tiles
    /// specification requires and which is easy to get wrong - see parseTilesetJson.
    enum class RefineMode
    {
        Add = 0,
        Replace = 1,
    };

    /// Content lifecycle.
    ///
    /// The reference implementation tracks this with four booleans (loaded, loading,
    /// contentReady, loadFailed); this enum carries the same information plus the
    /// PROCESSING stage that separates "bytes arrived" from "ready to render", matching
    /// Cesium's Cesium3DTileContentState.
    enum class ContentState
    {
        /// Never requested.
        Unloaded = 0,
        /// Download or parse in flight.
        Loading = 1,
        /// Bytes are in and need work on the main thread (glTF to Godot nodes, texture
        /// upload). Traversal must not block on this state.
        Processing = 2,
        /// Renderable.
        Ready = 3,
        /// Loaded but stale, awaiting replacement.
        Expired = 4,
        /// Permanently failed, retry budget exhausted.
        Failed = 5,
    };

    /// A tile's content declaration. The content bounding volume may differ from the
    /// tile's own bounding volume.
    struct TileContent
    {
        std::string uri;
        std::optional<math::BoundingVolume> boundingVolume;
    };

    /// Implicit tiling declaration (3D Tiles 1.1 core property, or the 1.0
    /// 3DTILES_implicit_tiling extension).
    struct ImplicitTiling
    {
        enum class SubdivisionScheme
        {
            Quadtree = 0,
            Octree = 1,
        };

        SubdivisionScheme subdivisionScheme = SubdivisionScheme::Quadtree;
        int subtreeLevels = 0;
        int maximumLevel = 0;

        /// URI template for subtree files, e.g. "subtrees/{level}/{x}/{y}.subtree".
        std::string subtreeUriTemplate;

        /// URI template for tile content, e.g. "content/{level}/{x}/{y}.b3dm".
        ///
        /// The specification puts this at tile level next to the implicit tiling
        /// declaration, not inside it, so the parser moves it here where the implicit tile
        /// manager expects it.
        std::optional<std::string> contentUriTemplate;

        /// Branching factor: 4 for a quadtree, 8 for an octree.
        int branchingFactor() const
        {
            return subdivisionScheme == SubdivisionScheme::Octree ? 8 : 4;
        }
    };

    /// Morton coordinates of a tile within its implicit tile set.
    struct ImplicitCoordinates
    {
        int level = 0;
        int x = 0;
        int y = 0;
        int z = 0;
    };

    /// One node of the tile tree.
    ///
    /// Children hang off unique_ptr so that a Tile's address stays put: the scheduler keeps
    /// raw Tile pointers in its load queue, its LRU cache and its per-frame visible list.
    ///
    /// The type is deliberately engine agnostic. It knows nothing about meshes, nodes or
    /// textures; turning content into renderer objects is the Godot layer's job. The only
    /// link to content is `contentState`, which the Godot layer drives.
    class Tile
    {
    public:
        Tile();
        ~Tile();

        Tile( const Tile & ) = delete;
        Tile &operator=( const Tile & ) = delete;

        /// Pre-order index assigned at parse time. Deterministic across implementations,
        /// which is what makes tools/sched_trace able to diff a C++ run against the
        /// TypeScript reference.
        std::size_t id = 0;

        /// Depth in the tile tree; the root is 0. Implicit tiles carry their Morton level.
        int depth = 0;

        /// Local -> parent transform. Identity unless the tileset JSON supplies one.
        math::Mat4 transform = math::identity();

        /// Geometric error in metres. Drives level of detail refinement.
        double geometricError = 0.0;

        RefineMode refine = RefineMode::Add;

        /// Absent only for tiles whose children carry the volumes. Phase 2 accepts tiles
        /// without one; traversal will treat them as always-visible.
        std::optional<math::BoundingVolume> boundingVolume;

        /// Present when the tile has directly loadable content. A URI template used by
        /// implicit tiling is *not* direct content; see parseTilesetJson.
        std::optional<TileContent> content;

        std::vector<std::unique_ptr<Tile>> children;

        std::optional<ImplicitTiling> implicitTiling;
        std::optional<ImplicitCoordinates> implicitCoordinates;

        /// True for a container tile created by an external tileset. Its empty content must
        /// not stop refinement.
        bool isExternalTileset = false;

        /// Accumulated world matrix from the last traversal. Unset before the first one.
        std::optional<math::Mat4> worldMatrix;

        ContentState contentState = ContentState::Unloaded;

        /// Bytes downloaded for this tile's content, for the memory budget.
        std::size_t contentBytes = 0;

        std::uint32_t loadErrorCount = 0;

        /// Radius of `boundingVolume` in the tile's own frame.
        ///
        /// Memoised because a region radius has to project eight corners onto the
        /// ellipsoid, which is far too expensive to repeat per frame.
        double boundingVolumeRadius() const;

        /// Drops the memoised radius. Must be called after replacing `boundingVolume`,
        /// otherwise a stale radius silently skews culling and request culling.
        void invalidateBoundingVolumeRadius() const;

        static const char *contentStateName( ContentState state );

    private:
        mutable std::optional<double> cachedBoundingVolumeRadius;
    };

    /// Converts every `region` bounding volume in the subtree into a box expressed in the
    /// tile's local frame.
    ///
    /// A region is EPSG:4979 absolute geodetic coordinates and is explicitly *not*
    /// transformed by the tile's transform chain (Cesium: "A region bounding volume is not
    /// transformed by the transform in the tileset JSON"), so its position in the render
    /// frame is always modelMatrix * ECEF. Converting once up front means every downstream
    /// consumer - frustum culling, screen space error, debug wireframes, framing the
    /// camera - can simply apply the traversal's worldMatrix and get the right answer.
    ///
    ///     box_tileLocal = chain^-1 * modelMatrix * OBB(ecef)
    ///     chain         = parentChain * tile.transform * ...
    ///
    /// @param modelMatrix ECEF to render frame mapping.
    /// @param parentChain world matrix of the subtree root's parent. Pass modelMatrix for
    ///        a main tileset; for an external tileset pass the container tile's world
    ///        matrix.
    void convertRegionBoundingVolumes( Tile &tile, const math::Mat4 &modelMatrix,
                                       const math::Mat4 &parentChain );

} // namespace tiles3d::core

#endif
