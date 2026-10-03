// SPDX-License-Identifier: Unlicense
//
// Ktx2Decoder: transcodes an embedded KTX2 texture payload to uncompressed RGBA8.
//
// Why this exists: glTF's KHR_texture_basisu is a *required* extension in every 3D
// Tiles 1.1 tileset produced by the Cesium ion tiling pipeline, and its payload is a
// KTX2 container holding ETC1S or UASTC supercompressed data. Godot's C++ bindings
// expose Image::load_ktx_from_buffer (KTX1) but no KTX2 entry point, and a KTX2 blob
// is not a raw pixel format anyway, so the transcode has to happen before the Godot
// layer ever sees the bytes.
//
// The heavy lifting is done by Basis Universal's ktx2_transcoder, vendored under
// extern/third_party/basisu. This wrapper exists to keep basisu out of the Godot
// layer, to keep the kernel free of engine types (this header includes neither
// godot_cpp/* nor any engine type), and to normalize the many failure modes of the
// transcoder into one human-readable reason.
//
// Deliberately not thread-safe internally: one ktx2_transcoder per call. Callers are
// free to run one decode per worker thread.

#ifndef TILES3D_CORE_CONTENT_KTX2DECODER_H
#define TILES3D_CORE_CONTENT_KTX2DECODER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tiles3d::core
{
    /// A decoded KTX2 texture: tightly packed, top-left origin, 4 bytes per pixel.
    struct Ktx2Image
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        /// RGBA8, row-major, width * height * 4 bytes, no padding between rows.
        /// Basis Universal's cTFRGBA32 already emits exactly this layout.
        std::vector<std::uint8_t> pixels;

        bool valid() const
        {
            return width > 0 && height > 0 &&
                   pixels.size() == static_cast<std::size_t>( width ) * height * 4u;
        }
    };

    /// Transcodes the base mip level of a KTX2 buffer to RGBA8.
    ///
    /// Only image 0 / layer 0 / face 0 is decoded: 3D Tiles textures are 2D, and the
    /// Godot layer regenerates mipmaps itself when the sampler asks for it (matching
    /// what the PNG/JPEG path already does), so decoding the stored mip chain would
    /// only duplicate work.
    ///
    /// Returns false with a reason in `error` on any failure - bad container, an
    /// unsupported basis format (ASTC HDR 6x6 and friends), or a transcode error.
    bool decodeKtx2( const std::uint8_t *data, std::size_t size, Ktx2Image &out,
                     std::string &error );

} // namespace tiles3d::core

#endif
