// SPDX-License-Identifier: Unlicense

#include "tiles/TilesetJson.h"

#include <array>
#include <cstddef>
#include <utility>

namespace tiles3d::core
{
    namespace
    {
        /// Reads a numeric field, falling back when absent or of the wrong type.
        ///
        /// Goes through double rather than the integer accessor so that a value outside
        /// int's range cannot throw; nlohmann's numeric conversions are exception free.
        double readReal( const nlohmann::json &object, const char *key, double fallback )
        {
            const auto it = object.find( key );
            if ( it == object.end() || !it->is_number() )
            {
                return fallback;
            }
            return it->get<double>();
        }

        int readInt( const nlohmann::json &object, const char *key, int fallback )
        {
            return static_cast<int>( readReal( object, key, static_cast<double>( fallback ) ) );
        }

        std::string readString( const nlohmann::json &object, const char *key )
        {
            const auto it = object.find( key );
            if ( it == object.end() || !it->is_string() )
            {
                return {};
            }
            return it->get<std::string>();
        }

        /// Reads exactly N numbers, or nothing.
        ///
        /// Validating the count here is a deliberate improvement over the reference, which
        /// accepts any array length and produces NaN geometry later.
        template <std::size_t N>
        std::optional<std::array<double, N>> readNumbers( const nlohmann::json &value )
        {
            if ( !value.is_array() || value.size() != N )
            {
                return std::nullopt;
            }

            std::array<double, N> numbers{};
            for ( std::size_t index = 0; index < N; ++index )
            {
                const nlohmann::json &element = value[index];
                if ( !element.is_number() )
                {
                    return std::nullopt;
                }
                numbers[index] = element.get<double>();
            }
            return numbers;
        }

        ImplicitTiling parseImplicitTiling( const nlohmann::json &value )
        {
            ImplicitTiling tiling;

            if ( readString( value, "subdivisionScheme" ) == "OCTREE" )
            {
                tiling.subdivisionScheme = ImplicitTiling::SubdivisionScheme::Octree;
            }

            tiling.subtreeLevels = readInt( value, "subtreeLevels", 0 );
            tiling.maximumLevel = readInt( value, "maximumLevel", 0 );

            const auto subtreesIt = value.find( "subtrees" );
            if ( subtreesIt != value.end() && subtreesIt->is_object() )
            {
                tiling.subtreeUriTemplate = readString( *subtreesIt, "uri" );
            }

            // 3D Tiles 1.1 allows the content template to sit inside the implicit tiling
            // object itself.
            const auto contentIt = value.find( "content" );
            if ( contentIt != value.end() && contentIt->is_object() )
            {
                const std::string uri = readString( *contentIt, "uri" );
                if ( !uri.empty() )
                {
                    tiling.contentUriTemplate = uri;
                }
            }

            return tiling;
        }

        /// Recursive worker. Returns null and fills `error` on failure.
        std::unique_ptr<Tile> parseTile( const nlohmann::json &json, int depth,
                                        RefineMode parentRefine, std::size_t &nextId,
                                        std::string &error )
        {
            if ( !json.is_object() )
            {
                error = "tile: expected a JSON object";
                return nullptr;
            }

            auto tile = std::make_unique<Tile>();
            tile->id = nextId++;
            tile->depth = depth;

            const auto volumeIt = json.find( "boundingVolume" );
            if ( volumeIt == json.end() )
            {
                error = "tile: missing boundingVolume";
                return nullptr;
            }

            std::optional<math::BoundingVolume> volume = parseBoundingVolume( *volumeIt );
            if ( !volume.has_value() )
            {
                error = "tile: boundingVolume is malformed (expected box[12], region[6] or sphere[4])";
                return nullptr;
            }
            tile->boundingVolume = *volume;

            tile->geometricError = readReal( json, "geometricError", 0.0 );

            // 3D Tiles requires `refine` only on the root; children inherit. Anything that
            // is not exactly "ADD" or "REPLACE" also inherits rather than being taken
            // verbatim, which is stricter than the reference and fails safe.
            RefineMode refine = parentRefine;
            const std::string refineText = readString( json, "refine" );
            if ( refineText == "REPLACE" )
            {
                refine = RefineMode::Replace;
            }
            else if ( refineText == "ADD" )
            {
                refine = RefineMode::Add;
            }
            tile->refine = refine;

            if ( const auto it = json.find( "transform" );
                 it != json.end() )
            {
                if ( const auto numbers = readNumbers<16>( *it ); numbers.has_value() )
                {
                    tile->transform = math::fromColumnMajor( numbers->data() );
                }
            }

            // Implicit tiling: the 1.1 core property takes precedence, then the 1.0
            // extension.
            const nlohmann::json *implicitJson = nullptr;
            if ( const auto it = json.find( "implicitTiling" );
                 it != json.end() && it->is_object() )
            {
                implicitJson = &( *it );
            }
            else if ( const auto extensionsIt = json.find( "extensions" );
                      extensionsIt != json.end() && extensionsIt->is_object() )
            {
                if ( const auto implicitIt = extensionsIt->find( "3DTILES_implicit_tiling" );
                     implicitIt != extensionsIt->end() && implicitIt->is_object() )
                {
                    implicitJson = &( *implicitIt );
                }
            }

            if ( implicitJson != nullptr )
            {
                tile->implicitTiling = parseImplicitTiling( *implicitJson );
                tile->implicitCoordinates = ImplicitCoordinates{};
            }

            // Content, and the template rule.
            std::string contentUri;
            std::optional<math::BoundingVolume> contentVolume;

            if ( const auto contentIt = json.find( "content" );
                 contentIt != json.end() && contentIt->is_object() )
            {
                contentUri = readString( *contentIt, "uri" );

                if ( const auto bvIt = contentIt->find( "boundingVolume" ); bvIt != contentIt->end() )
                {
                    contentVolume = parseBoundingVolume( *bvIt );
                }
            }

            // Reference rule, kept verbatim: the URI counts as a template when it contains
            // an opening brace. Requiring the closing brace too would be "safer" but would
            // diverge on malformed input, and behaviour parity is worth more here.
            const bool uriIsTemplate = contentUri.find( '{' ) != std::string::npos;
            const bool contentIsImplicitTemplate =
                uriIsTemplate && tile->implicitTiling.has_value();

            if ( contentIsImplicitTemplate && !tile->implicitTiling->contentUriTemplate.has_value() )
            {
                // Both 1.0 and 1.1 put the content template at tile level; move it to where
                // the implicit tile manager looks for it.
                tile->implicitTiling->contentUriTemplate = contentUri;
            }

            if ( !contentUri.empty() && !contentIsImplicitTemplate )
            {
                TileContent content;
                content.uri = contentUri;
                content.boundingVolume = contentVolume;
                tile->content = std::move( content );
            }

            if ( const auto childrenIt = json.find( "children" );
                 childrenIt != json.end() && childrenIt->is_array() )
            {
                tile->children.reserve( childrenIt->size() );

                for ( const nlohmann::json &childJson : *childrenIt )
                {
                    std::unique_ptr<Tile> child =
                        parseTile( childJson, depth + 1, tile->refine, nextId, error );
                    if ( child == nullptr )
                    {
                        // `error` is already set by the failing call.
                        return nullptr;
                    }
                    tile->children.push_back( std::move( child ) );
                }
            }

            return tile;
        }
    } // namespace

    std::optional<math::BoundingVolume> parseBoundingVolume( const nlohmann::json &value )
    {
        if ( !value.is_object() )
        {
            return std::nullopt;
        }

        if ( const auto it = value.find( "box" ); it != value.end() )
        {
            const auto numbers = readNumbers<12>( *it );
            if ( !numbers.has_value() )
            {
                return std::nullopt;
            }

            return math::BoundingVolume::fromBox(
                math::Vec3( ( *numbers )[0], ( *numbers )[1], ( *numbers )[2] ),
                math::Vec3( ( *numbers )[3], ( *numbers )[4], ( *numbers )[5] ),
                math::Vec3( ( *numbers )[6], ( *numbers )[7], ( *numbers )[8] ),
                math::Vec3( ( *numbers )[9], ( *numbers )[10], ( *numbers )[11] ) );
        }

        if ( const auto it = value.find( "region" ); it != value.end() )
        {
            const auto numbers = readNumbers<6>( *it );
            if ( !numbers.has_value() )
            {
                return std::nullopt;
            }

            math::Region region;
            region.west = ( *numbers )[0];
            region.south = ( *numbers )[1];
            region.east = ( *numbers )[2];
            region.north = ( *numbers )[3];
            region.minHeight = ( *numbers )[4];
            region.maxHeight = ( *numbers )[5];
            return math::BoundingVolume::fromRegion( region );
        }

        if ( const auto it = value.find( "sphere" ); it != value.end() )
        {
            const auto numbers = readNumbers<4>( *it );
            if ( !numbers.has_value() )
            {
                return std::nullopt;
            }

            return math::BoundingVolume::fromSphere(
                math::Vec3( ( *numbers )[0], ( *numbers )[1], ( *numbers )[2] ), ( *numbers )[3] );
        }

        return std::nullopt;
    }

    TilesetParseResult parseTilesetJson( const nlohmann::json &json, RefineMode rootRefine )
    {
        TilesetParseResult result;

        if ( !json.is_object() )
        {
            result.error = "tileset.json: expected a JSON object";
            return result;
        }

        const auto assetIt = json.find( "asset" );
        if ( assetIt == json.end() || !assetIt->is_object() )
        {
            result.error = "tileset.json: missing asset";
            return result;
        }

        result.assetVersion = readString( *assetIt, "version" );
        if ( result.assetVersion.empty() )
        {
            result.error = "tileset.json: missing asset.version";
            return result;
        }

        result.geometricError = readReal( json, "geometricError", 0.0 );

        const auto rootIt = json.find( "root" );
        if ( rootIt == json.end() )
        {
            result.error = "tileset.json: missing root";
            return result;
        }

        std::size_t nextId = 0;
        result.root = parseTile( *rootIt, 0, rootRefine, nextId, result.error );
        return result;
    }

} // namespace tiles3d::core
