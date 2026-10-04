// SPDX-License-Identifier: Unlicense
//
// Georeference3D: owns the local frame that 3D Tiles content is expressed in.
//
// Named CesiumGeoreference before the rename (docs/REFACTOR_PLAN.md D-4). It supplies the
// local-to-ECEF transform that Tileset3D needs to place a tileset geographically; the node
// transform itself is what carries the local frame into Godot's Y-up world (the demo scene
// uses a +90 degree rotation about X, which is exactly the Z-up to Y-up flip).
//
// Role in the simplified node model (docs/REFACTOR_PLAN.md): this is the MULTI-SCENE node.
// When several tilesets must be placed by real latitude/longitude relative to one another,
// they are added as sibling children of one Georeference3D so they share a single frame. A
// single tileset with no Georeference3D parent instead uses its own implicit georeference
// (origin centred on its ECEF centre) and needs no Georeference3D at all.

#ifndef GEOREFERENCE_3D_H
#define GEOREFERENCE_3D_H

#include "OriginAuthority.h"

#include "core/math/Types.h"

#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/classes/resource.hpp"

#include <optional>

namespace tiles3d
{
    class Georeference3D : public godot::Node3D
    {
        GDCLASS( Georeference3D, godot::Node3D )

    private:
        godot::Ref<godot::Resource> origin_authority;
        double scale = 1.0;

        /// Rebuilt on demand by local_to_ecef(); mutable because that accessor is const.
        mutable std::optional<math::Mat4> cached_local_to_ecef;

        void disconnect_origin_authority();

    protected:
        static void _bind_methods();

    public:
        Georeference3D();
        ~Georeference3D() override;

        /// Either a LongitudeLatitudeHeight or an EarthCenteredEarthFixed resource.
        void set_origin_authority( const godot::Ref<godot::Resource> &p_origin_authority );
        godot::Ref<godot::Resource> get_origin_authority() const;

        /// Multiplies positions expressed in the local frame. 1.0 (the default) leaves the
        /// frame rigid.
        void set_scale( double p_scale );
        double get_scale() const;

        /// The frame's origin, in ECEF metres.
        math::Vec3 origin_ecef() const;

        /// Local (Z-up east-north-up) to ECEF.
        const math::Mat4 &local_to_ecef() const;

        /// ECEF to local. Cached alongside local_to_ecef().
        const math::Mat4 &ecef_to_local() const;

        /// Drops the cached frame. Bound to ClassDB so the origin resources can call it
        /// through a Callable; also called automatically when the authority changes.
        void refresh();

        // ---- origin shift (floating origin) ----

        /// Moves the local frame's ORIGIN to `p_origin_ecef` **without rotating the frame**.
        ///
        /// This is the floating-origin primitive. The frame's basis stays the ENU basis of the
        /// *declared* anchor; only the translation follows the camera, so moving the origin is
        /// a pure translation of every local coordinate:
        ///
        ///     x_local_new = x_local_old - R^T (origin_new - origin_old)
        ///
        /// Why the rotation has to stay put: `eastNorthUpToFixedFrame()` derives a fresh ENU
        /// basis at whatever point it is given, so re-deriving it per rebase would also rotate
        /// the frame by the meridian convergence between the two points. Over a 1000 m rebase
        /// that is 0.009 degrees - a visible sideways twitch of the whole scene - and it grows
        /// linearly, so a 1000 km rebase would twist everything by about a degree. Freezing the
        /// basis makes every rebase exactly translatory, which is what lets the camera be
        /// compensated with the same vector and the picture not move at all.
        ///
        /// `refresh()` clears the rebase, so changing the origin authority (the demo's dataset
        /// switch) still re-derives a proper ENU frame at the new anchor.
        void rebase_origin_ecef( const godot::Vector3 &p_origin_ecef );

        /// The frame's effective origin in ECEF metres: the rebased one while a rebase is in
        /// effect, otherwise the declared anchor's position.
        godot::Vector3 get_frame_origin_ecef() const;

        /// True while the origin sits somewhere other than the declared anchor.
        bool is_rebased() const;

    private:
        /// Set by rebase_origin_ecef(); cleared by refresh().
        mutable std::optional<math::Vec3> rebased_origin_;

        mutable std::optional<math::Mat4> cached_ecef_to_local;
    };

} // namespace tiles3d

#endif
