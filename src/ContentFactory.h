// SPDX-License-Identifier: Unlicense
//
// ContentFactory: tile payload bytes -> a Godot node tree placed in the render frame.
//
// This is the boundary where the engine-agnostic kernel hands over to Godot. The kernel
// deals in bytes and doubles; everything from here on is Resource / Node / float32.

#ifndef CONTENT_FACTORY_H
#define CONTENT_FACTORY_H

#include "core/content/GltfReader.h"
#include "core/math/Mat4.h"
#include "core/tiles/TilesetJson.h"

#include "godot_cpp/classes/mesh.hpp"
#include "godot_cpp/classes/material.hpp"
#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/string.hpp"

#include <optional>
#include <string>
#include <vector>

namespace tiles3d
{
    struct ContentNode
    {
        /// Wrapper holding the generated glTF scene, already placed in the render frame.
        /// Null on failure. The caller takes ownership: add it to the tree and free it when
        /// the tile is unloaded.
        godot::Node3D *node = nullptr;

        /// The generated glTF scene root, a child of `node`. Exposed so callers (and the
        /// spike) can inspect the axis correction and RTC_CENTER placement.
        godot::Node3D *gltfRoot = nullptr;

        /// Empty on success.
        std::string error;

        explicit operator bool() const
        {
            return node != nullptr;
        }
    };

    /// Result of the engine-free half of content assembly.
    ///
    /// This split exists so the expensive work - unwrapping the container, running Draco and
    /// KTX2 over the mesh - can happen on a worker thread, while the part that must touch the
    /// scene tree stays on the main thread. `GltfModel` holds only std::vector / std::span /
    /// plain structs, so it is safe to build in parallel and hand across the boundary.
    struct DecodedTileContent
    {
        core::GltfModel model;

        /// RTC_CENTER as declared by the b3dm container, in the tile's coordinate system.
        std::optional<math::Vec3> rtcCenter;

        /// Empty on success.
        std::string error;
    };

    /// Unwraps a b3dm or binary glTF payload and decodes it fully - including Draco vertex
    /// decompression and KTX2 transcoding - without touching any Godot type.
    ///
    /// SAFE TO CALL FROM A WORKER THREAD.
    DecodedTileContent decodeTileContent( const godot::PackedByteArray &bytes );

    /// One glTF mesh node: where it sits and which prepared primitive it draws.
    ///
    /// Mirrors the two-stage split in cesium-native's IPrepareRendererResources, where the
    /// load thread produces meshes and the main thread only instantiates scene nodes. Godot's
    /// ArrayMesh / StandardMaterial3D / ImageTexture are RefCounted resources that are not in
    /// the scene tree, so building them off-thread is safe - which is exactly what the
    /// cesium-native-era Godot backend did in its own prepareInLoadThread.
    struct PreparedMeshNode
    {
        /// Local transform relative to the content root, up-axis correction already applied.
        math::Mat4 transform = math::identity();

        /// Index into `PreparedContent::primitives`.
        std::int32_t primitive = -1;
    };

    /// One mesh's geometry and the materials its surfaces use.
    struct PreparedPrimitive
    {
        godot::Ref<godot::Mesh> mesh;

        /// Material index per surface, indexing `PreparedContent::materials`. -1 for none.
        std::vector<std::int32_t> surfaceMaterials;
    };

    /// Everything the main thread needs to instantiate a tile's content, produced entirely on
    /// a worker thread.
    struct PreparedContent
    {
        /// Content root transform: the up-axis correction with RTC_CENTER as its origin.
        math::Mat4 rootTransform = math::identity();

        /// Node tree of the glTF scene, flattened into draw calls with their transforms.
        std::vector<PreparedMeshNode> nodes;

        /// Meshes, in the order `nodes` refers to them through PreparedMeshNode.
        std::vector<PreparedPrimitive> primitives;

        /// Shared materials, one per glTF material actually used.
        std::vector<godot::Ref<godot::Material>> materials;

        /// Empty on success.
        std::string error;
    };

    /// Turns a decoded payload into Godot *resources* (meshes, materials, textures) without
    /// creating a single scene node.
    ///
    /// SAFE TO CALL FROM A WORKER THREAD. The returned refs are owned by the caller until it
    /// hands them to the main thread, and none of them are in the scene tree.
    PreparedContent prepareContentResources( const DecodedTileContent &decoded,
                                             core::ModelUpAxis upAxis );

    /// Instantiates the scene nodes for prepared content and places the result in the render
    /// frame.
    ///
    /// MUST run on the main thread: it creates Node instances and parents them.
    ContentNode assembleContentNode( const PreparedContent &prepared,
                                     const math::Mat4 &worldMatrix );

    /// Convenience: decode + prepare + assemble in one call, for callers that have the bytes
    /// in hand and no reason to split the stages. Equivalent to calling the three in order.
    ///
    /// Transform layout of the result:
    ///   wrapper.transform  = worldMatrix
    ///   gltfRoot.transform = <up axis correction> with origin at RTC_CENTER
    ///
    /// The correction depends on `upAxis`, which comes from the tileset's
    /// `asset.gltfUpAxis`:
    ///
    ///   Y  +90 deg about X, i.e. M(v) = (vx, -vz, vy). The classic glTF Y-up -> tile Z-up
    ///      correction, and the default for every dataset that does not declare an axis.
    ///   Z  identity. The content is already Z-up and must NOT be rotated.
    ///   X  -90 deg about Y, i.e. M(v) = (-vz, vy, vx), taking glTF X-up to tile Z-up.
    ///
    /// It is applied to the *content* only - never to a bounding volume. RTC_CENTER is
    /// deliberately outside the rotation: it is expressed in the tile's coordinate system,
    /// so it composes after the axis correction rather than being carried by it.
    ///
    /// NOTE the rotation is about the glTF origin, so content whose vertices carry full
    /// tile-frame coordinates - far from that origin - is displaced when it *is* rotated.
    /// That is precisely why the declared axis has to be honoured: a Z-up dataset must skip
    /// the correction entirely rather than have the displacement patched up afterwards.
    ContentNode createContentNode( const godot::PackedByteArray &bytes,
                                   const math::Mat4 &worldMatrix,
                                   core::ModelUpAxis upAxis = core::ModelUpAxis::Y );

} // namespace tiles3d

#endif
