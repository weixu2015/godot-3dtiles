// SPDX-License-Identifier: Unlicense
//
// Origin resources for the georeference: either a geodetic position or a raw ECEF one.
//
// These were called `CesiumOriginAuthority` before the rename (docs/REFACTOR_PLAN.md
// D-4). The class names themselves are standard geodesy terms, not Cesium vocabulary, so
// they stay; only the namespace and the signal name changed.

#ifndef ORIGIN_AUTHORITY_H
#define ORIGIN_AUTHORITY_H

#include "core/math/GeoMath.h"

#include "godot_cpp/classes/resource.hpp"
#include "godot_cpp/variant/string.hpp"

namespace tiles3d
{
    /// The origin of the local coordinate system given as longitude, latitude and height.
    ///
    /// Longitude and latitude are in degrees (range -180..180 and -90..90), height is in
    /// metres above the ellipsoid - not above mean sea level, which can differ by tens of
    /// metres.
    class LongitudeLatitudeHeight : public godot::Resource
    {
        GDCLASS( LongitudeLatitudeHeight, godot::Resource )

    private:
        double longitude = 0.0;
        double latitude = 0.0;
        double height = 0.0;

    protected:
        static void _bind_methods();

    public:
        LongitudeLatitudeHeight();
        ~LongitudeLatitudeHeight() override;

        void set_longitude( double p_longitude );
        double get_longitude() const;

        void set_latitude( double p_latitude );
        double get_latitude() const;

        void set_height( double p_height );
        double get_height() const;
    };

    /// The origin of the local coordinate system given directly in earth-centred,
    /// earth-fixed coordinates (metres).
    class EarthCenteredEarthFixed : public godot::Resource
    {
        GDCLASS( EarthCenteredEarthFixed, godot::Resource )

    private:
        // Default origin: on the ellipsoid at longitude 0, latitude 0.
        double ecef_x = math::kWgs84SemiMajorAxis;
        double ecef_y = 0.0;
        double ecef_z = 0.0;

    protected:
        static void _bind_methods();

    public:
        EarthCenteredEarthFixed();
        ~EarthCenteredEarthFixed() override;

        void set_ecef_x( double p_ecef_x );
        double get_ecef_x() const;

        void set_ecef_y( double p_ecef_y );
        double get_ecef_y() const;

        void set_ecef_z( double p_ecef_z );
        double get_ecef_z() const;
    };

} // namespace tiles3d

#endif
