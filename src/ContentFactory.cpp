// SPDX-License-Identifier: Unlicense

#include "ContentFactory.h"

#include "GodotMathConvert.h"

#include "core/content/B3dmParser.h"
#include "core/content/GltfReader.h"

#include "godot_cpp/classes/array_mesh.hpp"
#include "godot_cpp/classes/base_material3d.hpp"
#include "godot_cpp/classes/global_constants.hpp"
#include "godot_cpp/classes/image.hpp"
#include "godot_cpp/classes/image_texture.hpp"
#include "godot_cpp/classes/material.hpp"
#include "godot_cpp/classes/mesh.hpp"
#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/classes/ref.hpp"
#include "godot_cpp/classes/standard_material3d.hpp"
#include "godot_cpp/classes/texture2d.hpp"
#include "godot_cpp/core/memory.hpp"
#include "godot_cpp/variant/array.hpp"
#include "godot_cpp/variant/color.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/packed_color_array.hpp"
#include "godot_cpp/variant/packed_int32_array.hpp"
#include "godot_cpp/variant/packed_vector2_array.hpp"
#include "godot_cpp/variant/packed_vector3_array.hpp"
#include "godot_cpp/variant/string.hpp"
#include "godot_cpp/variant/utility_functions.hpp"
#include "godot_cpp/variant/variant.hpp"
#include "godot_cpp/variant/vector2.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace tiles3d
{
    using godot::ArrayMesh;
    using godot::BaseMaterial3D;
    using godot::Color;
    using godot::Image;
    using godot::ImageTexture;
    using godot::Mesh;
    using godot::MeshInstance3D;
    using godot::Node3D;
    using godot::PackedByteArray;
    using godot::PackedColorArray;
    using godot::PackedInt32Array;
    using godot::PackedVector2Array;
    using godot::PackedVector3Array;
    using godot::Ref;
    using godot::StandardMaterial3D;
    using godot::String;
    using godot::Texture2D;
    using godot::UtilityFunctions;
    using godot::Vector2;
    using godot::Vector3;

    namespace
    {
        bool startsWith( const PackedByteArray &bytes, const char *magic, const std::size_t length )
        {
            if ( static_cast<std::size_t>( bytes.size() ) < length )
            {
                return false;
            }
            return std::memcmp( bytes.ptr(), magic, length ) == 0;
        }

        /// The kernel's std::byte span as Godot bytes.
        PackedByteArray toPackedByteArray( const std::byte *data, const std::size_t size )
        {
            PackedByteArray out;
            out.resize( static_cast<std::int64_t>( size ) );

            if ( size != 0u )
            {
                std::memcpy( out.ptrw(), data, size );
            }
            return out;
        }

        /// glTF sampler min/mag filter to Godot's material filter. The min filter decides
        /// (it is the one applied at distance); mipmapped modes get mipmaps generated on
        /// the image. Mirrors the cesium-native era loader.
        BaseMaterial3D::TextureFilter mapTextureFilter( const core::GltfTextureData &texture,
                                                        bool &generateMipmaps )
        {
            generateMipmaps = false;

            switch ( texture.minFilter )
            {
                case 9728u: // NEAREST
                    return BaseMaterial3D::TEXTURE_FILTER_NEAREST;
                case 9729u: // LINEAR
                    return BaseMaterial3D::TEXTURE_FILTER_LINEAR;
                case 9984u: // NEAREST_MIPMAP_NEAREST
                    generateMipmaps = true;
                    return BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS;
                case 9985u: // LINEAR_MIPMAP_NEAREST
                case 9986u: // NEAREST_MIPMAP_LINEAR
                case 9987u: // LINEAR_MIPMAP_LINEAR (also the glTF default)
                default:
                    generateMipmaps = true;
                    return BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS;
            }
        }

        /// Decodes an embedded image into a texture. Returns null (with a reason appended
        /// to `error`) when the reference is missing or the bytes do not decode; the
        /// material then falls back to its base color, which is non-fatal.
        Ref<Texture2D> buildAlbedoTexture( const core::GltfModel &model,
                                           const std::int32_t textureIndex,
                                           BaseMaterial3D::TextureFilter &outFilter,
                                           bool &outWrapClamp, std::string &error )
        {
            outFilter = BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS;
            outWrapClamp = false;

            if ( textureIndex < 0 ||
                 static_cast<std::size_t>( textureIndex ) >= model.textures.size() )
            {
                return nullptr;
            }

            const core::GltfTextureData &texture =
                model.textures[static_cast<std::size_t>( textureIndex )];

            outWrapClamp = texture.wrapS == 33071u || texture.wrapT == 33071u; // CLAMP_TO_EDGE

            if ( texture.imageIndex < 0 ||
                 static_cast<std::size_t>( texture.imageIndex ) >= model.images.size() )
            {
                error += " texture " + std::to_string( textureIndex ) + " has no image;";
                return nullptr;
            }

            const core::GltfImageData &imageData =
                model.images[static_cast<std::size_t>( texture.imageIndex )];

            Ref<Image> image;
            image.instantiate();

            const PackedByteArray bytes = toPackedByteArray( model.bin.data() + imageData.offset,
                                                             imageData.length );

            const bool isPng = imageData.mimeType == "image/png";
            const bool isJpeg = imageData.mimeType == "image/jpeg";
            const godot::Error decodeError =
                isPng ? image->load_png_from_buffer( bytes )
                      : isJpeg ? image->load_jpg_from_buffer( bytes )
                               : godot::FAILED;

            if ( decodeError != godot::OK )
            {
                error += " image " + std::to_string( texture.imageIndex ) +
                         " could not be decoded (mimeType " +
                         ( imageData.mimeType.empty() ? "<unset>" : imageData.mimeType ) + ");";
                return nullptr;
            }

            bool generateMipmaps = false;
            outFilter = mapTextureFilter( texture, generateMipmaps );

            if ( generateMipmaps && image->get_width() > 1 && image->get_height() > 1 )
            {
                image->generate_mipmaps();
            }

            return ImageTexture::create_from_image( image );
        }

        /// One StandardMaterial3D per glTF material index, shared by every primitive that
        /// references it (photogrammetry tiles repeat one material across hundreds of
        /// tiles; sharing keeps the material count at the glTF material count).
        Ref<StandardMaterial3D> buildMaterial( const core::GltfModel &model,
                                               const std::int32_t materialIndex,
                                               std::string &error )
        {
            Ref<StandardMaterial3D> material;
            material.instantiate();

            if ( materialIndex < 0 ||
                 static_cast<std::size_t>( materialIndex ) >= model.materials.size() )
            {
                return material;
            }

            const core::GltfMaterialData &data =
                model.materials[static_cast<std::size_t>( materialIndex )];

            material->set_albedo( Color( data.baseColorFactor[0], data.baseColorFactor[1],
                                         data.baseColorFactor[2], data.baseColorFactor[3] ) );

            if ( data.unlit )
            {
                material->set_shading_mode( BaseMaterial3D::SHADING_MODE_UNSHADED );
            }

            switch ( data.alphaMode )
            {
                case core::AlphaMode::Mask:
                    material->set_transparency( BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR );
                    material->set_alpha_scissor_threshold( data.alphaCutoff );
                    break;
                case core::AlphaMode::Blend:
                    material->set_transparency( BaseMaterial3D::TRANSPARENCY_ALPHA );
                    break;
                case core::AlphaMode::Opaque:
                default:
                    break;
            }

            material->set_cull_mode( data.doubleSided ? BaseMaterial3D::CULL_DISABLED
                                                      : BaseMaterial3D::CULL_BACK );

            if ( data.baseColorTexture >= 0 )
            {
                BaseMaterial3D::TextureFilter filter = BaseMaterial3D::TEXTURE_FILTER_LINEAR;
                bool wrapClamp = false;

                Ref<Texture2D> texture = buildAlbedoTexture( model, data.baseColorTexture,
                                                             filter, wrapClamp, error );
                if ( texture.is_valid() )
                {
                    material->set_texture( BaseMaterial3D::TEXTURE_ALBEDO, texture );
                    material->set_texture_filter( filter );

                    // Godot 4.7 models wrap as a bool flag (FLAG_USE_TEXTURE_REPEAT);
                    // mirrored repeat has no equivalent, so it maps to plain repeat.
                    material->set_flag( BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT, !wrapClamp );
                }
            }

            return material;
        }

        /// Surface arrays for one glTF primitive: SoA float buffers straight into Godot's
        /// packed arrays, index list as int32.
        ///
        /// A lit primitive without normals is expanded to per-triangle vertices carrying
        /// flat normals (the old computeFlatNormals path): shared vertices cannot carry a
        /// per-face normal, so the vertex data is duplicated. Unlit primitives - all of
        /// photogrammetry - skip normals entirely; unshaded shading ignores them.
        bool buildSurfaceArrays( const core::GltfPrimitiveData &primitive,
                                 const core::GltfMaterialData *material, godot::Array &arrays,
                                 std::string &error )
        {
            std::size_t vertexCount = primitive.positions.size() / 3u;
            if ( vertexCount == 0u )
            {
                error += " primitive has no vertices;";
                return false;
            }

            const bool needsNormals =
                primitive.normals.empty() && material != nullptr && !material->unlit;

            // Expanded copies when flat normals are generated; otherwise these start as
            // copies of the primitive's own data.
            std::vector<float> positions = primitive.positions;
            std::vector<float> normals;
            std::vector<float> texcoords0 = primitive.texcoords0;
            std::vector<float> colors = primitive.colors;
            std::vector<std::uint32_t> indices = primitive.indices;

            if ( needsNormals )
            {
                const std::size_t triangleCount = indices.size() / 3u;
                if ( triangleCount == 0u )
                {
                    error += " lit primitive without normals and without triangles;";
                    return false;
                }

                const std::size_t expandedCount = triangleCount * 3u;
                normals.resize( expandedCount * 3u );

                auto expand = [ & ]( std::vector<float> &buffer, const std::size_t components ) {
                    if ( buffer.empty() )
                    {
                        return;
                    }
                    std::vector<float> source = std::move( buffer );
                    buffer.resize( expandedCount * components );
                    for ( std::size_t triangle = 0; triangle < triangleCount; ++triangle )
                    {
                        for ( std::size_t corner = 0; corner < 3u; ++corner )
                        {
                            const std::uint32_t sourceVertex =
                                indices[triangle * 3u + corner];
                            std::memcpy(
                                buffer.data() + ( triangle * 3u + corner ) * components,
                                source.data() + sourceVertex * components,
                                components * sizeof( float ) );
                        }
                    }
                };

                expand( positions, 3u );
                expand( texcoords0, 2u );
                expand( colors, 4u );

                for ( std::size_t triangle = 0; triangle < triangleCount; ++triangle )
                {
                    const float *v0 = positions.data() + ( triangle * 3u + 0u ) * 3u;
                    const float *v1 = positions.data() + ( triangle * 3u + 1u ) * 3u;
                    const float *v2 = positions.data() + ( triangle * 3u + 2u ) * 3u;

                    const double ax = static_cast<double>( v1[0] ) - v0[0];
                    const double ay = static_cast<double>( v1[1] ) - v0[1];
                    const double az = static_cast<double>( v1[2] ) - v0[2];
                    const double bx = static_cast<double>( v2[0] ) - v0[0];
                    const double by = static_cast<double>( v2[1] ) - v0[1];
                    const double bz = static_cast<double>( v2[2] ) - v0[2];

                    double nx = ay * bz - az * by;
                    double ny = az * bx - ax * bz;
                    double nz = ax * by - ay * bx;
                    const double length = std::sqrt( nx * nx + ny * ny + nz * nz );

                    if ( length > 0.0 )
                    {
                        nx /= length;
                        ny /= length;
                        nz /= length;
                    }

                    float *normal = normals.data() + triangle * 9u;
                    normal[0] = static_cast<float>( nx );
                    normal[1] = static_cast<float>( ny );
                    normal[2] = static_cast<float>( nz );
                    normal[3] = static_cast<float>( nx );
                    normal[4] = static_cast<float>( ny );
                    normal[5] = static_cast<float>( nz );
                    normal[6] = static_cast<float>( nx );
                    normal[7] = static_cast<float>( ny );
                    normal[8] = static_cast<float>( nz );
                }

                vertexCount = expandedCount;
                indices.resize( expandedCount );
                for ( std::size_t vertex = 0; vertex < expandedCount; ++vertex )
                {
                    indices[vertex] = static_cast<std::uint32_t>( vertex );
                }
            }

            arrays.resize( static_cast<std::int32_t>( Mesh::ARRAY_MAX ) );

            PackedVector3Array vertices;
            vertices.resize( static_cast<std::int64_t>( vertexCount ) );
            std::memcpy( vertices.ptrw(), positions.data(), vertexCount * 3u * sizeof( float ) );
            arrays[static_cast<std::int32_t>( Mesh::ARRAY_VERTEX )] = vertices;

            if ( !normals.empty() )
            {
                PackedVector3Array normalArray;
                normalArray.resize( static_cast<std::int64_t>( normals.size() / 3u ) );
                std::memcpy( normalArray.ptrw(), normals.data(),
                             normals.size() * sizeof( float ) );
                arrays[static_cast<std::int32_t>( Mesh::ARRAY_NORMAL )] = normalArray;
            }

            if ( !texcoords0.empty() )
            {
                PackedVector2Array texcoords;
                texcoords.resize( static_cast<std::int64_t>( texcoords0.size() / 2u ) );
                std::memcpy( texcoords.ptrw(), texcoords0.data(),
                             texcoords0.size() * sizeof( float ) );
                arrays[static_cast<std::int32_t>( Mesh::ARRAY_TEX_UV )] = texcoords;
            }

            if ( !colors.empty() )
            {
                PackedColorArray colorArray;
                colorArray.resize( static_cast<std::int64_t>( colors.size() / 4u ) );
                godot::Color *colorWrite = colorArray.ptrw();
                for ( std::size_t vertex = 0; vertex < colors.size() / 4u; ++vertex )
                {
                    colorWrite[vertex] = Color( colors[vertex * 4u + 0u],
                                                colors[vertex * 4u + 1u],
                                                colors[vertex * 4u + 2u],
                                                colors[vertex * 4u + 3u] );
                }
                arrays[static_cast<std::int32_t>( Mesh::ARRAY_COLOR )] = colorArray;
            }

            // Godot front faces are wound clockwise, glTF/OpenGL counter-clockwise. Reverse the
            // whole index buffer so back-face culling keeps the outer surface instead of the
            // interior - the classic "buildings look hollow" symptom. Mirrors the legacy
            // GodotPrepareRendererResources::loadPrimitive path (indices_.reverse()).
            if ( !indices.empty() )
            {
                std::reverse( indices.begin(), indices.end() );
            }

            if ( !indices.empty() )
            {
                PackedInt32Array indexArray;
                indexArray.resize( static_cast<std::int64_t>( indices.size() ) );
                std::int32_t *indexWrite = indexArray.ptrw();
                for ( std::size_t index = 0; index < indices.size(); ++index )
                {
                    indexWrite[index] = static_cast<std::int32_t>( indices[index] );
                }
                arrays[static_cast<std::int32_t>( Mesh::ARRAY_INDEX )] = indexArray;
            }

            return true;
        }

        /// Depth-first walk of the glTF node graph, accumulating transforms in double and
        /// flattening every mesh into one MeshInstance3D under `parent` (its transform
        /// carries the accumulated world matrix, so node hierarchy depth costs nothing).
        void buildNodes( const core::GltfModel &model, const std::int32_t nodeIndex,
                         const math::Mat4 &parentTransform, Node3D &parent,
                         std::vector<Ref<StandardMaterial3D>> &materialCache,
                         int &meshInstanceCount, std::string &error )
        {
            if ( nodeIndex < 0 || static_cast<std::size_t>( nodeIndex ) >= model.nodes.size() )
            {
                return;
            }

            const core::GltfNodeData &node = model.nodes[static_cast<std::size_t>( nodeIndex )];
            const math::Mat4 worldTransform = math::multiply( parentTransform, node.transform );

            if ( node.mesh >= 0 && static_cast<std::size_t>( node.mesh ) < model.meshes.size() )
            {
                const core::GltfMeshData &meshData =
                    model.meshes[static_cast<std::size_t>( node.mesh )];

                Ref<ArrayMesh> arrayMesh;
                arrayMesh.instantiate();

                std::vector<std::int32_t> surfaceMaterials;

                for ( const std::int32_t primitiveIndex : meshData.primitives )
                {
                    if ( primitiveIndex < 0 ||
                         static_cast<std::size_t>( primitiveIndex ) >= model.primitives.size() )
                    {
                        continue;
                    }

                    const core::GltfPrimitiveData &primitive =
                        model.primitives[static_cast<std::size_t>( primitiveIndex )];

                    const core::GltfMaterialData *material = nullptr;
                    if ( primitive.material >= 0 &&
                         static_cast<std::size_t>( primitive.material ) <
                             model.materials.size() )
                    {
                        material =
                            &model.materials[static_cast<std::size_t>( primitive.material )];
                    }

                    // Diagnostic. Whether shading can even show a lighting error depends on
                    // these flags: an unlit primitive ignores normals completely (unshaded
                    // shading, and buildSurfaceArrays skips generating them), so "convex
                    // geometry looks concave" cannot be a normal problem there - it points
                    // at winding / backface culling instead.
                    {
                        static int reportedPrimitives = 0;
                        if ( reportedPrimitives < 3 )
                        {
                            ++reportedPrimitives;
                            godot::UtilityFunctions::print( godot::vformat(
                                "[Tileset3D] prim: verts=%d indices=%d hasNormal=%d unlit=%d "
                                "doubleSided=%d alphaMode=%d",
                                static_cast<int>( primitive.positions.size() / 3u ),
                                static_cast<int>( primitive.indices.size() ),
                                primitive.normals.empty() ? 0 : 1,
                                material != nullptr && material->unlit ? 1 : 0,
                                material != nullptr && material->doubleSided ? 1 : 0,
                                material != nullptr
                                    ? static_cast<int>( material->alphaMode )
                                    : -1 ) );
                        }
                    }

                    godot::Array arrays;
                    if ( !buildSurfaceArrays( primitive, material, arrays, error ) )
                    {
                        continue;
                    }

                    arrayMesh->add_surface_from_arrays( Mesh::PRIMITIVE_TRIANGLES, arrays );
                    surfaceMaterials.push_back( primitive.material );

                    // Build the material now (shared across primitives via the cache);
                    // attachment happens after all surfaces exist.
                    if ( primitive.material >= 0 )
                    {
                        const std::size_t materialSlot =
                            static_cast<std::size_t>( primitive.material );
                        if ( materialSlot >= materialCache.size() )
                        {
                            materialCache.resize( materialSlot + 1u );
                        }
                        if ( materialCache[materialSlot].is_null() )
                        {
                            materialCache[materialSlot] =
                                buildMaterial( model, primitive.material, error );
                        }
                    }

                    ++meshInstanceCount;
                }

                if ( arrayMesh->get_surface_count() > 0 )
                {
                    MeshInstance3D *meshInstance = memnew( MeshInstance3D );
                    meshInstance->set_transform( toGodotTransform( worldTransform ) );
                    meshInstance->set_mesh( arrayMesh );

                    // Per-surface materials, not material_override: multiple primitives
                    // may share one MeshInstance3D with different materials.
                    for ( std::size_t surface = 0;
                          surface < surfaceMaterials.size() &&
                          surface < static_cast<std::size_t>( arrayMesh->get_surface_count() );
                          ++surface )
                    {
                        const std::int32_t materialIndex = surfaceMaterials[surface];
                        if ( materialIndex >= 0 &&
                             static_cast<std::size_t>( materialIndex ) < materialCache.size() )
                        {
                            meshInstance->set_surface_override_material(
                                static_cast<std::int32_t>( surface ),
                                materialCache[static_cast<std::size_t>( materialIndex )] );
                        }
                    }

                    parent.add_child( meshInstance );
                }
            }

            for ( const std::int32_t child : node.children )
            {
                buildNodes( model, child, worldTransform, parent, materialCache,
                            meshInstanceCount, error );
            }
        }

    } // namespace

    ContentNode createContentNode( const PackedByteArray &bytes, const String &basePath,
                                   const math::Mat4 &worldMatrix )
    {
        // External resources are not resolved (embedded images only), so the base path is
        // no longer needed; the parameter stays for API stability.
        (void)basePath;

        ContentNode result;

        if ( bytes.size() == 0 )
        {
            result.error = "content is empty";
            return result;
        }

        std::vector<std::byte> glb;
        std::optional<math::Vec3> rtcCenter;

        if ( startsWith( bytes, "b3dm", 4 ) )
        {
            const core::B3dmParseResult parsed = core::parseB3dm(
                reinterpret_cast<const std::byte *>( bytes.ptr() ),
                static_cast<std::size_t>( bytes.size() ) );

            if ( !parsed )
            {
                result.error = parsed.error;
                return result;
            }

            const auto *glbBegin =
                reinterpret_cast<const std::byte *>( bytes.ptr() ) + parsed.glbOffset;
            glb.assign( glbBegin, glbBegin + parsed.glbLength );
            rtcCenter = parsed.rtcCenter;
        }
        else if ( startsWith( bytes, "glTF", 4 ) )
        {
            const auto *glbBegin = reinterpret_cast<const std::byte *>( bytes.ptr() );
            glb.assign( glbBegin, glbBegin + static_cast<std::size_t>( bytes.size() ) );
        }
        else
        {
            result.error = "unsupported tile content container (expected b3dm or binary glTF)";
            return result;
        }

        core::GltfModel model;
        if ( !core::parseGltfModel( glb, model, result.error ) )
        {
            return result;
        }

        // Content root: rotationX(+90 deg) is the glTF Y-up to tile Z-up correction, and
        // RTC_CENTER is its origin (expressed in the tile's coordinate system, so it
        // composes after the correction and is not rotated by it).
        math::Mat4 rootTransform{};
        rootTransform[0] = glm::dvec4( 1.0, 0.0, 0.0, 0.0 );
        rootTransform[1] = glm::dvec4( 0.0, 0.0, 1.0, 0.0 );
        rootTransform[2] = glm::dvec4( 0.0, -1.0, 0.0, 0.0 );

        if ( rtcCenter.has_value() )
        {
            rootTransform[3] = glm::dvec4( rtcCenter->x, rtcCenter->y, rtcCenter->z, 1.0 );
        }
        else
        {
            rootTransform[3] = glm::dvec4( 0.0, 0.0, 0.0, 1.0 );
        }

        Node3D *gltfRoot = memnew( Node3D );
        gltfRoot->set_name( "Content" );
        gltfRoot->set_transform( toGodotTransform( rootTransform ) );

        std::vector<Ref<StandardMaterial3D>> materialCache;
        int meshInstanceCount = 0;

        for ( const std::int32_t root : model.sceneNodes )
        {
            buildNodes( model, root, math::identity(), *gltfRoot, materialCache,
                        meshInstanceCount, result.error );
        }

        if ( meshInstanceCount == 0 )
        {
            gltfRoot->queue_free();
            if ( result.error.empty() )
            {
                result.error = "glTF content produced no drawable geometry";
            }
            return result;
        }

        // Report only the first few tiles: this tileset has 373 b3dm payloads and the
        // diagnostic would otherwise flood the output panel.
        {
            static int reported = 0;
            if ( reported < 3 )
            {
                UtilityFunctions::print( godot::vformat(
                    "[Tileset3D] content #%d: glb %d bytes, meshInstances=%d", reported,
                    static_cast<int>( bytes.size() ), meshInstanceCount ) );
                ++reported;
            }
        }

        Node3D *wrapper = memnew( Node3D );
        wrapper->set_name( "TileContent" );
        wrapper->set_transform( toGodotTransform( worldMatrix ) );
        wrapper->add_child( gltfRoot );

        result.node = wrapper;
        result.gltfRoot = gltfRoot;
        return result;
    }

} // namespace tiles3d
