// SPDX-License-Identifier: Unlicense
//
// GltfReader: the engine-agnostic glTF content model.
//
// Parses a binary glTF (the payload inside a b3dm, or a bare .glb) into plain
// SoA vertex data plus a small material / texture / node-graph description.
// The Godot layer turns that into ArrayMesh + StandardMaterial3D; this layer
// never touches a Godot header.
//
// Scope is deliberate: the photogrammetry subsets of glTF that 3D Tiles
// actually ships. Draco-compressed primitives are decoded straight into the
// vertex buffers (no glTF re-serialization, unlike the previous transcoder).
// Anything unsupported fails with a named reason instead of rendering garbage.
//
// Ported from GodotPrepareRendererResources (cesium-native era) and the Draco
// transcoder; behaviour for supported features matches three.js
// THREE.GLTFLoader as used by the reference threeDTiles scheduler.

#ifndef TILES3D_CORE_CONTENT_GLTFREADER_H
#define TILES3D_CORE_CONTENT_GLTFREADER_H

#include "math/Types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tiles3d::core
{
    /// glTF alphaMode.
    enum class AlphaMode : std::int32_t
    {
        Opaque = 0,
        Mask = 1,
        Blend = 2,
    };

    struct GltfImageData
    {
        /// Bytes of the image, as a span into GltfModel::bin (embedded image).
        std::size_t offset = 0;
        std::size_t length = 0;
        std::string mimeType; // "image/jpeg" / "image/png"; empty when unspecified
    };

    struct GltfTextureData
    {
        std::int32_t imageIndex = -1;
        std::int32_t samplerIndex = -1;

        // Resolved sampler parameters, glTF defaults when the sampler omits them.
        std::uint32_t minFilter = 9987u; // LINEAR_MIPMAP_LINEAR
        std::uint32_t magFilter = 9729u; // LINEAR
        std::uint32_t wrapS = 10497u;    // REPEAT
        std::uint32_t wrapT = 10497u;    // REPEAT
    };

    struct GltfMaterialData
    {
        float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        std::int32_t baseColorTexture = -1; // index into GltfModel::textures, -1 = none

        bool unlit = false; // KHR_materials_unlit
        AlphaMode alphaMode = AlphaMode::Opaque;
        float alphaCutoff = 0.5f;
        bool doubleSided = false;
    };

    /// One glTF primitive, decoded. Vertex attributes are per-attribute arrays
    /// (structure of arrays) sized to the vertex count; a missing attribute is an
    /// empty vector. Indices are always materialized: a non-indexed primitive gets
    /// the sequential index list, and TRIANGLE_STRIP / TRIANGLE_FAN are expanded to
    /// TRIANGLES exactly like the cesium-native era loader did.
    struct GltfPrimitiveData
    {
        std::vector<float> positions;   // 3 per vertex
        std::vector<float> normals;     // 3 per vertex, empty when absent
        std::vector<float> texcoords0;  // 2 per vertex, empty when absent
        std::vector<float> colors;      // 4 per vertex, empty when absent
        std::vector<std::uint32_t> indices;

        std::int32_t material = -1;
    };

    struct GltfMeshData
    {
        std::vector<std::int32_t> primitives; // indices into GltfModel::primitives
    };

    struct GltfNodeData
    {
        std::int32_t mesh = -1;
        math::Mat4 transform{}; // glTF local transform (matrix or composed TRS)
        std::vector<std::int32_t> children;
    };

    struct GltfModel
    {
        std::vector<GltfPrimitiveData> primitives;
        std::vector<GltfMeshData> meshes;
        std::vector<GltfMaterialData> materials;
        std::vector<GltfTextureData> textures;
        std::vector<GltfImageData> images;
        std::vector<GltfNodeData> nodes;
        std::vector<std::int32_t> sceneNodes; // roots of the default scene

        /// The GLB binary chunk. Image spans point into it, so it must outlive every
        /// GltfImageData view taken from this model.
        std::vector<std::byte> bin;
    };

    /// Parses a binary glTF into the content model. The core layer does not throw;
    /// failures come back as `false` with a human readable reason in `error`.
    bool parseGltfModel( const std::vector<std::byte> &glb, GltfModel &out, std::string &error );

} // namespace tiles3d::core

#endif
