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
