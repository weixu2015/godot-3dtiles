// SPDX-License-Identifier: Unlicense
//
// b3dm parsing tests, ported from the B3dmParser block of __tests__/parsers.spec.ts.

#include <doctest/doctest.h>

#include "content/B3dmParser.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace tiles3d::core;

namespace
{
    /// Byte sequence the reference fixture uses as a stand-in glTF: the "glTF" magic plus
    /// version 2.
    constexpr std::array<std::uint8_t, 8> kGlbStub{ 0x67, 0x6c, 0x54, 0x46, 0x02, 0x00, 0x00, 0x00 };

    void writeUint32LE( std::vector<std::byte> &out, const std::size_t at, const std::uint32_t value )
    {
        out[at + 0] = static_cast<std::byte>( value & 0xFFu );
        out[at + 1] = static_cast<std::byte>( ( value >> 8 ) & 0xFFu );
        out[at + 2] = static_cast<std::byte>( ( value >> 16 ) & 0xFFu );
        out[at + 3] = static_cast<std::byte>( ( value >> 24 ) & 0xFFu );
    }

    /// Builds a b3dm the same way the reference test fixture does, with both binary table
    /// lengths zero.
    std::vector<std::byte> buildB3dm( const std::string &featureTableJson,
                                      const std::string &batchTableJson )
    {
        const std::size_t tableEnd = 28 + featureTableJson.size() + batchTableJson.size();
        const std::size_t glbStart = ( tableEnd + 7 ) / 8 * 8;
        const std::size_t total = glbStart + kGlbStub.size();

        std::vector<std::byte> buffer( total, std::byte{ 0 } );

        std::memcpy( buffer.data(), "b3dm", 4 );
        writeUint32LE( buffer, 4, 1 );
        writeUint32LE( buffer, 8, static_cast<std::uint32_t>( total ) );
        writeUint32LE( buffer, 12, static_cast<std::uint32_t>( featureTableJson.size() ) );
        writeUint32LE( buffer, 16, 0 );
        writeUint32LE( buffer, 20, static_cast<std::uint32_t>( batchTableJson.size() ) );
        writeUint32LE( buffer, 24, 0 );

        std::memcpy( buffer.data() + 28, featureTableJson.data(), featureTableJson.size() );
        std::memcpy( buffer.data() + 28 + featureTableJson.size(), batchTableJson.data(),
                     batchTableJson.size() );

        // The specification requires space padding up to the 8 byte alignment.
        for ( std::size_t i = tableEnd; i < glbStart; ++i )
        {
            buffer[i] = std::byte{ 0x20 };
        }

        std::memcpy( buffer.data() + glbStart, kGlbStub.data(), kGlbStub.size() );
        return buffer;
    }
} // namespace

TEST_CASE( "parseB3dm reads the feature table, batch table and embedded glTF" )
{
    const std::vector<std::byte> buffer = buildB3dm(
        R"({"BATCH_LENGTH":3,"RTC_CENTER":[100,200,300]})",
        R"({"height":{"byteOffset":0,"componentType":"FLOAT","type":"SCALAR"}})" );

    const B3dmParseResult result = parseB3dm( buffer.data(), buffer.size() );

    CHECK( static_cast<bool>( result ) );
    CHECK( result.error.empty() );

    CHECK( result.batchLength == 3 );
    CHECK( result.rtcCenter.has_value() );
    CHECK( result.rtcCenter->x == 100.0 );
    CHECK( result.rtcCenter->y == 200.0 );
    CHECK( result.rtcCenter->z == 300.0 );

    CHECK( result.batchTableJson.find( "height" ) != std::string::npos );

    // The embedded glTF is reported as an offset/length pair, not a copy.
    CHECK( result.glbLength == kGlbStub.size() );
    CHECK( buffer.size() - result.glbOffset == result.glbLength );
    CHECK( std::memcmp( buffer.data() + result.glbOffset, "glTF", 4 ) == 0 );
}

TEST_CASE( "parseB3dm handles a tile with no batch table and no RTC_CENTER" )
{
    const std::vector<std::byte> buffer = buildB3dm( R"({"BATCH_LENGTH":1})", "" );

    const B3dmParseResult result = parseB3dm( buffer.data(), buffer.size() );

    CHECK( static_cast<bool>( result ) );
    CHECK( result.batchLength == 1 );
    CHECK_FALSE( result.rtcCenter.has_value() );
    CHECK( result.batchTableJson.empty() );
    CHECK( result.glbLength == kGlbStub.size() );
    CHECK( std::memcmp( buffer.data() + result.glbOffset, "glTF", 4 ) == 0 );
}

TEST_CASE( "parseB3dm aligns the glTF start when the tables are not 8 byte multiples" )
{
    // Deliberately sized so the tables end mid-alignment word: the GLB must still start on
    // the next 8 byte boundary, with space padding in between.
    const std::vector<std::byte> buffer = buildB3dm( R"({"BATCH_LENGTH":1})", "{}" );

    const B3dmParseResult result = parseB3dm( buffer.data(), buffer.size() );

    CHECK( static_cast<bool>( result ) );
    CHECK( result.glbOffset % 8 == 0 );
    CHECK( std::memcmp( buffer.data() + result.glbOffset, "glTF", 4 ) == 0 );
}

TEST_CASE( "parseB3dm rejects a foreign magic" )
{
    std::vector<std::byte> buffer( 28, std::byte{ 0 } );
    std::memcpy( buffer.data(), "xxxx", 4 );

    const B3dmParseResult result = parseB3dm( buffer.data(), buffer.size() );

    CHECK_FALSE( static_cast<bool>( result ) );
    CHECK( result.error.find( "Not a valid b3dm" ) != std::string::npos );
}

TEST_CASE( "parseB3dm rejects an unsupported version" )
{
    std::vector<std::byte> buffer = buildB3dm( R"({"BATCH_LENGTH":1})", "" );
    writeUint32LE( buffer, 4, 2 );

    const B3dmParseResult result = parseB3dm( buffer.data(), buffer.size() );

    CHECK_FALSE( static_cast<bool>( result ) );
    CHECK( result.error.find( "Unsupported b3dm version" ) != std::string::npos );
}

TEST_CASE( "parseB3dm rejects a truncated buffer" )
{
    // A short buffer must not be read past its end. The reference reads the header
    // unconditionally; this is a deliberate hardening.
    const std::vector<std::byte> tiny( 10, std::byte{ 0 } );
    CHECK_FALSE( static_cast<bool>( parseB3dm( tiny.data(), tiny.size() ) ) );

    // A header that claims more bytes than are present must also be refused.
    std::vector<std::byte> lying = buildB3dm( R"({"BATCH_LENGTH":1})", "" );
    writeUint32LE( lying, 8, static_cast<std::uint32_t>( lying.size() + 1024 ) );

    const B3dmParseResult result = parseB3dm( lying.data(), lying.size() );
    CHECK_FALSE( static_cast<bool>( result ) );
    CHECK( result.error.find( "only" ) != std::string::npos );

    CHECK_FALSE( static_cast<bool>( parseB3dm( nullptr, 0 ) ) );
}
