// SPDX-License-Identifier: Unlicense

#include "content/GltfReader.h"

#include <nlohmann/json.hpp>

// PointIndex / AttributeValueIndex live in geometry_indices.h and are defined by the
// DEFINE_NEW_DRACO_INDEX_TYPE macro - there is no draco/point_cloud/point_index.h.
#include "draco/attributes/geometry_indices.h"
#include "draco/compression/decode.h"
#include "draco/core/decoder_buffer.h"
#include "draco/mesh/mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

namespace tiles3d::core
{
    namespace
    {
        constexpr std::uint32_t kGlbMagic = 0x46546C67u;  // "glTF"
        constexpr std::uint32_t kChunkJson = 0x4E4F534Au; // "JSON"
        constexpr std::uint32_t kChunkBin = 0x004E4942u;  // "BIN\0"

        constexpr const char *kDracoExtension = "KHR_draco_mesh_compression";
        constexpr const char *kUnlitExtension = "KHR_materials_unlit";

        constexpr std::uint32_t kComponentSByte = 5120u;
        constexpr std::uint32_t kComponentUByte = 5121u;
        constexpr std::uint32_t kComponentShort = 5122u;
        constexpr std::uint32_t kComponentUShort = 5123u;
        constexpr std::uint32_t kComponentUint32 = 5125u;
        constexpr std::uint32_t kComponentFloat = 5126u;

        constexpr std::int32_t kModeTriangles = 4;
        constexpr std::int32_t kModeTriangleStrip = 5;
        constexpr std::int32_t kModeTriangleFan = 6;

        std::uint32_t readU32( const std::byte *data )
        {
            const auto *bytes = reinterpret_cast<const unsigned char *>( data );
            return static_cast<std::uint32_t>( bytes[0] ) |
                   ( static_cast<std::uint32_t>( bytes[1] ) << 8 ) |
                   ( static_cast<std::uint32_t>( bytes[2] ) << 16 ) |
                   ( static_cast<std::uint32_t>( bytes[3] ) << 24 );
        }

        struct GlbView
        {
            const std::byte *jsonBegin = nullptr;
            std::size_t jsonLength = 0;
            const std::byte *binBegin = nullptr;
            std::size_t binLength = 0;
        };

        bool parseGlb( const std::vector<std::byte> &glb, GlbView &out, std::string &error )
        {
            if ( glb.size() < 12u || readU32( glb.data() ) != kGlbMagic )
            {
                error = "not a binary glTF container";
                return false;
            }

            const std::size_t declaredLength = readU32( glb.data() + 8 );
            const std::size_t total = declaredLength <= glb.size() ? declaredLength : glb.size();

            std::size_t offset = 12u;
            bool haveJson = false;

            while ( offset + 8u <= total )
            {
                const std::size_t chunkLength = readU32( glb.data() + offset );
                const std::uint32_t chunkType = readU32( glb.data() + offset + 4 );
                const std::size_t body = offset + 8u;

                if ( body + chunkLength > total )
                {
                    error = "binary glTF chunk runs past the end of the buffer";
                    return false;
                }

                if ( chunkType == kChunkJson && !haveJson )
                {
                    out.jsonBegin = glb.data() + body;
                    out.jsonLength = chunkLength;
                    haveJson = true;
                }
                else if ( chunkType == kChunkBin && out.binBegin == nullptr )
                {
                    out.binBegin = glb.data() + body;
                    out.binLength = chunkLength;
                }

                offset = body + chunkLength;
            }

            if ( !haveJson )
            {
                error = "binary glTF has no JSON chunk";
                return false;
            }
            return true;
        }

        /// Not json::value(): that overload throws a type_error when the key exists with a
        /// different type, and this layer does not use exceptions.
        std::size_t readSizeField( const nlohmann::json &object, const char *key )
        {
            const auto it = object.find( key );
            if ( it == object.end() || !it->is_number_integer() )
            {
                return 0;
            }

            const std::int64_t value = it->get<std::int64_t>();
            return value > 0 ? static_cast<std::size_t>( value ) : 0;
        }

        std::int64_t readIntField( const nlohmann::json &object, const char *key,
                                   const std::int64_t fallback )
        {
            const auto it = object.find( key );
            if ( it == object.end() || !it->is_number_integer() )
            {
                return fallback;
            }

            return it->get<std::int64_t>();
        }

        std::uint32_t readUintField( const nlohmann::json &object, const char *key,
                                     const std::uint32_t fallback )
        {
            const auto it = object.find( key );
            if ( it == object.end() )
            {
                return fallback;
            }
            if ( it->is_number_unsigned() )
            {
                return static_cast<std::uint32_t>( it->get<std::uint64_t>() );
            }
            if ( it->is_number_integer() )
            {
                const std::int64_t value = it->get<std::int64_t>();
                return value >= 0 ? static_cast<std::uint32_t>( value ) : fallback;
            }
            return fallback;
        }

        bool arrayMentions( const nlohmann::json &document, const char *key, const char *value )
        {
            const auto it = document.find( key );
            if ( it == document.end() || !it->is_array() )
            {
                return false;
            }

            for ( const nlohmann::json &entry : *it )
            {
                if ( entry.is_string() && entry.get<std::string>() == value )
                {
                    return true;
                }
            }
            return false;
        }

        std::size_t componentSize( const std::uint32_t componentType )
        {
            switch ( componentType )
            {
                case kComponentSByte:
                case kComponentUByte:
                    return 1u;
                case kComponentShort:
                case kComponentUShort:
                    return 2u;
                case kComponentUint32:
                case kComponentFloat:
                    return 4u;
                default:
                    return 0u;
            }
        }

        std::int32_t typeComponentCount( const std::string &type )
        {
            if ( type == "SCALAR" )
            {
                return 1;
            }
            if ( type == "VEC2" )
            {
                return 2;
            }
            if ( type == "VEC3" )
            {
                return 3;
            }
            if ( type == "VEC4" )
            {
                return 4;
            }
            if ( type == "MAT4" )
            {
                return 16;
            }
            return 0;
        }

        /// One resolved glTF accessor: a typed view into the model's binary chunk.
        struct AccessorRef
        {
            const std::byte *base = nullptr; // first element, already offset by bufferView
            std::size_t count = 0;
            std::int32_t components = 0;
            std::size_t stride = 0; // bytes between elements
            std::uint32_t componentType = 0;
            bool normalized = false;
        };

        bool resolveAccessor( const nlohmann::json &document,
                              const std::vector<std::byte> &bin, const std::int64_t accessorIndex,
                              AccessorRef &out, std::string &error )
        {
            const auto accessorsIt = document.find( "accessors" );
            const auto bufferViewsIt = document.find( "bufferViews" );

            if ( accessorsIt == document.end() || !accessorsIt->is_array() ||
                 accessorIndex < 0 || static_cast<std::size_t>( accessorIndex ) >=
                                          accessorsIt->size() )
            {
                error = "accessor index out of range";
                return false;
            }

            const nlohmann::json &accessor = ( *accessorsIt )[static_cast<std::size_t>( accessorIndex )];

            if ( accessor.find( "sparse" ) != accessor.end() )
            {
                error = "sparse accessors are not supported";
                return false;
            }

            const std::string type = accessor.value( "type", std::string() );
            out.components = typeComponentCount( type );
            if ( out.components <= 0 )
            {
                error = "accessor has unsupported type " + type;
                return false;
            }

            out.count = readSizeField( accessor, "count" );
            out.componentType = static_cast<std::uint32_t>(
                readIntField( accessor, "componentType", 0 ) );
            out.normalized = accessor.value( "normalized", false );

            if ( componentSize( out.componentType ) == 0u )
            {
                error = "accessor has unsupported componentType";
                return false;
            }

            const std::int64_t bufferViewIndex = readIntField( accessor, "bufferView", -1 );
            if ( bufferViewIndex < 0 )
            {
                // Only valid for a Draco-compressed primitive; the caller must not read
                // from such an accessor. Enforced by not resolving it here.
                error = "accessor has no bufferView (is the primitive still Draco compressed?)";
                return false;
            }

            if ( bufferViewsIt == document.end() || !bufferViewsIt->is_array() ||
                 static_cast<std::size_t>( bufferViewIndex ) >= bufferViewsIt->size() )
            {
                error = "bufferView index out of range";
                return false;
            }

            const nlohmann::json &bufferView =
                ( *bufferViewsIt )[static_cast<std::size_t>( bufferViewIndex )];

            const std::size_t viewOffset = readSizeField( bufferView, "byteOffset" );
            const std::size_t viewLength = readSizeField( bufferView, "byteLength" );
            const std::size_t elementSize =
                componentSize( out.componentType ) * static_cast<std::size_t>( out.components );

            std::size_t stride = elementSize;
            const std::size_t declaredStride = readSizeField( bufferView, "byteStride" );
            if ( declaredStride != 0u )
            {
                if ( declaredStride < elementSize )
                {
                    error = "bufferView byteStride is smaller than one element";
                    return false;
                }
                stride = declaredStride;
            }

            const std::size_t accessorByteOffset = readSizeField( accessor, "byteOffset" );
            if ( viewLength < accessorByteOffset + ( out.count > 0u
                                                         ? ( out.count - 1u ) * stride + elementSize
                                                         : 0u ) )
            {
                error = "accessor data runs past its bufferView";
                return false;
            }
            if ( viewOffset + viewLength > bin.size() )
            {
                error = "bufferView runs past the binary chunk";
                return false;
            }

            out.base = bin.data() + viewOffset + accessorByteOffset;
            out.stride = stride;
            return true;
        }

        float readNormalizedComponent( const std::byte *component, const std::uint32_t componentType,
                                       const bool normalized )
        {
            const auto *bytes = reinterpret_cast<const unsigned char *>( component );

            switch ( componentType )
            {
                case kComponentFloat:
                {
                    float value = 0.0f;
                    std::memcpy( &value, component, sizeof( float ) );
                    return value;
                }
                case kComponentUByte:
                {
                    const float value = static_cast<float>( bytes[0] );
                    return normalized ? value / 255.0f : value;
                }
                case kComponentSByte:
                {
                    const auto value = static_cast<float>( reinterpret_cast<const signed char *>( component )[0] );
                    return normalized ? std::max( value / 127.0f, -1.0f ) : value;
                }
                case kComponentUShort:
                {
                    const std::uint16_t raw = static_cast<std::uint16_t>( bytes[0] ) |
                                              ( static_cast<std::uint16_t>( bytes[1] ) << 8 );
                    const float value = static_cast<float>( raw );
                    return normalized ? value / 65535.0f : value;
                }
                case kComponentShort:
                {
                    const std::int16_t raw = static_cast<std::int16_t>(
                        static_cast<std::uint16_t>( bytes[0] ) |
                        ( static_cast<std::uint16_t>( bytes[1] ) << 8 ) );
                    const float value = static_cast<float>( raw );
                    return normalized ? std::max( value / 32767.0f, -1.0f ) : value;
                }
                case kComponentUint32:
                {
                    std::uint32_t raw = 0u;
                    std::memcpy( &raw, component, sizeof( std::uint32_t ) );
                    return static_cast<float>( raw );
                }
                default:
                    return 0.0f;
            }
        }

        /// Reads a resolved accessor as floats, converting integer component types and
        /// honouring the normalized flag (glTF 2.0 3.6.2.3). This read is total: every
        /// component type resolves, so it never fails.
        void readAccessorFloats( const AccessorRef &accessor, std::vector<float> &out )
        {
            out.assign( accessor.count * static_cast<std::size_t>( accessor.components ), 0.0f );

            for ( std::size_t element = 0; element < accessor.count; ++element )
            {
                const std::byte *cursor =
                    accessor.base + element * accessor.stride;

                float *write = out.data() + element * static_cast<std::size_t>( accessor.components );
                for ( std::int32_t component = 0; component < accessor.components; ++component )
                {
                    write[component] = readNormalizedComponent(
                        cursor + componentSize( accessor.componentType ) *
                                     static_cast<std::size_t>( component ),
                        accessor.componentType, accessor.normalized );
                }
            }
        }

        /// Reads a resolved index accessor as uint32.
        bool readAccessorIndices( const AccessorRef &accessor, std::vector<std::uint32_t> &out,
                                  std::string &error )
        {
            out.resize( accessor.count );

            for ( std::size_t element = 0; element < accessor.count; ++element )
            {
                const std::byte *cursor = accessor.base + element * accessor.stride;
                const auto *bytes = reinterpret_cast<const unsigned char *>( cursor );

                switch ( accessor.componentType )
                {
                    case kComponentUByte:
                        out[element] = bytes[0];
                        break;
                    case kComponentUShort:
                        out[element] = static_cast<std::uint16_t>( bytes[0] ) |
                                       ( static_cast<std::uint16_t>( bytes[1] ) << 8 );
                        break;
                    case kComponentUint32:
                        out[element] = readU32( cursor );
                        break;
                    default:
                        error = "index accessor has a non-integer componentType";
                        return false;
                }
            }
            return true;
        }

        /// Reads one Draco attribute entry as floats.
        ///
        /// draco's typed accessor is GetValue<T, N>() where N is a *non-type* template
        /// parameter, so it cannot be deduced from a "float *" - the component count has
        /// to be dispatched at compile time. The raw read is correct because the decoder
        /// applies attribute transforms unless SetSkipAttributeTransform() is called, so
        /// quantised attributes come out of DecodeMeshFromBuffer() already dequantised.
        bool readDracoAttribute( const draco::PointAttribute *attribute,
                                 const draco::AttributeValueIndex index, float *out,
                                 const std::int32_t components )
        {
            switch ( components )
            {
                case 1:
                {
                    std::array<float, 1> value{};
                    if ( !attribute->GetValue<float, 1>( index, &value ) )
                    {
                        return false;
                    }
                    std::memcpy( out, value.data(), sizeof( value ) );
                    return true;
                }
                case 2:
                {
                    std::array<float, 2> value{};
                    if ( !attribute->GetValue<float, 2>( index, &value ) )
                    {
                        return false;
                    }
                    std::memcpy( out, value.data(), sizeof( value ) );
                    return true;
                }
                case 3:
                {
                    std::array<float, 3> value{};
                    if ( !attribute->GetValue<float, 3>( index, &value ) )
                    {
                        return false;
                    }
                    std::memcpy( out, value.data(), sizeof( value ) );
                    return true;
                }
                case 4:
                {
                    std::array<float, 4> value{};
                    if ( !attribute->GetValue<float, 4>( index, &value ) )
                    {
                        return false;
                    }
                    std::memcpy( out, value.data(), sizeof( value ) );
                    return true;
                }
                default:
                    return false;
            }
        }

        /// Composes a glTF node's local transform: a column-major matrix, or TRS with a
        /// (x, y, z, w) quaternion, into a column-major 4x4.
        math::Mat4 composeNodeTransform( const nlohmann::json &node )
        {
            const auto matrixIt = node.find( "matrix" );
            if ( matrixIt != node.end() && matrixIt->is_array() && matrixIt->size() == 16 )
            {
                // glTF stores column-major number[16]; so does glm.
                math::Mat4 result{};
                for ( std::size_t column = 0; column < 4; ++column )
                {
                    for ( std::size_t row = 0; row < 4; ++row )
                    {
                        result[column][row] = ( *matrixIt )[column * 4u + row].get<double>();
                    }
                }
                return result;
            }

            double translation[3] = { 0.0, 0.0, 0.0 };
            if ( const auto it = node.find( "translation" );
                 it != node.end() && it->is_array() && it->size() >= 3u )
            {
                for ( std::size_t axis = 0; axis < 3u; ++axis )
                {
                    translation[axis] = ( *it )[axis].get<double>();
                }
            }

            double rotation[4] = { 0.0, 0.0, 0.0, 1.0 };
            if ( const auto it = node.find( "rotation" );
                 it != node.end() && it->is_array() && it->size() >= 4u )
            {
                for ( std::size_t axis = 0; axis < 4u; ++axis )
                {
                    rotation[axis] = ( *it )[axis].get<double>();
                }
            }

            double scale[3] = { 1.0, 1.0, 1.0 };
            if ( const auto it = node.find( "scale" );
                 it != node.end() && it->is_array() && it->size() >= 3u )
            {
                for ( std::size_t axis = 0; axis < 3u; ++axis )
                {
                    scale[axis] = ( *it )[axis].get<double>();
                }
            }

            const double qx = rotation[0];
            const double qy = rotation[1];
            const double qz = rotation[2];
            const double qw = rotation[3];

            // Quaternion to 3x3, column-major like glm: each constructor triple is one
            // column of the rotation matrix.
            //
            // Named `basis`, not `rotation`: `rotation` is already the quaternion array in
            // this scope, and reusing the name silently turns "matrix column * scale" into
            // "double * double", which does not construct a dvec4.
            const math::Mat3 basis(
                1.0 - 2.0 * ( qy * qy + qz * qz ), 2.0 * ( qx * qy + qz * qw ),
                2.0 * ( qx * qz - qy * qw ), 2.0 * ( qx * qy - qz * qw ),
                1.0 - 2.0 * ( qx * qx + qz * qz ), 2.0 * ( qy * qz + qx * qw ),
                2.0 * ( qx * qz + qy * qw ), 2.0 * ( qy * qz - qx * qw ),
                1.0 - 2.0 * ( qx * qx + qy * qy ) );

            math::Mat4 result{ 1.0 };
            for ( std::size_t column = 0; column < 3; ++column )
            {
                result[column] = glm::dvec4( basis[column] * scale[column], 0.0 );
            }
            result[3] = glm::dvec4( translation[0], translation[1], translation[2], 1.0 );
            return result;
        }

    } // namespace

    bool parseGltfModel( const std::vector<std::byte> &glb, GltfModel &out, std::string &error )
    {
        GlbView view;
        if ( !parseGlb( glb, view, error ) )
        {
            return false;
        }

        const nlohmann::json document = nlohmann::json::parse(
            std::string( reinterpret_cast<const char *>( view.jsonBegin ), view.jsonLength ),
            nullptr, false );

        if ( document.is_discarded() )
        {
            error = "binary glTF JSON is not valid JSON";
            return false;
        }

        if ( arrayMentions( document, "extensionsRequired", "EXT_meshopt_compression" ) )
        {
            error = "EXT_meshopt_compression is not supported";
            return false;
        }
        if ( arrayMentions( document, "extensionsRequired", "KHR_texture_basisu" ) )
        {
            error = "KHR_texture_basisu is not supported";
            return false;
        }

        const auto buffersIt = document.find( "buffers" );
        if ( buffersIt == document.end() || !buffersIt->is_array() || buffersIt->empty() )
        {
            error = "binary glTF has no buffer";
            return false;
        }

        if ( ( *buffersIt )[0].find( "uri" ) != ( *buffersIt )[0].end() )
        {
            error = "external .bin buffers are not supported";
            return false;
        }

        out.bin.assign( view.binBegin, view.binBegin + view.binLength );

        // --- images ---
        if ( const auto imagesIt = document.find( "images" );
             imagesIt != document.end() && imagesIt->is_array() )
        {
            const auto bufferViewsIt = document.find( "bufferViews" );

            for ( const nlohmann::json &image : *imagesIt )
            {
                GltfImageData imageData;
                imageData.mimeType = image.value( "mimeType", std::string() );

                const std::string uri = image.value( "uri", std::string() );
                const auto bufferViewIt = image.find( "bufferView" );

                if ( bufferViewIt != image.end() && bufferViewIt->is_number_integer() )
                {
                    const std::size_t viewIndex =
                        static_cast<std::size_t>( bufferViewIt->get<std::int64_t>() );
                    if ( bufferViewsIt == document.end() || !bufferViewsIt->is_array() ||
                         viewIndex >= bufferViewsIt->size() )
                    {
                        error = "image bufferView index out of range";
                        return false;
                    }

                    const nlohmann::json &bufferView = ( *bufferViewsIt )[viewIndex];
                    imageData.offset = readSizeField( bufferView, "byteOffset" );
                    imageData.length = readSizeField( bufferView, "byteLength" );

                    if ( imageData.length == 0u ||
                         imageData.offset + imageData.length > out.bin.size() )
                    {
                        error = "image bufferView runs past the binary chunk";
                        return false;
                    }
                }
                else if ( !uri.empty() )
                {
                    // data: URIs and relative image files are not resolved here; b3dm
                    // photogrammetry embeds its images, so this fails loudly instead.
                    error = "external image URI is not supported: " + uri;
                    return false;
                }
                else
                {
                    error = "image has neither bufferView nor uri";
                    return false;
                }

                out.images.push_back( std::move( imageData ) );
            }
        }

        // --- samplers / textures ---
        if ( const auto texturesIt = document.find( "textures" );
             texturesIt != document.end() && texturesIt->is_array() )
        {
            const auto samplersIt = document.find( "samplers" );

            for ( const nlohmann::json &texture : *texturesIt )
            {
                GltfTextureData textureData;
                textureData.imageIndex =
                    static_cast<std::int32_t>( readIntField( texture, "source", -1 ) );
                textureData.samplerIndex =
                    static_cast<std::int32_t>( readIntField( texture, "sampler", -1 ) );

                if ( samplersIt != document.end() && samplersIt->is_array() &&
                     textureData.samplerIndex >= 0 &&
                     static_cast<std::size_t>( textureData.samplerIndex ) < samplersIt->size() )
                {
                    const nlohmann::json &sampler =
                        ( *samplersIt )[static_cast<std::size_t>( textureData.samplerIndex )];
                    textureData.minFilter =
                        readUintField( sampler, "minFilter", textureData.minFilter );
                    textureData.magFilter =
                        readUintField( sampler, "magFilter", textureData.magFilter );
                    textureData.wrapS = readUintField( sampler, "wrapS", textureData.wrapS );
                    textureData.wrapT = readUintField( sampler, "wrapT", textureData.wrapT );
                }

                out.textures.push_back( textureData );
            }
        }

        // --- materials ---
        if ( const auto materialsIt = document.find( "materials" );
             materialsIt != document.end() && materialsIt->is_array() )
        {
            for ( const nlohmann::json &material : *materialsIt )
            {
                GltfMaterialData materialData;

                // KHR_materials_unlit is declared per material, in material.extensions.
                if ( const auto extIt = material.find( "extensions" );
                     extIt != material.end() && extIt->is_object() )
                {
                    materialData.unlit = extIt->find( kUnlitExtension ) != extIt->end();
                }

                const auto pbrIt = material.find( "pbrMetallicRoughness" );
                if ( pbrIt != material.end() && pbrIt->is_object() )
                {
                    if ( const auto factorIt = pbrIt->find( "baseColorFactor" );
                         factorIt != pbrIt->end() && factorIt->is_array() && factorIt->size() == 4u )
                    {
                        for ( std::size_t channel = 0; channel < 4u; ++channel )
                        {
                            materialData.baseColorFactor[channel] =
                                static_cast<float>( ( *factorIt )[channel].get<double>() );
                        }
                    }

                    if ( const auto textureIt = pbrIt->find( "baseColorTexture" );
                         textureIt != pbrIt->end() && textureIt->is_object() )
                    {
                        materialData.baseColorTexture =
                            static_cast<std::int32_t>( readIntField( *textureIt, "index", -1 ) );
                    }
                }

                const std::int64_t alphaMode = readIntField( material, "alphaMode", 0 );
                if ( alphaMode >= 0 && alphaMode <= 2 )
                {
                    materialData.alphaMode = static_cast<AlphaMode>( alphaMode );
                }
                if ( const auto cutoffIt = material.find( "alphaCutoff" );
                     cutoffIt != material.end() && cutoffIt->is_number() )
                {
                    materialData.alphaCutoff = static_cast<float>( cutoffIt->get<double>() );
                }
                materialData.doubleSided = material.value( "doubleSided", false );

                out.materials.push_back( materialData );
            }
        }

        // --- meshes / primitives ---
        const auto meshesIt = document.find( "meshes" );
        const auto accessorsIt = document.find( "accessors" );
        const auto bufferViewsIt = document.find( "bufferViews" );

        if ( meshesIt == document.end() || !meshesIt->is_array() )
        {
            error = "binary glTF has no meshes";
            return false;
        }
        if ( accessorsIt == document.end() || !accessorsIt->is_array() ||
             bufferViewsIt == document.end() || !bufferViewsIt->is_array() )
        {
            error = "binary glTF is missing accessors or bufferViews";
            return false;
        }

        for ( const nlohmann::json &mesh : *meshesIt )
        {
            GltfMeshData meshData;

            const auto primitivesIt = mesh.find( "primitives" );
            if ( primitivesIt == mesh.end() || !primitivesIt->is_array() ||
                 primitivesIt->empty() )
            {
                error = "mesh has no primitives";
                return false;
            }

            for ( const nlohmann::json &primitive : *primitivesIt )
            {
                const std::int32_t mode =
                    static_cast<std::int32_t>( readIntField( primitive, "mode", kModeTriangles ) );
                if ( mode != kModeTriangles && mode != kModeTriangleStrip &&
                     mode != kModeTriangleFan )
                {
                    error = "primitive mode " + std::to_string( mode ) +
                            " is not supported (TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN)";
                    return false;
                }

                GltfPrimitiveData primitiveData;
                primitiveData.material =
                    static_cast<std::int32_t>( readIntField( primitive, "material", -1 ) );

                const auto attributesIt = primitive.find( "attributes" );
                if ( attributesIt == primitive.end() || !attributesIt->is_object() )
                {
                    error = "primitive has no attributes";
                    return false;
                }

                std::size_t vertexCount = 0;

                // --- Draco: decode straight into the SoA buffers ---
                const auto extensionsIt = primitive.find( "extensions" );
                const bool hasExtensions =
                    extensionsIt != primitive.end() && extensionsIt->is_object();

                // dracoIt is an iterator into the *extensions* object, so it must be
                // compared against that object's end(), not primitive's. An iterator has
                // no .end() of its own - `extensionsIt.end()` was the original mistake.
                // hasExtensions guards short-circuit so extensionsIt is never dereferenced
                // when it is primitive.end().
                auto dracoIt = primitive.end();
                if ( hasExtensions )
                {
                    dracoIt = extensionsIt->find( kDracoExtension );
                }

                const bool isDraco =
                    hasExtensions && dracoIt != extensionsIt->end() && dracoIt->is_object();

                if ( isDraco )
                {
                    const auto dracoAttributesIt = dracoIt->find( "attributes" );
                    if ( dracoAttributesIt == dracoIt->end() ||
                         !dracoAttributesIt->is_object() )
                    {
                        error = "Draco extension has no attribute mapping";
                        return false;
                    }

                    const auto bufferViewIt = dracoIt->find( "bufferView" );
                    if ( bufferViewIt == dracoIt->end() || !bufferViewIt->is_number_integer() )
                    {
                        error = "Draco extension has no bufferView";
                        return false;
                    }

                    const std::size_t compressedViewIndex =
                        static_cast<std::size_t>( bufferViewIt->get<std::int64_t>() );
                    if ( compressedViewIndex >= bufferViewsIt->size() )
                    {
                        error = "Draco extension bufferView index is out of range";
                        return false;
                    }

                    const nlohmann::json &compressedView =
                        ( *bufferViewsIt )[compressedViewIndex];
                    const std::size_t compressedOffset =
                        readSizeField( compressedView, "byteOffset" );
                    const std::size_t compressedLength =
                        readSizeField( compressedView, "byteLength" );

                    if ( compressedLength == 0u ||
                         compressedOffset + compressedLength > out.bin.size() )
                    {
                        error = "Draco payload lies outside the binary chunk";
                        return false;
                    }

                    draco::DecoderBuffer decoderBuffer;
                    decoderBuffer.Init(
                        reinterpret_cast<const char *>( out.bin.data() + compressedOffset ),
                        compressedLength );

                    draco::Decoder decoder;
                    auto decoded = decoder.DecodeMeshFromBuffer( &decoderBuffer );
                    if ( !decoded.ok() )
                    {
                        error = std::string( "Draco decode failed: " ) +
                                decoded.status().error_msg();
                        return false;
                    }

                    std::unique_ptr<draco::Mesh> decodedMesh = std::move( decoded ).value();
                    if ( decodedMesh == nullptr )
                    {
                        error = "Draco decoder returned no mesh";
                        return false;
                    }

                    const std::size_t pointCount = decodedMesh->num_points();
                    vertexCount = pointCount;

                    for ( const auto &[semantic, dracoAttributeId] : dracoAttributesIt->items() )
                    {
                        if ( !dracoAttributeId.is_number_integer() )
                        {
                            continue;
                        }

                        const draco::PointAttribute *attribute = decodedMesh->attribute(
                            dracoAttributeId.get<std::int32_t>() );
                        if ( attribute == nullptr )
                        {
                            continue;
                        }

                        const std::int32_t components = attribute->num_components();
                        if ( components < 1 || components > 4 )
                        {
                            continue;
                        }

                        std::vector<float> values(
                            pointCount * static_cast<std::size_t>( components ) );
                        for ( std::size_t point = 0; point < pointCount; ++point )
                        {
                            const draco::AttributeValueIndex valueIndex =
                                attribute->mapped_index(
                                    draco::PointIndex( static_cast<std::uint32_t>( point ) ) );
                            if ( !readDracoAttribute(
                                     attribute, valueIndex,
                                     values.data() +
                                         point * static_cast<std::size_t>( components ),
                                     components ) )
                            {
                                error = "failed reading Draco attribute for semantic " +
                                        semantic;
                                return false;
                            }
                        }

                        if ( semantic == "POSITION" && components == 3 )
                        {
                            primitiveData.positions = std::move( values );
                        }
                        else if ( semantic == "NORMAL" && components == 3 )
                        {
                            primitiveData.normals = std::move( values );
                        }
                        else if ( semantic == "TEXCOORD_0" && components == 2 )
                        {
                            primitiveData.texcoords0 = std::move( values );
                        }
                        else if ( semantic == "COLOR_0" && ( components == 3 || components == 4 ) )
                        {
                            // glTF colours may be RGB; Godot wants RGBA.
                            primitiveData.colors.resize( pointCount * 4u, 1.0f );
                            for ( std::size_t point = 0; point < pointCount; ++point )
                            {
                                float *rgba =
                                    primitiveData.colors.data() + point * 4u;
                                const float *source =
                                    values.data() + point * static_cast<std::size_t>( components );
                                rgba[0] = source[0];
                                rgba[1] = source[1];
                                rgba[2] = source[2];
                                rgba[3] = components == 4 ? source[3] : 1.0f;
                            }
                        }
                        // Other semantics are dropped, like three.js drops unknown ones.
                    }

                    if ( primitiveData.positions.empty() )
                    {
                        error = "Draco primitive has no POSITION attribute";
                        return false;
                    }

                    // --- indices from the decoded faces ---
                    const std::size_t faceCount = decodedMesh->num_faces();
                    primitiveData.indices.resize( faceCount * 3u );
                    for ( std::size_t face = 0; face < faceCount; ++face )
                    {
                        const draco::Mesh::Face &triangle = decodedMesh->face(
                            draco::FaceIndex( static_cast<std::uint32_t>( face ) ) );
                        for ( std::size_t corner = 0; corner < 3u; ++corner )
                        {
                            primitiveData.indices[face * 3u + corner] =
                                triangle[corner].value();
                        }
                    }
                }
                else
                {
                    // --- plain primitive: read attributes through the accessor model ---
                    const auto readSemantic = [ & ]( const char *semantic,
                                                     std::int32_t requiredComponents,
                                                     std::vector<float> &target ) -> bool {
                        const auto semanticIt = attributesIt->find( semantic );
                        if ( semanticIt == attributesIt->end() ||
                             !semanticIt->is_number_integer() )
                        {
                            return true; // absent
                        }

                        AccessorRef accessor;
                        if ( !resolveAccessor( document, out.bin, semanticIt->get<std::int64_t>(),
                                               accessor, error ) )
                        {
                            return false;
                        }
                        if ( accessor.components != requiredComponents )
                        {
                            error = std::string( semantic ) + " accessor has " +
                                    std::to_string( accessor.components ) + " components";
                            return false;
                        }
                        if ( vertexCount != 0u && accessor.count != vertexCount )
                        {
                            error = std::string( semantic ) +
                                    " accessor vertex count differs from POSITION";
                            return false;
                        }
                        vertexCount = accessor.count;

                        readAccessorFloats( accessor, target );
                        return true;
                    };

                    if ( !readSemantic( "POSITION", 3, primitiveData.positions ) ||
                         !readSemantic( "NORMAL", 3, primitiveData.normals ) ||
                         !readSemantic( "TEXCOORD_0", 2, primitiveData.texcoords0 ) )
                    {
                        return false;
                    }

                    // COLOR_0 is special: RGB is expanded to RGBA.
                    {
                        const auto colorIt = attributesIt->find( "COLOR_0" );
                        if ( colorIt != attributesIt->end() && colorIt->is_number_integer() )
                        {
                            AccessorRef accessor;
                            if ( !resolveAccessor( document, out.bin,
                                                   colorIt->get<std::int64_t>(), accessor,
                                                   error ) )
                            {
                                return false;
                            }
                            if ( accessor.components != 3 && accessor.components != 4 )
                            {
                                error = "COLOR_0 accessor must be VEC3 or VEC4";
                                return false;
                            }
                            if ( vertexCount != 0u && accessor.count != vertexCount )
                            {
                                error = "COLOR_0 accessor vertex count differs from POSITION";
                                return false;
                            }
                            vertexCount = accessor.count;

                            // Total read: every component type resolves, so there is no
                            // error out-param and nothing to check (see readAccessorFloats).
                            std::vector<float> raw;
                            readAccessorFloats( accessor, raw );

                            primitiveData.colors.assign( vertexCount * 4u, 1.0f );
                            for ( std::size_t point = 0; point < vertexCount; ++point )
                            {
                                float *rgba = primitiveData.colors.data() + point * 4u;
                                const float *source =
                                    raw.data() +
                                    point * static_cast<std::size_t>( accessor.components );
                                rgba[0] = source[0];
                                rgba[1] = source[1];
                                rgba[2] = source[2];
                                rgba[3] = accessor.components == 4 ? source[3] : 1.0f;
                            }
                        }
                    }

                    if ( primitiveData.positions.empty() )
                    {
                        error = "primitive has no POSITION attribute";
                        return false;
                    }

                    // --- indices ---
                    const auto indicesIt = primitive.find( "indices" );
                    if ( indicesIt != primitive.end() && indicesIt->is_number_integer() )
                    {
                        AccessorRef accessor;
                        if ( !resolveAccessor( document, out.bin,
                                               indicesIt->get<std::int64_t>(), accessor,
                                               error ) )
                        {
                            return false;
                        }
                        if ( !readAccessorIndices( accessor, primitiveData.indices, error ) )
                        {
                            return false;
                        }
                    }
                    else
                    {
                        primitiveData.indices.resize( vertexCount );
                        for ( std::size_t point = 0; point < vertexCount; ++point )
                        {
                            primitiveData.indices[point] =
                                static_cast<std::uint32_t>( point );
                        }
                    }
                }

                // --- expand STRIP / FAN to TRIANGLES ---
                if ( mode != kModeTriangles )
                {
                    std::vector<std::uint32_t> expanded;
                    const std::size_t sourceCount = primitiveData.indices.size();

                    if ( mode == kModeTriangleStrip )
                    {
                        if ( sourceCount < 3u )
                        {
                            error = "TRIANGLE_STRIP primitive has fewer than 3 indices";
                            return false;
                        }
                        expanded.reserve( ( sourceCount - 2u ) * 3u );
                        for ( std::size_t i = 0; i + 2u < sourceCount; ++i )
                        {
                            if ( i % 2u == 0u )
                            {
                                expanded.push_back( primitiveData.indices[i] );
                                expanded.push_back( primitiveData.indices[i + 1u] );
                                expanded.push_back( primitiveData.indices[i + 2u] );
                            }
                            else
                            {
                                expanded.push_back( primitiveData.indices[i] );
                                expanded.push_back( primitiveData.indices[i + 2u] );
                                expanded.push_back( primitiveData.indices[i + 1u] );
                            }
                        }
                    }
                    else // TRIANGLE_FAN
                    {
                        if ( sourceCount < 3u )
                        {
                            error = "TRIANGLE_FAN primitive has fewer than 3 indices";
                            return false;
                        }
                        expanded.reserve( ( sourceCount - 2u ) * 3u );
                        for ( std::size_t i = 2u; i < sourceCount; ++i )
                        {
                            expanded.push_back( primitiveData.indices[0] );
                            expanded.push_back( primitiveData.indices[i - 1u] );
                            expanded.push_back( primitiveData.indices[i] );
                        }
                    }

                    primitiveData.indices = std::move( expanded );
                }

                // Bounds-check indices so the Godot layer can trust them.
                for ( const std::uint32_t index : primitiveData.indices )
                {
                    if ( static_cast<std::size_t>( index ) >= vertexCount )
                    {
                        error = "primitive index out of range";
                        return false;
                    }
                }

                meshData.primitives.push_back(
                    static_cast<std::int32_t>( out.primitives.size() ) );
                out.primitives.push_back( std::move( primitiveData ) );
            }

            out.meshes.push_back( std::move( meshData ) );
        }

        // --- scene graph ---
        {
            const auto nodesIt = document.find( "nodes" );
            if ( nodesIt == document.end() || !nodesIt->is_array() || nodesIt->empty() )
            {
                error = "binary glTF has no nodes";
                return false;
            }

            out.nodes.resize( nodesIt->size() );
            for ( std::size_t nodeIndex = 0; nodeIndex < nodesIt->size(); ++nodeIndex )
            {
                const nlohmann::json &node = ( *nodesIt )[nodeIndex];
                GltfNodeData &nodeData = out.nodes[nodeIndex];

                nodeData.mesh = static_cast<std::int32_t>( readIntField( node, "mesh", -1 ) );
                nodeData.transform = composeNodeTransform( node );

                if ( const auto childrenIt = node.find( "children" );
                     childrenIt != node.end() && childrenIt->is_array() )
                {
                    for ( const nlohmann::json &child : *childrenIt )
                    {
                        if ( child.is_number_integer() )
                        {
                            nodeData.children.push_back( child.get<std::int32_t>() );
                        }
                    }
                }
            }

            // Default scene, or scene 0, or all nodes without a parent.
            std::vector<std::int32_t> sceneRoots;
            const auto scenesIt = document.find( "scenes" );
            if ( scenesIt != document.end() && scenesIt->is_array() && !scenesIt->empty() )
            {
                const std::int64_t sceneIndex = readIntField( document, "scene", 0 );
                const std::size_t scene =
                    sceneIndex >= 0 && static_cast<std::size_t>( sceneIndex ) < scenesIt->size()
                        ? static_cast<std::size_t>( sceneIndex )
                        : 0u;

                if ( const auto sceneNodesIt = ( *scenesIt )[scene].find( "nodes" );
                     sceneNodesIt != ( *scenesIt )[scene].end() && sceneNodesIt->is_array() )
                {
                    for ( const nlohmann::json &node : *sceneNodesIt )
                    {
                        if ( node.is_number_integer() )
                        {
                            sceneRoots.push_back( node.get<std::int32_t>() );
                        }
                    }
                }
            }
            else
            {
                for ( std::size_t nodeIndex = 0; nodeIndex < out.nodes.size(); ++nodeIndex )
                {
                    sceneRoots.push_back( static_cast<std::int32_t>( nodeIndex ) );
                }
            }

            // Filter roots that are actually someone's child (spec violation tolerated
            // like three.js: keep only nodes never referenced as a child, unless empty).
            std::vector<bool> hasParent( out.nodes.size(), false );
            for ( const GltfNodeData &node : out.nodes )
            {
                for ( const std::int32_t child : node.children )
                {
                    if ( child >= 0 && static_cast<std::size_t>( child ) < hasParent.size() )
                    {
                        hasParent[static_cast<std::size_t>( child )] = true;
                    }
                }
            }

            std::vector<std::int32_t> filtered;
            for ( const std::int32_t root : sceneRoots )
            {
                if ( root >= 0 && static_cast<std::size_t>( root ) < hasParent.size() &&
                     !hasParent[static_cast<std::size_t>( root )] )
                {
                    filtered.push_back( root );
                }
            }

            out.sceneNodes = filtered.empty() ? std::move( sceneRoots ) : std::move( filtered );
        }

        return true;
    }

} // namespace tiles3d::core
