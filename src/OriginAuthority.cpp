// SPDX-License-Identifier: Unlicense

#include "OriginAuthority.h"

#include "godot_cpp/core/class_db.hpp"

namespace tiles3d
{
    using godot::ClassDB;
    using godot::D_METHOD;
    using godot::PropertyInfo;
    using godot::Variant;

    // ---------------------------------------------------------------------------
    // LongitudeLatitudeHeight
    // ---------------------------------------------------------------------------

    void LongitudeLatitudeHeight::_bind_methods()
    {
        ClassDB::bind_method( D_METHOD( "get_longitude" ), &LongitudeLatitudeHeight::get_longitude );
        ClassDB::bind_method( D_METHOD( "set_longitude", "p_longitude" ),
                              &LongitudeLatitudeHeight::set_longitude );
        ClassDB::add_property( "LongitudeLatitudeHeight",
                               PropertyInfo( Variant::FLOAT, "longitude" ), "set_longitude",
                               "get_longitude" );

        ClassDB::bind_method( D_METHOD( "get_latitude" ), &LongitudeLatitudeHeight::get_latitude );
        ClassDB::bind_method( D_METHOD( "set_latitude", "p_latitude" ),
                              &LongitudeLatitudeHeight::set_latitude );
        ClassDB::add_property( "LongitudeLatitudeHeight",
                               PropertyInfo( Variant::FLOAT, "latitude" ), "set_latitude",
                               "get_latitude" );

        ClassDB::bind_method( D_METHOD( "get_height" ), &LongitudeLatitudeHeight::get_height );
        ClassDB::bind_method( D_METHOD( "set_height", "p_height" ),
                              &LongitudeLatitudeHeight::set_height );
        ClassDB::add_property( "LongitudeLatitudeHeight", PropertyInfo( Variant::FLOAT, "height" ),
                               "set_height", "get_height" );

        // Was `lngLatH_changed`. Renamed for readability; the georeference connects to it
        // to know when to rebuild its local frame.
        ADD_SIGNAL( godot::MethodInfo( "geodetic_changed" ) );
    }

    LongitudeLatitudeHeight::LongitudeLatitudeHeight() = default;
    LongitudeLatitudeHeight::~LongitudeLatitudeHeight() = default;

    void LongitudeLatitudeHeight::set_longitude( const double p_longitude )
    {
        if ( longitude != p_longitude )
        {
            longitude = p_longitude;
            emit_signal( "geodetic_changed" );
        }
    }

    double LongitudeLatitudeHeight::get_longitude() const
    {
        return longitude;
    }

    void LongitudeLatitudeHeight::set_latitude( const double p_latitude )
    {
        if ( latitude != p_latitude )
        {
            latitude = p_latitude;
            emit_signal( "geodetic_changed" );
        }
    }

    double LongitudeLatitudeHeight::get_latitude() const
    {
        return latitude;
    }

    void LongitudeLatitudeHeight::set_height( const double p_height )
    {
        if ( height != p_height )
        {
            height = p_height;
            emit_signal( "geodetic_changed" );
        }
    }

    double LongitudeLatitudeHeight::get_height() const
    {
        return height;
    }

    // ---------------------------------------------------------------------------
    // EarthCenteredEarthFixed
    // ---------------------------------------------------------------------------

    void EarthCenteredEarthFixed::_bind_methods()
    {
        ClassDB::bind_method( D_METHOD( "get_ecef_x" ), &EarthCenteredEarthFixed::get_ecef_x );
        ClassDB::bind_method( D_METHOD( "set_ecef_x", "p_ecef_x" ),
                              &EarthCenteredEarthFixed::set_ecef_x );
        ClassDB::add_property( "EarthCenteredEarthFixed", PropertyInfo( Variant::FLOAT, "ecef_x" ),
                               "set_ecef_x", "get_ecef_x" );

        ClassDB::bind_method( D_METHOD( "get_ecef_y" ), &EarthCenteredEarthFixed::get_ecef_y );
        ClassDB::bind_method( D_METHOD( "set_ecef_y", "p_ecef_y" ),
                              &EarthCenteredEarthFixed::set_ecef_y );
        ClassDB::add_property( "EarthCenteredEarthFixed", PropertyInfo( Variant::FLOAT, "ecef_y" ),
                               "set_ecef_y", "get_ecef_y" );

        ClassDB::bind_method( D_METHOD( "get_ecef_z" ), &EarthCenteredEarthFixed::get_ecef_z );
        ClassDB::bind_method( D_METHOD( "set_ecef_z", "p_ecef_z" ),
                              &EarthCenteredEarthFixed::set_ecef_z );
        ClassDB::add_property( "EarthCenteredEarthFixed", PropertyInfo( Variant::FLOAT, "ecef_z" ),
                               "set_ecef_z", "get_ecef_z" );

        ADD_SIGNAL( godot::MethodInfo( "ecef_changed" ) );
    }

    EarthCenteredEarthFixed::EarthCenteredEarthFixed() = default;
    EarthCenteredEarthFixed::~EarthCenteredEarthFixed() = default;

    void EarthCenteredEarthFixed::set_ecef_x( const double p_ecef_x )
    {
        if ( ecef_x != p_ecef_x )
        {
            ecef_x = p_ecef_x;
            emit_signal( "ecef_changed" );
        }
    }

    double EarthCenteredEarthFixed::get_ecef_x() const
    {
        return ecef_x;
    }

    void EarthCenteredEarthFixed::set_ecef_y( const double p_ecef_y )
    {
        if ( ecef_y != p_ecef_y )
        {
            ecef_y = p_ecef_y;
            emit_signal( "ecef_changed" );
        }
    }

    double EarthCenteredEarthFixed::get_ecef_y() const
    {
        return ecef_y;
    }

    void EarthCenteredEarthFixed::set_ecef_z( const double p_ecef_z )
    {
        if ( ecef_z != p_ecef_z )
        {
            ecef_z = p_ecef_z;
            emit_signal( "ecef_changed" );
        }
    }

    double EarthCenteredEarthFixed::get_ecef_z() const
    {
        return ecef_z;
    }

} // namespace tiles3d
