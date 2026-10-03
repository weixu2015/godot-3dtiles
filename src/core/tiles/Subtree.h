// SPDX-License-Identifier: Unlicense
//
// 3D Tiles 1.1 implicit tiling: the `.subtree` binary format and the expansion of a subtree
// into concrete child tiles.
//
// Ported from the reference implementation's SubtreeReader / BitStream /
// ImplicitTileManager.expandSubtree (threeDTiles/index.ts). Keeping this in the kernel -
// rather than the Godot layer - means the availability bitstream semantics and the Morton
// row walk are unit testable without an engine; only the file IO stays engine side.

#ifndef TILES3D_CORE_TILES_SUBTREE_H
#define TILES3D_CORE_TILES_SUBTREE_H

#include "tiles/Tile.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tiles3d::core
{
    /// One availability bitstream from a subtree file.
    ///
    /// The specification allows three shapes and the reference accepts all three: a
    /// `constant` (every bit equal), a real `bitstream` addressed through `bufferViews`, and
    /// - for compatibility with pre-1.1 files - the old `bufferView` key. A stream that is
    /// absent entirely means "nothing available".
    struct Availability
    {
        enum class Kind
        {
            Absent = 0,
            Constant = 1,
            Bitstream = 2,
        };

        Kind kind = Kind::Absent;

        /// Meaningful when `kind == Constant`.
        bool constantValue = false;

        /// Raw bytes when `kind == Bitstream`, LSB-first as the specification stores them.
        std::vector<std::uint8_t> bytes;

        /// Bit `index`, or false when out of range or absent.
        bool get( std::int64_t index ) const
        {
            if ( kind == Kind::Constant )
            {
                return constantValue;
            }
            if ( kind != Kind::Bitstream || index < 0 )
            {
                return false;
            }

            const std::int64_t byteIndex = index >> 3;
            if ( static_cast<std::size_t>( byteIndex ) >= bytes.size() )
            {
                return false;
            }
            return ( ( bytes[static_cast<std::size_t>( byteIndex )] >> ( index & 7 ) ) & 1 ) != 0;
        }
    };

    /// Decoded contents of a `.subtree` file.
    struct SubtreeData
    {
        /// One bit per tile of the subtree, in Morton order across all its levels.
        Availability tileAvailability;

        /// One stream per content (the reference only ever consults index 0).
        std::vector<Availability> contentAvailability;

        /// One bit per potential child subtree, i.e. per tile on the subtree's bottom row.
        Availability childSubtreeAvailability;

        bool tileAvailable( std::int64_t index ) const
        {
            return tileAvailability.get( index );
        }

        bool contentAvailable( std::int64_t index, std::size_t contentIndex = 0 ) const
        {
            return contentIndex < contentAvailability.size()
                       ? contentAvailability[contentIndex].get( index )
                       : false;
        }

        bool childSubtreeAvailable( std::int64_t index ) const
        {
            return childSubtreeAvailability.get( index );
        }
    };

    /// Decodes a `.subtree` payload.
    ///
    /// Layout, all little endian: `"subt"(4) | version u32(4) | jsonByteLength u64(8) |
    /// binaryByteLength u64(8) | JSON | binary`. Availability bitstreams are one bit per
    /// tile, LSB-first within each byte, indexed in Morton order inside the subtree.
    ///
    /// Returns false and fills `error` on a malformed file. This layer does not throw; see
    /// BoundingVolume.h for why.
    bool parseSubtree( const std::uint8_t *data, std::size_t size, SubtreeData &out,
                       std::string &error );

    /// Instantiates the `{level}` / `{x}` / `{y}` / `{z}` placeholders of a URI template.
    /// A template without placeholders is returned unchanged.
    std::string replaceTemplate( const std::string &uriTemplate, int level, int x, int y, int z );

    /// Expands `parent`'s subtree declaration into concrete child tiles.
    ///
    /// Mirrors the reference `ImplicitTileManager.expandSubtree`: it walks the subtree's
    /// level rows, creating a child for every tile the availability bitstream marks present,
    /// halving the geometric error and subdividing the parent's box per child index. Tiles on
    /// the subtree's bottom row additionally get a placeholder child for every available
    /// child subtree; those placeholders carry `implicitTiling` so the next subtree is
    /// decoded when the traversal reaches them.
    ///
    /// `parent` must carry `implicitCoordinates`, a bounding volume and a geometric error -
    /// expansion subdivides all three. Content URIs are instantiated from the declaration's
    /// template but left relative, so the caller's own base-directory resolution applies.
    /// `nextId` continues the pre-order numbering so tiles materialised later do not collide
    /// with the parsed tree.
    void expandImplicitSubtree( Tile &parent, const SubtreeData &subtree, const ImplicitTiling &decl,
                                std::size_t &nextId );

} // namespace tiles3d::core

#endif
