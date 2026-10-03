// SPDX-License-Identifier: Unlicense

#include "content/Ktx2Decoder.h"

// basisu_transcoder.h must come first among the basisu headers: it defines
// BASISD_SUPPORT_KTX2 / BASISD_SUPPORT_KTX2_ZSTD and includes the internal headers
// that the .inc tables assume. basisu_containers.h is pulled in transitively.
#include "basisu_transcoder.h"

#include <mutex>
#include <string>

namespace tiles3d::core
{
    namespace
    {
        constexpr std::uint32_t kKtx2IdentifierLength = 12u;

        /// basisu_transcoder_init() builds the transcoder's lookup tables (~9 ms on a
        /// Core i7). It must run exactly once per process and before any transcode -
        /// skipping it aborts on the `assert(g_transcoder_initialized)` inside
        /// basisu_transcoder.cpp. It is not idempotent (a second call logs an error and
        /// returns), so it is guarded rather than called per decode.
        ///
        /// std::call_once also makes this safe when tiles are decoded on worker threads.
        void ensureTranscoderInitialized()
        {
            static std::once_flag once;
            std::call_once( once, []() { basist::basisu_transcoder_init(); } );
        }

        /// KHR/KTX2 container magic: 0xAB 'K' 'T' 'X' ' ' '2' '0' 0xBB '\r' '\n' 0x1A '\n'.
        /// ktx2_transcoder::init() checks this too, but failing here yields a clearer
        /// message and avoids constructing a transcoder for obviously wrong input.
        bool hasKtx2Identifier( const std::uint8_t *data )
        {
            static constexpr std::uint8_t kIdentifier[kKtx2IdentifierLength] = {
                0xABu, 0x4Bu, 0x54u, 0x58u, 0x20u, 0x32u,
                0x30u, 0xBBu, 0x0Du, 0x0Au, 0x1Au, 0x0Au,
            };

            for ( std::uint32_t i = 0; i < kKtx2IdentifierLength; ++i )
            {
                if ( data[i] != kIdentifier[i] )
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    bool decodeKtx2( const std::uint8_t *data, std::size_t size, Ktx2Image &out,
                     std::string &error )
    {
        out = Ktx2Image{};

        if ( data == nullptr || size == 0u )
        {
            error = "KTX2 buffer is empty";
            return false;
        }
        if ( size < kKtx2IdentifierLength || !hasKtx2Identifier( data ) )
        {
            error = "not a KTX2 container (bad identifier)";
            return false;
        }
        if ( size > 0xFFFFFFFFull )
        {
            // ktx2_transcoder::init() takes uint32_t. A texture this large cannot come
            // out of a 3D Tiles tile, so this is a malformed-input guard, not a limit.
            error = "KTX2 buffer is larger than 4 GiB";
            return false;
        }

        ensureTranscoderInitialized();

        basist::ktx2_transcoder transcoder;
        if ( !transcoder.init( data, static_cast<std::uint32_t>( size ) ) )
        {
            error = "KTX2 header could not be parsed";
            return false;
        }

        if ( transcoder.get_faces() != 1u )
        {
            error = "KTX2 cubemaps are not supported";
            return false;
        }
        if ( transcoder.get_layers() != 0u )
        {
            error = "KTX2 texture arrays are not supported";
            return false;
        }

        // LDR only: the RGBA32 target format cannot be produced from HDR input, and the
        // HDR formats (ASTC/HDR, UASTC HDR) have no 8-bit equivalent anyway.
        if ( !transcoder.is_ldr() )
        {
            error = "KTX2 HDR textures are not supported";
            return false;
        }

        const std::uint32_t width = transcoder.get_width();
        const std::uint32_t height = transcoder.get_height();
        if ( width == 0u || height == 0u )
        {
            error = "KTX2 texture has a zero dimension";
            return false;
        }

        // start_transcoding() decompresses the ETC1S global codebooks; it is required
        // before any transcode_image_level() call and is the expensive part.
        if ( !transcoder.start_transcoding() )
        {
            error = "KTX2 codebooks could not be decoded";
            return false;
        }

        constexpr basist::transcoder_texture_format kFormat = basist::transcoder_texture_format::cTFRGBA32;
        const std::size_t pixelBytes =
            static_cast<std::size_t>( width ) * height * 4u;

        out.width = width;
        out.height = height;
        out.pixels.assign( pixelBytes, 0u );

        // cTFRGBA32 is an uncompressed target, so the buffer size argument is in pixels
        // (not blocks). output_row_pitch_in_blocks_or_pixels = 0 means "tightly packed".
        const bool ok = transcoder.transcode_image_level(
            0u, 0u, 0u, out.pixels.data(), static_cast<std::uint32_t>( pixelBytes ),
            kFormat, 0u, 0u, 0u );

        if ( !ok )
        {
            out = Ktx2Image{};
            error = "KTX2 transcode to RGBA32 failed (" +
                    std::to_string( width ) + "x" + std::to_string( height ) + ")";
            return false;
        }

        return true;
    }

} // namespace tiles3d::core
