// SPDX-License-Identifier: Unlicense
//
// tileset.json parsing: document -> tile tree.
//
// Ported from parseBoundingVolume, parseTilesetJson and parseTile in
// threeDTiles/index.ts.

#ifndef TILES3D_CORE_TILES_TILESETJSON_H
#define TILES3D_CORE_TILES_TILESETJSON_H

#include "tiles/Tile.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>

namespace tiles3d::core
{
    /// Parses one bounding volume object: `{"box":[12]}`, `{"region":[6]}` or
    /// `{"sphere":[4]}`.
    ///
    /// Returns nullopt for anything unrecognised or with the wrong element count. The
    /// reference implementation does not validate the length, which turns a malformed
    /// volume into a silent NaN much later; failing here is far cheaper to diagnose.
    std::optional<math::BoundingVolume> parseBoundingVolume( const nlohmann::json &value );

    /// The up axis of a tileset's glTF *content*, as declared by `asset.gltfUpAxis`
    /// (3D Tiles 1.1, and the `gltfUpAxis` property of the 1.0 spec).
    ///
    /// The tile tree itself is always Z-up; this only says which axis the content is
    /// authored in, and therefore whether the content needs an axis correction before it
    /// lines up with its bounding volume.
    enum class ModelUpAxis
    {
        X = 0,
        Y = 1,
        Z = 2,
    };

    /// Resolves the content up axis from a tileset document's `asset.gltfUpAxis`.
    ///
    /// Reading the declared axis rather than assuming Y is what keeps datasets that are
    /// *already* Z-up from being rotated. The correction is a rotation about the glTF
    /// origin, so applying it to content that sits far from that origin - a tile whose
    /// vertices carry full tile-frame coordinates, e.g. (38722, 119689, 119) - throws the
    /// geometry hundreds of thousands of units away, outside the camera's far plane, and
    /// the tileset renders as nothing at all even though the scheduler reports it as
    /// loaded. That is the taiwan regression: it declares `gltfUpAxis: "Z"` and must not
    /// be rotated.
    ///
    /// An absent, empty or unrecognised value falls back to `fallback` (Y), which is what
    /// every 1.0 dataset that never declared the field expects.
    ModelUpAxis resolveModelUpAxis( const nlohmann::json &tilesetJson,
                                    ModelUpAxis fallback = ModelUpAxis::Y );

    /// Outcome of parsing a tileset.json.
    ///
    /// The core layer does not throw (see BoundingVolume.h for why), so failures are
    /// reported through `error` instead.
    struct TilesetParseResult
    {
        /// Null when parsing failed.
        std::unique_ptr<Tile> root;

        /// The tileset's asset version, e.g. "1.0" or "1.1". Kept because content loading
        /// has to tell a tileset document apart from a glTF document.
        std::string assetVersion;

        /// Tileset-level geometric error, as declared.
        double geometricError = 0.0;

        /// Content up axis, resolved from `asset.gltfUpAxis`. Defaults to Y when the
        /// document omits it or declares an unrecognised value, so every 1.0 dataset keeps
        /// its existing behaviour.
        ModelUpAxis modelUpAxis = ModelUpAxis::Y;

        /// Empty on success, otherwise a human readable reason.
        std::string error;

        explicit operator bool() const
        {
            return root != nullptr;
        }
    };

    /// Builds the tile tree from a parsed tileset.json document.
    ///
    /// Two behaviours matter here and are easy to get wrong:
    ///
    /// 1. **`refine` is inherited from the parent when a tile omits it.** Defaulting to
    ///    ADD instead silently turns a REPLACE dataset into an ADD one, so every level
    ///    renders and coarse parents cover the fine children. Photogrammetry tilesets
    ///    normally declare REPLACE only on the root, which is exactly the case that
    ///    breaks.
    /// 2. **A content URI containing a `{...}` placeholder on a tile that declares
    ///    implicit tiling is a template, not content.** It is moved into
    ///    ImplicitTiling::contentUriTemplate and Tile::content is left empty, because the
    ///    template describes how to build child URIs and must never be fetched directly.
    ///
    /// @param rootRefine refinement mode assumed for the root when it omits one. The
    ///        reference default is ADD.
    TilesetParseResult parseTilesetJson( const nlohmann::json &json,
                                        RefineMode rootRefine = RefineMode::Add );

} // namespace tiles3d::core

#endif
