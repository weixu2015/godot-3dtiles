// SPDX-License-Identifier: Unlicense

#include "tiles/Tile.h"

#include <array>

namespace tiles3d::core
{
    namespace
    {
        /// Rewrites one bounding volume in place. Non-region volumes are left untouched,
        /// which is exactly what the reference does (its `convert` returns the input
        /// unchanged for box and sphere).
        void convertVolumeToLocalFrame( math::BoundingVolume &volume, const math::Mat4 &modelMatrix,
                                        const math::Mat3 &worldLinear,
                                        const math::Mat4 &chainInverse )
        {
            if ( volume.type != math::BoundingVolume::Type::Region )
            {
                return;
            }

            const math::OrientedBoundingBox box = math::regionToEcefObb( volume.asRegion() );

            // The region's place in the render frame is modelMatrix * ECEF; bring it back
            // into the tile's local frame with the chain inverse.
            const math::Vec3 worldCenter = math::transformPoint( modelMatrix, box.center );
            const math::Vec3 localCenter = math::transformPoint( chainInverse, worldCenter );

            const math::Mat3 chainLinear = math::linearPart( chainInverse );

            std::array<math::Vec3, 3> localHalfAxes{};
            for ( int axis = 0; axis < 3; ++axis )
            {
                localHalfAxes[static_cast<std::size_t>( axis )] =
                    math::transformLinear( chainLinear,
                                           math::transformLinear(
                                               worldLinear,
                                               box.halfAxes[static_cast<std::size_t>( axis )] ) );
            }

            volume = math::BoundingVolume::fromBox( localCenter, localHalfAxes[0], localHalfAxes[1],
                                                    localHalfAxes[2] );
        }

        void convertSubtree( Tile &tile, const math::Mat4 &modelMatrix, const math::Mat3 &worldLinear,
                             const math::Mat4 &parentChain )
        {
            const math::Mat4 chain = math::multiply( parentChain, tile.transform );
            const math::Mat4 chainInverse = math::invert( chain );

            if ( tile.boundingVolume )
            {
                convertVolumeToLocalFrame( *tile.boundingVolume, modelMatrix, worldLinear,
                                           chainInverse );
                tile.invalidateBoundingVolumeRadius();
            }

            if ( tile.content && tile.content->boundingVolume )
            {
                convertVolumeToLocalFrame( *tile.content->boundingVolume, modelMatrix, worldLinear,
                                           chainInverse );
            }

            for ( const std::unique_ptr<Tile> &child : tile.children )
            {
                convertSubtree( *child, modelMatrix, worldLinear, chain );
            }
        }
    } // namespace

    Tile::Tile() = default;

    // Out of line so that unique_ptr<Tile> children can be destroyed with Tile complete.
    Tile::~Tile() = default;

    double Tile::boundingVolumeRadius() const
    {
        if ( !cachedBoundingVolumeRadius.has_value() )
        {
            cachedBoundingVolumeRadius =
                boundingVolume.has_value() ? math::boundingVolumeRadius( *boundingVolume ) : 0.0;
        }
        return *cachedBoundingVolumeRadius;
    }

    void Tile::invalidateBoundingVolumeRadius() const
    {
        cachedBoundingVolumeRadius.reset();
    }

    const char *Tile::contentStateName( ContentState state )
    {
        switch ( state )
        {
            case ContentState::Unloaded:
                return "unloaded";
            case ContentState::Loading:
                return "loading";
            case ContentState::Processing:
                return "processing";
            case ContentState::Ready:
                return "ready";
            case ContentState::Expired:
                return "expired";
            case ContentState::Failed:
                return "failed";
        }
        return "unknown";
    }

    void convertRegionBoundingVolumes( Tile &tile, const math::Mat4 &modelMatrix,
                                       const math::Mat4 &parentChain )
    {
        // The world linear part is constant for the whole subtree, so it is hoisted out of
        // the recursion rather than recomputed per bounding volume as the reference does.
        convertSubtree( tile, modelMatrix, math::linearPart( modelMatrix ), parentChain );
    }

} // namespace tiles3d::core
