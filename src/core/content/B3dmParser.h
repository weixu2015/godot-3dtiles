// SPDX-License-Identifier: Unlicense
//
// b3dm parsing: the Batched 3D Model container.
//
// A b3dm file is a 28 byte header followed by a feature table, a batch table and then a
// binary glTF. Only what the scheduler and the renderer actually need is extracted here;
// the batch table is handed on as raw JSON text so this header stays free of the JSON
// dependency.
//
// Ported from B3dmParser in threeDTiles/index.ts.

#ifndef TILES3D_CORE_CONTENT_B3DMPARSER_H
#define TILES3D_CORE_CONTENT_B3DMPARSER_H

#include "math/Types.h"

#include <cstddef>
#include <optional>
#include <string>

namespace tiles3d::core
{
    /// b3dm fixed header size, bytes.
    inline constexpr std::size_t kB3dmHeaderSize = 28;

    struct B3dmParseResult
    {
        /// The embedded binary glTF, as an offset into the buffer that was parsed. Returned
        /// as an offset rather than a copy so a large tile is not duplicated in memory; the
        /// caller slices when it needs a contiguous copy (Godot wants a PackedByteArray).
        std::size_t glbOffset = 0;
        std::size_t glbLength = 0;

        /// Feature table BATCH_LENGTH, 0 when absent.
        std::size_t batchLength = 0;

        /// Feature table RTC_CENTER, when present. This is the model's position within the
        /// tile's coordinate system, so it has to be applied separately from the tile
        /// transform (and is deliberately NOT affected by the up-axis correction).
        std::optional<math::Vec3> rtcCenter;

        /// Raw batch table JSON text; empty when the tile has no batch table.
        std::string batchTableJson;

        /// Empty on success, otherwise a human readable reason.
        std::string error;

        explicit operator bool() const
        {
            return error.empty();
        }
    };

    /// Parses a b3dm from a caller owned buffer.
    ///
    /// The core layer does not throw, so failures come back through `error`.
    ///
    /// @param data start of the file.
    /// @param size total size in bytes.
    B3dmParseResult parseB3dm( const std::byte *data, std::size_t size );

} // namespace tiles3d::core

#endif
