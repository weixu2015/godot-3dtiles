// SPDX-License-Identifier: Unlicense

#include "tiles/Subtree.h"

#include "math/BoundingVolume.h"
#include "math/GeoMath.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <memory>
#include <utility>

namespace tiles3d::core
{
    namespace
    {
        std::uint32_t readU32( const std::uint8_t *p )
        {
            return static_cast<std::uint32_t>( p[0] ) | ( static_cast<std::uint32_t>( p[1] ) << 8 ) |
                   ( static_cast<std::uint32_t>( p[2] ) << 16 ) |
                   ( static_cast<std::uint32_t>( p[3] ) << 24 );
        }

        std::uint64_t readU64( const std::uint8_t *p )
        {
            std::uint64_t value = 0;
            for ( int i = 7; i >= 0; --i )
            {
                value = ( value << 8 ) | static_cast<std::uint64_t>( p[i] );
            }
            return value;
        }

        /// Numeric JSON field, falling back when absent or not a number. nlohmann's numeric
        /// conversions are exception free, but going through double keeps this uniform with
        /// the tileset parser.
        double readField( const nlohmann::json &object, const char *key, double fallback )
        {
            if ( !object.is_object() )
            {
                return fallback;
            }
            const auto it = object.find( key );
            if ( it == object.end() || !it->is_number() )
            {
                return fallback;
            }
            return it->get<double>();
        }

        /// Decodes one availability entry. Handles `constant`, `bitstream` and the pre-1.1
        /// `bufferView` spelling; anything else is Absent (all bits false).
        Availability parseAvailability( const nlohmann::json &value, const nlohmann::json &bufferViews,
                                        const std::uint8_t *binary, std::size_t binarySize )
        {
            Availability out;

            if ( !value.is_object() )
            {
                return out;
            }

            if ( const auto it = value.find( "constant" );
                 it != value.end() && it->is_number() )
            {
                out.kind = Availability::Kind::Constant;
                out.constantValue = it->get<double>() != 0.0;
                return out;
            }

            const nlohmann::json *bitstream = nullptr;
            if ( const auto bitstreamIt = value.find( "bitstream" );
                 bitstreamIt != value.end() && bitstreamIt->is_number() )
            {
                bitstream = &( *bitstreamIt );
            }
            else if ( const auto bufferViewIt = value.find( "bufferView" );
                      bufferViewIt != value.end() && bufferViewIt->is_number() )
            {
                bitstream = &( *bufferViewIt );
            }

            if ( bitstream == nullptr || !bufferViews.is_array() )
            {
                return out;
            }

            const auto viewIndex = static_cast<std::size_t>( bitstream->get<double>() );
            if ( viewIndex >= bufferViews.size() )
            {
                return out;
            }

            const nlohmann::json &view = bufferViews[viewIndex];
            const auto offset = static_cast<std::size_t>( readField( view, "byteOffset", 0.0 ) );
            const auto length = static_cast<std::size_t>( readField( view, "byteLength", 0.0 ) );
            if ( offset > binarySize || length > binarySize - offset )
            {
                return out;
            }

            out.kind = Availability::Kind::Bitstream;
            out.bytes.assign( binary + offset, binary + offset + length );
            return out;
        }

        void replaceAll( std::string &text, const std::string &needle, const std::string &value )
        {
            std::size_t position = 0;
            while ( ( position = text.find( needle, position ) ) != std::string::npos )
            {
                text.replace( position, needle.size(), value );
                position += value.size();
            }
        }
    } // namespace

    bool parseSubtree( const std::uint8_t *data, std::size_t size, SubtreeData &out, std::string &error )
    {
        if ( data == nullptr || size < 24 || std::memcmp( data, "subt", 4 ) != 0 )
        {
            error = "subtree: bad magic (expected 'subt')";
            return false;
        }

        const std::uint32_t version = readU32( data + 4 );
        if ( version != 1 )
        {
            error = "subtree: unsupported version " + std::to_string( version );
            return false;
        }

        const std::uint64_t jsonLength = readU64( data + 8 );
        const std::uint64_t binaryLength = readU64( data + 16 );
        if ( jsonLength > size || binaryLength > size ||
             24 + jsonLength + binaryLength > size )
        {
            error = "subtree: header lengths exceed the file";
            return false;
        }

        const std::uint8_t *jsonBegin = data + 24;
        const std::uint8_t *jsonEnd = jsonBegin + jsonLength;
        const std::uint8_t *binary = jsonEnd;
        const auto binarySize = static_cast<std::size_t>( binaryLength );

        nlohmann::json json = nlohmann::json::parse( reinterpret_cast<const char *>( jsonBegin ),
                                                     reinterpret_cast<const char *>( jsonEnd ),
                                                     nullptr, false );
        if ( json.is_discarded() || !json.is_object() )
        {
            error = "subtree: JSON header is not valid";
            return false;
        }

        const nlohmann::json bufferViews =
            json.contains( "bufferViews" ) ? json["bufferViews"] : nlohmann::json();

        out = SubtreeData{};
        out.tileAvailability = parseAvailability(
            json.contains( "tileAvailability" ) ? json["tileAvailability"] : nlohmann::json(),
            bufferViews, binary, binarySize );

        if ( const auto it = json.find( "contentAvailability" ); it != json.end() )
        {
            if ( it->is_array() )
            {
                out.contentAvailability.reserve( it->size() );
                for ( const nlohmann::json &entry : *it )
                {
                    out.contentAvailability.push_back(
                        parseAvailability( entry, bufferViews, binary, binarySize ) );
                }
            }
            else if ( it->is_object() )
            {
                out.contentAvailability.push_back(
                    parseAvailability( *it, bufferViews, binary, binarySize ) );
            }
        }

        out.childSubtreeAvailability = parseAvailability(
            json.contains( "childSubtreeAvailability" ) ? json["childSubtreeAvailability"]
                                                        : nlohmann::json(),
            bufferViews, binary, binarySize );

        return true;
    }

    std::string replaceTemplate( const std::string &uriTemplate, int level, int x, int y, int z )
    {
        if ( uriTemplate.find( '{' ) == std::string::npos )
        {
            return uriTemplate;
        }

        std::string result = uriTemplate;
        replaceAll( result, "{level}", std::to_string( level ) );
        replaceAll( result, "{x}", std::to_string( x ) );
        replaceAll( result, "{y}", std::to_string( y ) );
        replaceAll( result, "{z}", std::to_string( z ) );
        return result;
    }

    void expandImplicitSubtree( Tile &parent, const SubtreeData &subtree, const ImplicitTiling &decl,
                                std::size_t &nextId )
    {
        if ( !parent.implicitCoordinates.has_value() || !parent.boundingVolume.has_value() )
        {
            return;
        }

        const bool isOctree = decl.subdivisionScheme == ImplicitTiling::SubdivisionScheme::Octree;
        const int branchingFactor = decl.branchingFactor();
        const int subtreeLevels = decl.subtreeLevels;
        const bool hasContentTemplate = decl.contentUriTemplate.has_value();

        // Row of tiles at the current subtree level, indexed by Morton index. Unavailable
        // tiles stay in the row as nullptr so the parent index (morton / bf) keeps lining up.
        std::vector<Tile *> parentRow;
        parentRow.push_back( &parent );

        for ( int level = 1; level < subtreeLevels; ++level )
        {
            const std::int64_t levelOffset = math::levelOffset( branchingFactor, level );
            const auto childCount =
                static_cast<std::size_t>( branchingFactor ) * parentRow.size();
            std::vector<Tile *> currentRow( childCount, nullptr );

            for ( std::size_t childMorton = 0; childMorton < childCount; ++childMorton )
            {
                const auto bitIndex = levelOffset + static_cast<std::int64_t>( childMorton );
                if ( !subtree.tileAvailable( bitIndex ) )
                {
                    continue;
                }

                Tile *parentTile = parentRow[childMorton / static_cast<std::size_t>( branchingFactor )];
                if ( parentTile == nullptr )
                {
                    continue;
                }

                const int childIndex = static_cast<int>( childMorton % static_cast<std::size_t>( branchingFactor ) );
                const int ix = childIndex & 1;
                const int iy = ( childIndex >> 1 ) & 1;
                const int iz = isOctree ? ( childIndex >> 2 ) & 1 : 0;

                const ImplicitCoordinates &parentCoords = *parentTile->implicitCoordinates;

                auto child = std::make_unique<Tile>();
                child->id = nextId++;
                child->depth = parentTile->depth + 1;

                ImplicitCoordinates coords;
                coords.level = parentCoords.level + 1;
                coords.x = ( parentCoords.x << 1 ) | ix;
                coords.y = ( parentCoords.y << 1 ) | iy;
                coords.z = isOctree ? ( ( parentCoords.z << 1 ) | iz ) : parentCoords.z;
                child->implicitCoordinates = coords;

                child->refine = parent.refine;
                child->geometricError = parentTile->geometricError / 2.0;
                child->boundingVolume =
                    math::subdivideBox( *parentTile->boundingVolume, childIndex, isOctree );

                if ( hasContentTemplate && subtree.contentAvailable( bitIndex, 0 ) )
                {
                    TileContent content;
                    content.uri = replaceTemplate( *decl.contentUriTemplate, coords.level, coords.x,
                                                   coords.y, coords.z );
                    child->content = std::move( content );
                }

                Tile *raw = child.get();
                parentTile->children.push_back( std::move( child ) );
                currentRow[childMorton] = raw;
            }

            parentRow = std::move( currentRow );
        }

        // Bottom row: one placeholder per available child subtree, carrying the implicit
        // declaration so the traversal expands the next subtree when it reaches them.
        for ( std::size_t i = 0; i < parentRow.size(); ++i )
        {
            Tile *leaf = parentRow[i];
            if ( leaf == nullptr )
            {
                continue;
            }

            for ( int j = 0; j < branchingFactor; ++j )
            {
                const auto childSubtreeIndex =
                    static_cast<std::int64_t>( i ) * branchingFactor + j;
                if ( !subtree.childSubtreeAvailable( childSubtreeIndex ) )
                {
                    continue;
                }

                const int ix = j & 1;
                const int iy = ( j >> 1 ) & 1;
                const int iz = isOctree ? ( j >> 2 ) & 1 : 0;

                const ImplicitCoordinates &leafCoords = *leaf->implicitCoordinates;

                auto placeholder = std::make_unique<Tile>();
                placeholder->id = nextId++;
                placeholder->depth = leaf->depth + 1;

                ImplicitCoordinates coords;
                coords.level = leafCoords.level + 1;
                coords.x = ( leafCoords.x << 1 ) | ix;
                coords.y = ( leafCoords.y << 1 ) | iy;
                coords.z = isOctree ? ( ( leafCoords.z << 1 ) | iz ) : leafCoords.z;
                placeholder->implicitCoordinates = coords;

                placeholder->refine = parent.refine;
                placeholder->geometricError = leaf->geometricError / 2.0;
                placeholder->boundingVolume =
                    math::subdivideBox( *leaf->boundingVolume, j, isOctree );

                // A placeholder has no content of its own; its subtree decides that. What it
                // must carry is the declaration, so the traversal can request the next
                // subtree instead of treating it as a childless leaf.
                placeholder->implicitTiling = decl;

                leaf->children.push_back( std::move( placeholder ) );
            }
        }
    }

} // namespace tiles3d::core
