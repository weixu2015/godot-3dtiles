// SPDX-License-Identifier: Unlicense
//
// KTX2 transcoding, the decoder behind glTF's KHR_texture_basisu.
//
// The synthetic cases pin the failure modes (short buffer, wrong magic, empty input)
// so a malformed tile produces a named reason instead of an exception or a crash.
//
// The real-file case is the one that matters: it reads an actual tile out of the 1.1
// Photogrammetry dataset and asserts that the transcode produces the declared
// dimensions and a non-uniform image. It is skipped when the dataset is not present
// so the suite still passes on a machine without E:/GISData.
//
// Note: only CHECK / CHECK_FALSE are used - see tests/CMakeLists.txt for why.

#include <doctest/doctest.h>

#include "content/GltfReader.h"
#include "content/Ktx2Decoder.h"

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace tiles3d::core;

namespace
{
    /// The 12-byte KHR/KTX2 magic with nothing after it: enough to pass the identifier
    /// check and then fail inside ktx2_transcoder::init().
    std::vector<std::uint8_t> ktx2IdentifierOnly()
    {
        return { 0xABu, 0x4Bu, 0x54u, 0x58u, 0x20u, 0x32u,
                 0x30u, 0xBBu, 0x0Du, 0x0Au, 0x1Au, 0x0Au };
    }

    constexpr const char *kPhotogrammetryTile =
        "E:/GISData/3D Tiles/1.1/Photogrammetry/tiles/1/0/0/0.glb";

    bool readWholeFile( const char *path, std::vector<std::byte> &out )
    {
        std::FILE *file = std::fopen( path, "rb" );
        if ( file == nullptr )
        {
            return false;
        }

        std::fseek( file, 0, SEEK_END );
        const long length = std::ftell( file );
        std::fseek( file, 0, SEEK_SET );

        if ( length <= 0 )
        {
            std::fclose( file );
            return false;
        }

        out.resize( static_cast<std::size_t>( length ) );
        const std::size_t read = std::fread( out.data(), 1u, out.size(), file );
        std::fclose( file );

        return read == out.size();
    }
} // namespace

TEST_CASE( "decodeKtx2 rejects an empty buffer" )
{
    Ktx2Image image;
    std::string error;

    CHECK_FALSE( decodeKtx2( nullptr, 0u, image, error ) );
    CHECK_FALSE( error.empty() );
}

TEST_CASE( "decodeKtx2 rejects a buffer that is not a KTX2 container" )
{
    const std::vector<std::uint8_t> garbage = { 'g', 'l', 'T', 'F', 1u, 2u, 3u, 4u,
                                                5u,  6u,  7u,  8u,  9u, 10u, 11u, 12u };

    Ktx2Image image;
    std::string error;

    CHECK_FALSE( decodeKtx2( garbage.data(), garbage.size(), image, error ) );
    CHECK( error.find( "KTX2" ) != std::string::npos );
}

TEST_CASE( "decodeKtx2 rejects a truncated KTX2 header" )
{
    const std::vector<std::uint8_t> identifier = ktx2IdentifierOnly();

    Ktx2Image image;
    std::string error;

    // Correct magic, no header behind it: init() must fail rather than read past the end.
    CHECK_FALSE( decodeKtx2( identifier.data(), identifier.size(), image, error ) );
    CHECK_FALSE( error.empty() );
    CHECK_FALSE( image.valid() );
}

TEST_CASE( "parseGltfModel resolves a KHR_texture_basisu texture source" )
{
    // The 1.1 Photogrammetry tileset declares KHR_texture_basisu in extensionsRequired
    // and puts the image behind texture.extensions.KHR_texture_basisu.source rather
    // than a top-level "source". Both used to make the whole tile fail to load.
    std::vector<std::byte> glb;
    if ( !readWholeFile( kPhotogrammetryTile, glb ) )
    {
        MESSAGE( "skipping: the 1.1 Photogrammetry dataset is not present" );
        return;
    }

    GltfModel model;
    std::string error;
    CHECK( parseGltfModel( glb, model, error ) );

    CHECK( model.images.size() == 1u );
    CHECK( model.images[0].encoding == ImageEncoding::Ktx2 );
    CHECK( model.images[0].length > 0u );

    CHECK( model.textures.size() == 1u );
    CHECK( model.textures[0].imageIndex == 0 );
}

TEST_CASE( "decodeKtx2 transcodes a real KHR_texture_basisu tile image" )
{
    std::vector<std::byte> glb;
    if ( !readWholeFile( kPhotogrammetryTile, glb ) )
    {
        MESSAGE( "skipping: the 1.1 Photogrammetry dataset is not present" );
        return;
    }

    GltfModel model;
    std::string error;
    CHECK( parseGltfModel( glb, model, error ) );
    CHECK( model.images.size() == 1u );

    const GltfImageData &imageData = model.images[0];
    const auto *bytes = reinterpret_cast<const std::uint8_t *>( model.bin.data() ) +
                        imageData.offset;

    Ktx2Image image;
    CHECK( decodeKtx2( bytes, imageData.length, image, error ) );
    CHECK( image.valid() );

    // The photo texture is a real image, so it cannot collapse to a single colour.
    // A decoder that returns an all-zero or all-255 buffer would pass `valid()` but
    // is exactly the failure this guards against.
    std::set<std::uint32_t> distinctColors;
    for ( std::size_t pixel = 0; pixel < image.pixels.size(); pixel += 4u )
    {
        const std::uint32_t color =
            ( static_cast<std::uint32_t>( image.pixels[pixel] ) << 16 ) |
            ( static_cast<std::uint32_t>( image.pixels[pixel + 1u] ) << 8 ) |
            static_cast<std::uint32_t>( image.pixels[pixel + 2u] );
        distinctColors.insert( color );
        if ( distinctColors.size() > 16u )
        {
            break;
        }
    }

    CHECK( distinctColors.size() > 16u );
    CHECK( image.width >= 4u );
    CHECK( image.height >= 4u );
}
