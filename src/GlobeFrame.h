// SPDX-License-Identifier: Unlicense
//
// GlobeFrame: the one world frame every globe node (Globe3D, GlobeTileLayer,
// GlobeCameraController) and every Tileset3D shares.
//
// Why this exists: 3D Tiles content is authored in Z-up ECEF, Godot is Y-up, and the
// kernel already ships two different conversions (eastNorthUpToFixedFrame for tilesets,
// geodeticToYUp for globe geometry). Nodes that mix them silently drift apart - the globe
// has orbited a point one Earth radius off its own mesh because of exactly that. This
// struct is the single place where ECEF meets a scene frame, so every consumer multiplies
// by the *same* matrix:
//
//   - Under a Georeference3D the frame is exactly the tileset frame: reference->ecef_to_local()
//     (Z-up ENU, tile content and globe geometry both land in it, and the Georeference3D
//     node's own -90 deg X transform carries the Y-up flip into the world).
//   - With no Georeference3D the flip is baked in instead (local space IS Y-up ECEF, the
//     same convention geodeticToYUp emits), which keeps a lone globe working unchanged.
//
// All matrices are kernel Mat4 (column-major, glm::dmat4).

#ifndef GLOBE_FRAME_H
#define GLOBE_FRAME_H

#include "core/math/Mat4.h"
#include "core/math/Types.h"

namespace godot
{
    class Node;
    class Node3D;
}

namespace tiles3d
{
    class GlobeFrame
    {
    public:
        /// Walks `node`'s ancestors for a Georeference3D and builds the shared frame.
        /// Cheap enough to call per frame (the georeference caches its matrices).
        static GlobeFrame resolve( const godot::Node3D *node );

        /// The frame a node *beside* the carrier has to use.
        ///
        /// resolve() only walks ancestors, so a node that sits next to the Georeference3D
        /// instead of under it - the globe camera, which is a sibling by design - gets the
        /// fallback frame and then computes correct-looking distances to completely wrong
        /// directions. This builds the frame from the node that actually carries it:
        /// a Georeference3D contributes its ENU matrices, a Globe3D the frame it resolved
        /// for itself. `carrier == nullptr` yields the same fallback as resolve().
        static GlobeFrame from_carrier( const godot::Node *carrier );

        /// Z-up ECEF metres -> the node parent space the mesh is built in.
        math::Vec3 to_local( const math::Vec3 &ecef_z_up ) const;

        /// Inverse of to_local.
        math::Vec3 to_ecef_z_up( const math::Vec3 &local ) const;

        /// Camera position expressed the way the horizon culling wants it: Y-up ECEF,
        /// metres from the ellipsoid centre.
        math::Vec3 camera_ecef_y_up( const math::Vec3 &local ) const;

        /// A Y-up ECEF direction (e.g. an orbit direction from orbit_to) expressed in
        /// the local frame. Rotation only; caller normalises.
        math::Vec3 local_direction( const math::Vec3 &ecef_y_up_direction ) const;

        /// Where the ellipsoid's centre lands in the local frame.
        math::Vec3 ellipsoid_center_local() const;

        /// Linear part of the frame, for rotating normals/directions.
        math::Vec3 rotate_local( const math::Vec3 &ecef_z_up_direction ) const;

        math::Mat4 ecef_to_local_{};
        math::Mat4 local_to_ecef_{};
    };

} // namespace tiles3d

#endif
