// SPDX-License-Identifier: Unlicense

#include "GlobeTile.h"

#include "core/math/GeoMath.h"
#include "core/math/EllipsoidalOccluder.h"

#include <glm/geometric.hpp>

#include <algorithm>

namespace tiles3d
{
    GlobeTile::GlobeTile( const std::int64_t p_x, const std::int64_t p_y, const int p_level,
                          GlobeTile *p_parent )
        : x( p_x ),
          y( p_y ),
          level( p_level ),
          parent( p_parent ),
          // The imagery rectangle must be computed here, not filled in later by the
          // layer: children are created lazily, and a rectangle assigned only to the root
          // would leave every descendant collapsed to a point (the reference documents
          // exactly this failure as black tiles + NaN UVs).
          rectangle( math::tileXYToRectangle( p_x, p_y, p_level ) )
    {
    }

    GlobeTile::~GlobeTile()
    {
        delete nw;
        delete ne;
        delete sw;
        delete se;
    }

    math::TileRectangle GlobeTile::geometry_rectangle() const
    {
        const std::int64_t n = std::int64_t{ 1 } << level;
        math::TileRectangle g = rectangle;
        if ( y == 0 )
        {
            g.north = math::kPi / 2.0;
        }
        if ( y == n - 1 )
        {
            g.south = -math::kPi / 2.0;
        }
        return g;
    }

    GlobeTile *GlobeTile::child( const int index )
    {
        if ( nw == nullptr )
        {
            nw = new GlobeTile( x * 2, y * 2, level + 1, this );
            ne = new GlobeTile( x * 2 + 1, y * 2, level + 1, this );
            sw = new GlobeTile( x * 2, y * 2 + 1, level + 1, this );
            se = new GlobeTile( x * 2 + 1, y * 2 + 1, level + 1, this );
        }
        switch ( index )
        {
            case 0:
                return nw;
            case 1:
                return ne;
            case 2:
                return sw;
            default:
                return se;
        }
    }

    int GlobeTile::child_count() const
    {
        return nw != nullptr ? 4 : 0;
    }

    void GlobeTile::compute_bounds( const GlobeFrame &frame )
    {
        if ( bounds_computed )
        {
            return;
        }

        // Nine samples: corners, edge midpoints, centre - the same set the reference
        // feeds to the horizon culling point.
        const math::TileRectangle g = geometry_rectangle();
        const double samples[9][2] = {
            { g.west, g.north },                                  //
            { g.east, g.north },                                  //
            { g.west, g.south },                                  //
            { g.east, g.south },                                  //
            { ( g.west + g.east ) / 2.0, ( g.north + g.south ) / 2.0 },
            { ( g.west + g.east ) / 2.0, g.north },               //
            { ( g.west + g.east ) / 2.0, g.south },               //
            { g.west, ( g.north + g.south ) / 2.0 },              //
            { g.east, ( g.north + g.south ) / 2.0 },              //
        };

        // Sphere centre sits on the ellipsoid under the rectangle centre, mapped into the
        // shared frame (the latitude comes from the *geometry* rectangle so stretched polar
        // rows centre correctly). Radii are measured in ECEF and survive the rigid mapping.
        const math::Vec3 center = math::wgs84ToCartesian(
            ( rectangle.west + rectangle.east ) / 2.0, ( g.north + g.south ) / 2.0, 0.0 );

        double radius = 0.0;
        for ( int i = 0; i < 9; ++i )
        {
            const math::Vec3 p = math::wgs84ToCartesian( samples[i][0], samples[i][1], 0.0 );
            radius = std::max( radius, glm::length( p - center ) );
        }

        bounds_center = frame.to_local( center );
        bounds_radius = radius;
        bounds_computed = true;

        const math::HorizonCullingPoint point = math::computeHorizonCullingPointFromRectangle(
            g.west, g.south, g.east, g.north, &samples[0][0], 9 );
        occludee_valid = point.valid;
        occludee_point = point.position;
    }

    void GlobeTile::free_resources()
    {
        texture = godot::Ref<godot::ImageTexture>();
        // The layer detaches and queue_frees the MeshInstance3D before calling this; the
        // pointer is cleared here so a later frame cannot touch a freed node.
        mesh = nullptr;
        base_uvs = godot::PackedVector2Array();
        appearance_source = nullptr;
        texture_version = -1;
        renderable = false;
        state = LoadState::START;
        fail_time = 0.0;
        // Bounds survive: they are pure geometry and expensive to recompute.
    }

    // ---- TileReplacementQueue ----

    void TileReplacementQueue::mark_start_of_render_frame()
    {
        last_before_start_of_frame = head;
    }

    void TileReplacementQueue::unlink( GlobeTile *tile )
    {
        GlobeTile *previous = tile->replacement_previous;
        GlobeTile *next = tile->replacement_next;
        if ( previous != nullptr )
        {
            previous->replacement_next = next;
        }
        else
        {
            head = next;
        }
        if ( next != nullptr )
        {
            next->replacement_previous = previous;
        }
        else
        {
            tail = previous;
        }
        tile->replacement_previous = nullptr;
        tile->replacement_next = nullptr;
        tile->in_replacement_queue = false;
        --count;
    }

    void TileReplacementQueue::mark_tile_rendered( GlobeTile *tile )
    {
        if ( tile->in_replacement_queue )
        {
            if ( head == tile )
            {
                return;
            }
            unlink( tile );
        }
        tile->replacement_previous = nullptr;
        tile->replacement_next = head;
        if ( head != nullptr )
        {
            head->replacement_previous = tile;
        }
        else
        {
            tail = tile;
        }
        head = tile;
        tile->in_replacement_queue = true;
        if ( tile->replacement_next == nullptr )
        {
            tail = tile;
        }
        ++count;
    }

    void TileReplacementQueue::remove( GlobeTile *tile )
    {
        if ( tile->in_replacement_queue )
        {
            unlink( tile );
        }
    }

    int TileReplacementQueue::trim_tiles( const int maximum_tiles,
                                          const std::function<bool( GlobeTile * )> &is_protected,
                                          const std::function<void( GlobeTile * )> &on_trim )
    {
        int trimmed = 0;
        GlobeTile *node = tail;
        // Tiles rendered before the start of this frame are protected: they belong to a
        // traversal that has not run yet, and freeing them would thrash the cache.
        while ( count > maximum_tiles && node != nullptr && node != last_before_start_of_frame )
        {
            GlobeTile *previous = node->replacement_previous;
            if ( !is_protected( node ) )
            {
                unlink( node );
                on_trim( node );
                ++trimmed;
            }
            node = previous;
        }
        return trimmed;
    }

    void TileReplacementQueue::clear()
    {
        head = nullptr;
        tail = nullptr;
        count = 0;
        last_before_start_of_frame = nullptr;
    }

} // namespace tiles3d
