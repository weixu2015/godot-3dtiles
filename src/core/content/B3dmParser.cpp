// SPDX-License-Identifier: Unlicense

#include "content/B3dmParser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace tiles3d::core
{
    namespace
    {
        constexpr std::size_t kHeaderSize = 28;
        constexpr std::size_t kAlignment = 8;

        /// b3dm is little endian throughout. Read by hand so this does not depend on the
        /// host being little endian.
        std::uint32_t readUint32LE( const std::byte *data )
        {
            const auto *bytes = reinterpret_cast<const unsigned char *>( data );
            return static_cast<std::uint32_t>( bytes[0] ) |
                   ( static_cast<std::uint32_t>( bytes[1] ) << 8 ) |
                   ( static_cast<std::uint32_t>( bytes[2] ) << 16 ) |
                   ( static_cast<std::uint32_t>( bytes[3] ) << 24 );
        }

        std::size_t alignUp( std::size_t value, std::size_t alignment )
        {
            return ( value + alignment - 1 ) / alignment * alignment;
        }
    } // namespace

    B3dmParseResult parseB3dm( const std::byte *data, const std::size_t size )
    {
        B3dmParseResult result;

        if ( data == nullptr )
        {
            result.error = "b3dm: null buffer";
            return result;
        }

        if ( size < kHeaderSize )
        {
            result.error = "b3dm: buffer is smaller than the 28 byte header";
            return result;
        }

        if ( std::memcmp( data, "b3dm", 4 ) != 0 )
        {
            const std::string magic( reinterpret_cast<const char *>( data ), 4 );
            result.error = "Not a valid b3dm file: " + magic;
            return result;
        }

        const std::uint32_t version = readUint32LE( data + 4 );
        if ( version != 1 )
        {
            result.error = "Unsupported b3dm version: " + std::to_string( version );
            return result;
        }

        const std::uint32_t byteLength = readUint32LE( data + 8 );
        const std::uint32_t featureTableJsonLength = readUint32LE( data + 12 );
        const std::uint32_t featureTableBinaryLength = readUint32LE( data + 16 );
        const std::uint32_t batchTableJsonLength = readUint32LE( data + 20 );
        const std::uint32_t batchTableBinaryLength = readUint32LE( data + 24 );

        // The reference implementation trusts the header and would happily produce a slice
        // past the end of a truncated buffer. Checking costs nothing and turns a silent
        // out-of-bounds read into a clear error.
        if ( byteLength > size )
        {
            result.error = "b3dm: header claims " + std::to_string( byteLength ) +
                           " bytes but only " + std::to_string( size ) + " are available";
            return result;
        }

        std::size_t offset = kHeaderSize;

        // Feature table JSON.
        if ( featureTableJsonLength > 0 )
        {
            const std::size_t end = offset + featureTableJsonLength;
            if ( end > byteLength )
            {
                result.error = "b3dm: feature table JSON runs past the end of the file";
                return result;
            }

            const std::string text( reinterpret_cast<const char *>( data ) + offset,
                                    featureTableJsonLength );

            // allow_exceptions = false: a malformed table is reported, not thrown.
            const nlohmann::json featureTable =
                nlohmann::json::parse( text, nullptr, /* allow_exceptions */ false );

            if ( featureTable.is_discarded() || !featureTable.is_object() )
            {
                result.error = "b3dm: feature table JSON is not valid JSON";
                return result;
            }

            if ( const auto it = featureTable.find( "BATCH_LENGTH" );
                 it != featureTable.end() && it->is_number() )
            {
                result.batchLength = static_cast<std::size_t>( it->get<double>() );
            }

            if ( const auto it = featureTable.find( "RTC_CENTER" );
                 it != featureTable.end() && it->is_array() && it->size() == 3 )
            {
                result.rtcCenter = math::Vec3( ( *it )[0].get<double>(), ( *it )[1].get<double>(),
                                               ( *it )[2].get<double>() );
            }

            offset = end;
        }

        // Feature table binary: skipped, but it counts towards the GLB offset.
        offset += featureTableBinaryLength;

        // Batch table JSON.
        if ( batchTableJsonLength > 0 )
        {
            const std::size_t end = offset + batchTableJsonLength;
            if ( end > byteLength )
            {
                result.error = "b3dm: batch table JSON runs past the end of the file";
                return result;
            }

            result.batchTableJson.assign( reinterpret_cast<const char *>( data ) + offset,
                                          batchTableJsonLength );
            offset = end;
        }

        // Batch table binary: skipped, but it counts towards the GLB offset.
        offset += batchTableBinaryLength;

        // The embedded glTF must start at an 8 byte boundary, so padding may follow the
        // tables. The offset must include the *binary* table lengths as well as the JSON
        // ones, otherwise tiles with a batch table binary would be sliced mid-glTF.
        const std::size_t glbOffset = alignUp( offset, kAlignment );

        if ( glbOffset >= byteLength )
        {
            result.error = "b3dm: no embedded glTF (tables consume the whole file)";
            return result;
        }

        result.glbOffset = glbOffset;
        result.glbLength = byteLength - glbOffset;
        return result;
    }

} // namespace tiles3d::core
