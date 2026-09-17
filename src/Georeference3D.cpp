// SPDX-License-Identifier: Unlicense

#include "Georeference3D.h"

#include "core/math/GeoMath.h"
#include "core/math/Mat4.h"

#include "godot_cpp/classes/global_constants.hpp"
#include "godot_cpp/core/class_db.hpp"

namespace tiles3d
{
    using godot::Callable;
    using godot::ClassDB;
    using godot::D_METHOD;
    using godot::Object;
    using godot::PropertyInfo;
    using godot::Variant;

    namespace
    {
        constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

        /// The signal the origin resource emits when its value changes, or null for an
        /// unrecognised resource.
        const char *origin_signal_name( const godot::Ref<godot::Resource> &resource )
        {
            if ( Object::cast_to<LongitudeLatitudeHeight>( resource.ptr() ) != nullptr )
            {
                return "geodetic_changed";
            }
            if ( Object::cast_to<EarthCenteredEarthFixed>( resource.ptr() ) != nullptr )
            {
                return "ecef_changed";
            }
            return nullptr;
        }
    } // namespace

    void Georeference3D::_bind_methods()
    {
        ClassDB::bind_method( D_METHOD( "set_origin_authority", "p_origin_authority" ),
                              &Georeference3D::set_origin_authority );
        ClassDB::bind_method( D_METHOD( "get_origin_authority" ),
                              &Georeference3D::get_origin_authority );
        ClassDB::add_property(
            "Georeference3D",
            PropertyInfo( Variant::OBJECT, "origin_authority",
                          godot::PROPERTY_HINT_RESOURCE_TYPE,
                          "LongitudeLatitudeHeight,EarthCenteredEarthFixed" ),
            "set_origin_authority", "get_origin_authority" );

        ClassDB::bind_method( D_METHOD( "set_scale", "p_scale" ), &Georeference3D::set_scale );
        ClassDB::bind_method( D_METHOD( "get_scale" ), &Georeference3D::get_scale );
        ClassDB::add_property( "Georeference3D", PropertyInfo( Variant::FLOAT, "scale" ),
                               "set_scale", "get_scale" );

        // Bound so the origin resources can reach it through a Callable.
        ClassDB::bind_method( D_METHOD( "refresh" ), &Georeference3D::refresh );

        ADD_SIGNAL( godot::MethodInfo( "georeference_changed" ) );
    }

    Georeference3D::Georeference3D() = default;

    // Godot tears down a node's connections itself; doing it by hand here would mean
    // building a Callable pointing at an object mid-destruction.
    Georeference3D::~Georeference3D() = default;

    void Georeference3D::disconnect_origin_authority()
    {
        if ( !origin_authority.is_valid() )
        {
            return;
        }

        if ( const char *signal = origin_signal_name( origin_authority ); signal != nullptr )
        {
            const Callable callable( this, "refresh" );
            if ( origin_authority->is_connected( signal, callable ) )
            {
                origin_authority->disconnect( signal, callable );
            }
        }
    }

    void Georeference3D::set_origin_authority(
        const godot::Ref<godot::Resource> &p_origin_authority )
    {
        if ( origin_authority == p_origin_authority )
        {
            return;
        }

        disconnect_origin_authority();
        origin_authority = p_origin_authority;

        if ( const char *signal = origin_signal_name( origin_authority ); signal != nullptr )
        {
            origin_authority->connect( signal, Callable( this, "refresh" ) );
        }

        refresh();
    }

    godot::Ref<godot::Resource> Georeference3D::get_origin_authority() const
    {
        return origin_authority;
    }

    void Georeference3D::set_scale( const double p_scale )
    {
        if ( scale != p_scale )
        {
            scale = p_scale;
            refresh();
        }
    }

    double Georeference3D::get_scale() const
    {
        return scale;
    }

    math::Vec3 Georeference3D::origin_ecef() const
    {
        if ( const LongitudeLatitudeHeight *geodetic =
                 Object::cast_to<LongitudeLatitudeHeight>( origin_authority.ptr() );
             geodetic != nullptr )
        {
            return math::wgs84ToCartesian( geodetic->get_longitude() * kDegreesToRadians,
                                           geodetic->get_latitude() * kDegreesToRadians,
                                           geodetic->get_height() );
        }

        if ( const EarthCenteredEarthFixed *ecef =
                 Object::cast_to<EarthCenteredEarthFixed>( origin_authority.ptr() );
             ecef != nullptr )
        {
            return math::Vec3( ecef->get_ecef_x(), ecef->get_ecef_y(), ecef->get_ecef_z() );
        }

        // No authority set: sit on the ellipsoid at (longitude 0, latitude 0). That is the
        // default of the EarthCenteredEarthFixed resource too.
        return math::Vec3( math::kWgs84SemiMajorAxis, 0.0, 0.0 );
    }

    const math::Mat4 &Georeference3D::local_to_ecef() const
    {
        if ( !cached_local_to_ecef.has_value() )
        {
            math::Mat4 frame = math::eastNorthUpToFixedFrame( origin_ecef() );

            // `scale` multiplies positions expressed in the local frame, so it scales the
            // three axes. 1.0 (the default) leaves the frame rigid, which is the only value
            // exercised so far.
            if ( scale != 1.0 )
            {
                for ( int axis = 0; axis < 3; ++axis )
                {
                    frame[axis] *= scale;
                }
            }

            cached_local_to_ecef = frame;
        }
        return *cached_local_to_ecef;
    }

    const math::Mat4 &Georeference3D::ecef_to_local() const
    {
        if ( !cached_ecef_to_local.has_value() )
        {
            cached_ecef_to_local = math::invert( local_to_ecef() );
        }
        return *cached_ecef_to_local;
    }

    void Georeference3D::refresh()
    {
        cached_local_to_ecef.reset();
        cached_ecef_to_local.reset();
        emit_signal( "georeference_changed" );
    }

} // namespace tiles3d
