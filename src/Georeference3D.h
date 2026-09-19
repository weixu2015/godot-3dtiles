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

    private:
        mutable std::optional<math::Mat4> cached_ecef_to_local;
    };

} // namespace tiles3d

#endif
