// SPDX-License-Identifier: Unlicense
//
// ContentFactory: tile payload bytes -> a Godot node tree placed in the render frame.
//
// This is the boundary where the engine-agnostic kernel hands over to Godot. The kernel
// deals in bytes and doubles; everything from here on is Resource / Node / float32.

#ifndef CONTENT_FACTORY_H
#define CONTENT_FACTORY_H

#include "core/math/Mat4.h"

#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/string.hpp"

#include <string>

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

    /// Detects the container (b3dm or binary glTF), parses it with the kernel's
    /// GltfReader, and assembles the node tree by hand: one ArrayMesh surface per glTF
    /// primitive, one shared StandardMaterial3D per glTF material, textures decoded from
    /// the embedded image bytes.
    ///
    /// This is deliberately NOT Godot's GLTFDocument: the importer generates
    /// ImporterMeshInstance3D placeholders that it only converts to MeshInstance3D
    /// outside the editor (modules/gltf/register_types.cpp gates the conversion
    /// extension on !is_editor_hint()), drags tiles through the editor image import
    /// pipeline, and re-serializes what the GltfReader already decoded. Hand assembly
    /// behaves identically in editor and at runtime.
    ///
    /// MUST run on the main thread: it creates Resource and Node instances.
    ///
    /// Transform layout of the result:
    ///   wrapper.transform  = worldMatrix
    ///   gltfRoot.transform = rotationX(+90 deg) with origin at RTC_CENTER
    ///
    /// The +90 degree rotation about X is the glTF Y-up to tile Z-up correction, and it is
    /// applied to the *content* only - never to a bounding volume. RTC_CENTER is
    /// deliberately outside that rotation: it is expressed in the tile's coordinate system,
    /// so it composes after the axis correction rather than being carried by it.
    ///
    /// @param bytes raw payload.
    /// @param basePath unused (embedded images only); kept for API stability.
    /// @param worldMatrix the tile's accumulated world matrix.
    ContentNode createContentNode( const godot::PackedByteArray &bytes,
                                  const godot::String &basePath, const math::Mat4 &worldMatrix );

} // namespace tiles3d

#endif
