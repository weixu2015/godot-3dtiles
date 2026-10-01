// SPDX-License-Identifier: Unlicense

#include "GlobeFrame.h"

#include "Georeference3D.h"

#include "core/math/GeoMath.h"

#include "godot_cpp/classes/node.hpp"
#include "godot_cpp/classes/node3d.hpp"

namespace tiles3d
{
    namespace
    {
        /// Z-up ECEF -> Y-up. The same flip Tileset3D bakes into its implicit model
        /// matrix: M(v) = (vx, vz, -vy). Column-major.
        math::Mat4 z_up_to_y_up()
        {
            math::Mat4 flip;
            flip[0] = math::Vec4( 1.0, 0.0, 0.0, 0.0 );
            flip[1] = math::Vec4( 0.0, 0.0, -1.0, 0.0 );
            flip[2] = math::Vec4( 0.0, 1.0, 0.0, 0.0 );
            flip[3] = math::Vec4( 0.0, 0.0, 0.0, 1.0 );
            return flip;
        }

        /// Y-up ECEF -> Z-up ECEF for directions: the inverse of the flip above.
        math::Vec3 y_up_to_z_up( const math::Vec3 &v )
        {
            return math::Vec3( v.x, -v.z, v.y );
        }

        math::Vec3 z_up_to_y_up_vec( const math::Vec3 &v )
        {
            return math::Vec3( v.x, v.z, -v.y );
        }
    } // namespace

    GlobeFrame GlobeFrame::resolve( const godot::Node3D *node )
    {
        GlobeFrame frame;
        for ( const godot::Node *ancestor =
                  node != nullptr ? node->get_parent() : nullptr;
              ancestor != nullptr; ancestor = ancestor->get_parent() )
        {
            if ( const Georeference3D *reference =
                     godot::Object::cast_to<Georeference3D>( ancestor ) )
            {
                // The tileset frame: content and globe geometry both live in the
                // georeference's local space, and the georeference node's own transform
                // carries the Y-up flip into the world.
                frame.ecef_to_local_ = reference->ecef_to_local();
                frame.local_to_ecef_ = reference->local_to_ecef();
                return frame;
            }
        }

        // No georeference: the local frame *is* Y-up ECEF, with the flip baked in so a
        // lone globe keeps the orientation geodeticToYUp produces.
        frame.ecef_to_local_ = z_up_to_y_up();
        frame.local_to_ecef_ = math::invert( frame.ecef_to_local_ );
        return frame;
    }

    math::Vec3 GlobeFrame::to_local( const math::Vec3 &ecef_z_up ) const
    {
        return math::transformPoint( ecef_to_local_, ecef_z_up );
    }

    math::Vec3 GlobeFrame::to_ecef_z_up( const math::Vec3 &local ) const
    {
        return math::transformPoint( local_to_ecef_, local );
    }

    math::Vec3 GlobeFrame::camera_ecef_y_up( const math::Vec3 &local ) const
    {
        return z_up_to_y_up_vec( to_ecef_z_up( local ) );
    }

    math::Vec3 GlobeFrame::local_direction( const math::Vec3 &ecef_y_up_direction ) const
    {
        return rotate_local( y_up_to_z_up( ecef_y_up_direction ) );
    }

    math::Vec3 GlobeFrame::ellipsoid_center_local() const
    {
        return to_local( math::Vec3( 0.0 ) );
    }

    math::Vec3 GlobeFrame::rotate_local( const math::Vec3 &ecef_z_up_direction ) const
    {
        // Mat4 has no transformVector helper; casting to a 3x3 keeps the translation out.
        const glm::dmat3 linear( ecef_to_local_ );
        return linear * ecef_z_up_direction;
    }

} // namespace tiles3d
