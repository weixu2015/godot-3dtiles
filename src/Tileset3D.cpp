// SPDX-License-Identifier: Unlicense

#include "Tileset3D.h"

#include "ContentFactory.h"
#include "Globe3D.h"
#include "GodotMathConvert.h"

#include "core/io/Url.h"
#include "core/math/BoundingVolume.h"
#include "core/math/Mat4.h"
#include "core/math/ScreenSpaceError.h"
#include "core/tiles/Subtree.h"
#include "core/tiles/TilesetJson.h"

#include <nlohmann/json.hpp>

#include <glm/geometric.hpp>

#include "godot_cpp/classes/array_mesh.hpp"
#include "godot_cpp/classes/base_material3d.hpp"
#include "godot_cpp/classes/camera3d.hpp"
// EditorInterface / SubViewport are only linked in an editor build (see the
// TILES3D_EDITOR_TARGET note in the top level CMakeLists).
#ifdef TILES3D_EDITOR_TARGET
#include "godot_cpp/classes/editor_interface.hpp"
#include "godot_cpp/classes/sub_viewport.hpp"
#endif
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/file_access.hpp"
#include "godot_cpp/classes/http_client.hpp"
#include "godot_cpp/classes/mesh.hpp"
#include "godot_cpp/classes/standard_material3d.hpp"
#include "godot_cpp/classes/viewport.hpp"
#include "godot_cpp/classes/scene_tree.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/memory.hpp"
#include "godot_cpp/variant/array.hpp"
#include "godot_cpp/variant/color.hpp"
#include "godot_cpp/variant/packed_color_array.hpp"
#include "godot_cpp/variant/packed_string_array.hpp"
#include "godot_cpp/variant/packed_vector3_array.hpp"
#include "godot_cpp/variant/utility_functions.hpp"

#include "core/math/GeoMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace tiles3d
{
    using godot::Array;
    using godot::ArrayMesh;
    using godot::BaseMaterial3D;
    using godot::ClassDB;
    using godot::Color;
    using godot::D_METHOD;
    using godot::FileAccess;
    using godot::Mesh;
    using godot::MeshInstance3D;
    using godot::PackedByteArray;
    using godot::PackedColorArray;
    using godot::PackedVector3Array;
    using godot::PropertyInfo;
    using godot::Ref;
    using godot::StandardMaterial3D;
    using godot::String;
    using godot::UtilityFunctions;
    using godot::Variant;
    using godot::Vector3;

    namespace
    {
        /// Emits the 12 edges of a bounding volume, transformed by `world`.
        ///
        /// Boxes use their true oriented box. Spheres are approximated by their
        /// axis-aligned cube - good enough for a structural view, and regions never reach
        /// here because convertRegionBoundingVolumes turns them into boxes first.
        void appendVolumeEdges( const math::Mat4 &world, const math::BoundingVolume &volume,
                                double scale, int depth, bool colorize,
                                PackedVector3Array &vertices, PackedColorArray &colors )
        {
            std::array<math::Vec3, 8> corners{};

            if ( volume.type == math::BoundingVolume::Type::Box )
            {
                const math::Vec3 center = math::boundingVolumeCenter( volume );
                const math::Vec3 axisX = volume.boxHalfAxis( 0 ) * scale;
                const math::Vec3 axisY = volume.boxHalfAxis( 1 ) * scale;
                const math::Vec3 axisZ = volume.boxHalfAxis( 2 ) * scale;

                for ( int index = 0; index < 8; ++index )
                {
                    const double signX = ( index & 1 ) != 0 ? 1.0 : -1.0;
                    const double signY = ( index & 2 ) != 0 ? 1.0 : -1.0;
                    const double signZ = ( index & 4 ) != 0 ? 1.0 : -1.0;

                    corners[static_cast<std::size_t>( index )] =
                        center + axisX * signX + axisY * signY + axisZ * signZ;
                }
            }
            else if ( volume.type == math::BoundingVolume::Type::Sphere )
            {
                const math::Vec3 center = math::boundingVolumeCenter( volume );
                const double radius = volume.data[3] * scale;

                for ( int index = 0; index < 8; ++index )
                {
                    corners[static_cast<std::size_t>( index )] =
                        center + math::Vec3( ( index & 1 ) != 0 ? radius : -radius,
                                             ( index & 2 ) != 0 ? radius : -radius,
                                             ( index & 4 ) != 0 ? radius : -radius );
                }
            }
            else
            {
                return;
            }

            // Colour by tree depth so the level of detail structure is readable at a glance.
            // Explicit floats: Color::from_hsv takes float, and C4305 (double to float
            // truncation) is fatal here because the target builds with /WX.
            const Color color = colorize
                                    ? Color::from_hsv( static_cast<float>( std::fmod( 0.11 * depth, 1.0 ) ),
                                                      0.85f, 1.0f )
                                    : Color( 0.2f, 0.8f, 1.0f );

            // A cube has 12 edges: for every corner, the three neighbours whose index
            // differs by one bit, emitted once.
            for ( int index = 0; index < 8; ++index )
            {
                for ( int bit = 1; bit <= 4; bit <<= 1 )
                {
                    const int neighbour = index ^ bit;
                    if ( neighbour <= index )
                    {
                        continue;
                    }

                    vertices.push_back( toGodotVector(
                        math::transformPoint( world, corners[static_cast<std::size_t>( index )] ) ) );
                    vertices.push_back( toGodotVector(
                        math::transformPoint( world, corners[static_cast<std::size_t>( neighbour )] ) ) );
                    colors.push_back( color );
                    colors.push_back( color );
                }
            }
        }

        void collectTileEdges( const core::Tile &tile, const math::Mat4 &parentWorld, double scale,
                               bool colorize, PackedVector3Array &vertices,
                               PackedColorArray &colors )
        {
            const math::Mat4 world = math::multiply( parentWorld, tile.transform );

            if ( tile.boundingVolume.has_value() )
            {
                appendVolumeEdges( world, *tile.boundingVolume, scale, tile.depth, colorize,
                                   vertices, colors );
            }

            for ( const std::unique_ptr<core::Tile> &child : tile.children )
            {
                collectTileEdges( *child, world, scale, colorize, vertices, colors );
            }
        }

        void countTilesRecursive( const core::Tile &tile, std::size_t &count, int &maxDepth )
        {
            ++count;
            maxDepth = std::max( maxDepth, tile.depth );

            for ( const std::unique_ptr<core::Tile> &child : tile.children )
            {
                countTilesRecursive( *child, count, maxDepth );
            }
        }

        void dumpRecursive( const core::Tile &tile, int maxDepth, int indent )
        {
            if ( tile.depth > maxDepth )
            {
                return;
            }

            String line;
            for ( int i = 0; i < indent; ++i )
            {
                line += "  ";
            }

            line += godot::vformat( "tile %d  depth=%d  ge=%.4f  refine=%s", (int)tile.id, tile.depth,
                                    tile.geometricError,
                                    tile.refine == core::RefineMode::Replace ? "REPLACE" : "ADD" );

            if ( tile.boundingVolume.has_value() )
            {
                const math::BoundingVolume &volume = *tile.boundingVolume;
                const char *kind = volume.type == math::BoundingVolume::Type::Box      ? "box"
                                   : volume.type == math::BoundingVolume::Type::Region ? "region"
                                                                                      : "sphere";
                line += String( "  bv=" ) + kind;
                line += godot::vformat( "  radius=%.2f", tile.boundingVolumeRadius() );
            }

            if ( tile.content.has_value() )
            {
                line += "  content=" + String( tile.content->uri.c_str() );
            }
            else if ( tile.implicitTiling.has_value() )
            {
                line += "  implicit(";
                line += tile.implicitTiling->contentUriTemplate.has_value()
                            ? String( tile.implicitTiling->contentUriTemplate->c_str() )
                            : String( "<no content template>" );
                line += ")";
            }

            UtilityFunctions::print( line );

            for ( const std::unique_ptr<core::Tile> &child : tile.children )
            {
                dumpRecursive( *child, maxDepth, indent + 1 );
            }
        }

        /// The Z-up -> Y-up flip that the demo's Georeference3D node applies. Godot's
        /// Transform3D stores its basis row-major, so the demo transform
        /// `Transform3D(1,0,0, 0,0,1, 0,-1,0)` is the matrix M(v) = (vx, vz, -vy), i.e. a
        /// -90 degree rotation about X. Baking this into the implicit model matrix makes a
        /// single, georeference-free tileset render with the exact same orientation as one
        /// placed under an explicit Georeference3D. Columns below are the column-major form
        /// of that matrix.
        math::Mat4 z_up_to_y_up()
        {
            math::Mat4 flip;
            flip[0] = math::Vec4( 1.0, 0.0, 0.0, 0.0 );
            flip[1] = math::Vec4( 0.0, 0.0, -1.0, 0.0 );
            flip[2] = math::Vec4( 0.0, 1.0, 0.0, 0.0 );
            flip[3] = math::Vec4( 0.0, 0.0, 0.0, 1.0 );
            return flip;
        }

        /// True when a content URI names another tileset document rather than renderable
        /// content.
        ///
        /// 3D Tiles has no explicit flag for an external tileset - only the URI. A `.gltf`
        /// is text glTF and therefore content; a `.json` is always a tileset document.
        bool is_external_tileset_uri( const std::string &uri )
        {
            return uri.size() >= 5 && uri.compare( uri.size() - 5, 5, ".json" ) == 0;
        }

        /// True when `node` or any of its ancestors is a Georeference3D.
        bool has_georeference_ancestor( const godot::Node *node )
        {
            for ( const godot::Node *cursor = node; cursor != nullptr;
                  cursor = cursor->get_parent() )
            {
                if ( godot::Object::cast_to<Georeference3D>( cursor ) != nullptr )
                {
                    return true;
                }
            }
            return false;
        }

        /// True when a Globe3D sits anywhere below `subtree`. The globe is planetary-scale,
        /// so in a globe scene the tileset must not claim the editor camera: framing a
        /// metre-sized dataset would park the camera a kilometre above the georeference
        /// origin, deep inside the sky, with the Earth far below the local horizon. The
        /// globe's own framing (planetary distance) is the only pose that shows anything.
        bool has_globe_in_scene( const godot::Node *subtree )
        {
            if ( subtree == nullptr )
            {
                return false;
            }
            if ( godot::Object::cast_to<Globe3D>( subtree ) != nullptr )
            {
                return true;
            }
            for ( int i = 0; i < subtree->get_child_count(); ++i )
            {
                if ( has_globe_in_scene( subtree->get_child( i ) ) )
                {
                    return true;
                }
            }
            return false;
        }

        /// Counts Tileset3D nodes under `subtree` (excluding `except`) that have no
        /// Georeference3D ancestor - the tilesets that would each centre on their own origin
        /// instead of being placed by real latitude/longitude.
        std::size_t count_unguided_tilesets( const godot::Node *subtree, const godot::Node *except )
        {
            std::size_t count = 0;
            if ( subtree == nullptr || subtree == except )
            {
                return 0;
            }
            if ( godot::Object::cast_to<Tileset3D>( subtree ) != nullptr &&
                 !has_georeference_ancestor( subtree ) )
            {
                ++count;
            }
            for ( int i = 0; i < subtree->get_child_count(); ++i )
            {
                count += count_unguided_tilesets( subtree->get_child( i ), except );
            }
            return count;
        }
    } // namespace

    void Tileset3D::_bind_methods()
    {
        ClassDB::bind_method( D_METHOD( "set_url", "p_url" ), &Tileset3D::set_url );
        ClassDB::bind_method( D_METHOD( "get_url" ), &Tileset3D::get_url );
        ClassDB::add_property( "Tileset3D", PropertyInfo( Variant::STRING, "url" ), "set_url",
                               "get_url" );

        ClassDB::bind_method( D_METHOD( "set_maximum_screen_space_error", "p_value" ),
                              &Tileset3D::set_maximum_screen_space_error );
        ClassDB::bind_method( D_METHOD( "get_maximum_screen_space_error" ),
                              &Tileset3D::get_maximum_screen_space_error );
        ClassDB::add_property( "Tileset3D",
                               PropertyInfo( Variant::FLOAT, "maximum_screen_space_error" ),
                               "set_maximum_screen_space_error",
                               "get_maximum_screen_space_error" );

        ClassDB::bind_method( D_METHOD( "set_maximum_simultaneous_loads", "p_value" ),
                              &Tileset3D::set_maximum_simultaneous_loads );
        ClassDB::bind_method( D_METHOD( "get_maximum_simultaneous_loads" ),
                              &Tileset3D::get_maximum_simultaneous_loads );
        ClassDB::add_property( "Tileset3D",
                               PropertyInfo( Variant::INT, "maximum_simultaneous_loads",
                                             godot::PROPERTY_HINT_RANGE, "1,128,1" ),
                               "set_maximum_simultaneous_loads",
                               "get_maximum_simultaneous_loads" );

        ClassDB::bind_method( D_METHOD( "set_debug_show_bounding_volume", "p_value" ),
                              &Tileset3D::set_debug_show_bounding_volume );
        ClassDB::bind_method( D_METHOD( "get_debug_show_bounding_volume" ),
                              &Tileset3D::get_debug_show_bounding_volume );
        ClassDB::add_property( "Tileset3D",
                               PropertyInfo( Variant::BOOL, "debug_show_bounding_volume" ),
                               "set_debug_show_bounding_volume",
                               "get_debug_show_bounding_volume" );

        ClassDB::bind_method( D_METHOD( "set_debug_bounding_volume_scale", "p_value" ),
                              &Tileset3D::set_debug_bounding_volume_scale );
        ClassDB::bind_method( D_METHOD( "get_debug_bounding_volume_scale" ),
                              &Tileset3D::get_debug_bounding_volume_scale );
        ClassDB::add_property( "Tileset3D",
                               PropertyInfo( Variant::FLOAT, "debug_bounding_volume_scale" ),
                               "set_debug_bounding_volume_scale",
                               "get_debug_bounding_volume_scale" );

        ClassDB::bind_method( D_METHOD( "set_show", "p_value" ), &Tileset3D::set_show );
        ClassDB::bind_method( D_METHOD( "get_show" ), &Tileset3D::get_show );
        ClassDB::add_property( "Tileset3D", PropertyInfo( Variant::BOOL, "show" ), "set_show",
                               "get_show" );

        ClassDB::bind_method( D_METHOD( "set_auto_frame_on_load", "p_value" ),
                              &Tileset3D::set_auto_frame_on_load );
        ClassDB::bind_method( D_METHOD( "get_auto_frame_on_load" ),
                              &Tileset3D::get_auto_frame_on_load );
        ClassDB::add_property( "Tileset3D", PropertyInfo( Variant::BOOL, "auto_frame_on_load" ),
                               "set_auto_frame_on_load", "get_auto_frame_on_load" );

        ClassDB::bind_method( D_METHOD( "set_debug_colorize_tiles", "p_value" ),
                              &Tileset3D::set_debug_colorize_tiles );
        ClassDB::bind_method( D_METHOD( "get_debug_colorize_tiles" ),
                              &Tileset3D::get_debug_colorize_tiles );
        ClassDB::add_property( "Tileset3D", PropertyInfo( Variant::BOOL, "debug_colorize_tiles" ),
                               "set_debug_colorize_tiles", "get_debug_colorize_tiles" );

        ClassDB::bind_method( D_METHOD( "load" ), &Tileset3D::load );
        ClassDB::bind_method( D_METHOD( "reload" ), &Tileset3D::reload );
        ClassDB::bind_method( D_METHOD( "unload" ), &Tileset3D::unload );
        ClassDB::bind_method( D_METHOD( "rebase", "p_parent_delta" ), &Tileset3D::rebase );
        ClassDB::bind_method( D_METHOD( "dump_tree", "max_depth" ), &Tileset3D::dump_tree );

        ClassDB::bind_method( D_METHOD( "get_tile_count" ), &Tileset3D::get_tile_count );
        ClassDB::bind_method( D_METHOD( "get_maximum_depth" ), &Tileset3D::get_maximum_depth );
        ClassDB::bind_method( D_METHOD( "get_asset_version" ), &Tileset3D::get_asset_version );
        ClassDB::bind_method( D_METHOD( "get_root_geometric_error" ),
                              &Tileset3D::get_root_geometric_error );
        ClassDB::bind_method( D_METHOD( "get_last_error" ), &Tileset3D::get_last_error );

        ClassDB::bind_method( D_METHOD( "get_dataset_longitude" ),
                              &Tileset3D::get_dataset_longitude );
        ClassDB::bind_method( D_METHOD( "get_dataset_latitude" ), &Tileset3D::get_dataset_latitude );
        ClassDB::bind_method( D_METHOD( "get_dataset_height" ), &Tileset3D::get_dataset_height );
        ClassDB::bind_method( D_METHOD( "get_dataset_radius" ), &Tileset3D::get_dataset_radius );
        ClassDB::bind_method( D_METHOD( "get_anchor_separation" ),
                              &Tileset3D::get_anchor_separation );
        ClassDB::bind_method( D_METHOD( "is_placed_by_georeference" ),
                              &Tileset3D::is_placed_by_georeference );
        ClassDB::bind_method( D_METHOD( "get_loaded_tile_count" ),
                              &Tileset3D::get_loaded_tile_count );
        ClassDB::bind_method( D_METHOD( "get_last_rendered_count" ),
                              &Tileset3D::get_last_rendered_count );
        ClassDB::bind_method( D_METHOD( "get_loaded_bytes" ), &Tileset3D::get_loaded_bytes );
        ClassDB::bind_method( D_METHOD( "get_in_flight_count" ), &Tileset3D::get_in_flight_count );

        ADD_SIGNAL( godot::MethodInfo( "tileset_loaded" ) );
        ADD_SIGNAL( godot::MethodInfo( "load_failed", PropertyInfo( Variant::STRING, "reason" ) ) );
    }

    Tileset3D::Tileset3D() = default;
    Tileset3D::~Tileset3D() = default;

    void Tileset3D::_notification( int p_what )
    {
        switch ( p_what )
        {
            case NOTIFICATION_READY:
            {
                // The traversal runs every frame through NOTIFICATION_PROCESS rather than a
                // _process override: godot-cpp does not declare _process as a virtual on
                // Node, so overriding it would not be called.
                set_process( true );

                // The loader is a child node so its lifetime follows this node's, and so its
                // HTTPRequest children are torn down with the scene.
                if ( loader == nullptr )
                {
                    loader = memnew( TilesetContentLoader );
                    loader->set_name( "ContentLoader" );
                    loader->set_max_concurrent( maximum_simultaneous_loads );
                    add_child( loader );
                }

                // Convenience for editor testing: a node with a url in the scene loads as
                // soon as it is ready.
                if ( !url.strip_edges().is_empty() )
                {
                    load();
                }
                break;
            }

            case NOTIFICATION_PROCESS:
                update_tiles();
                break;

            default:
                break;
        }
    }

    void Tileset3D::set_url( const String &p_url )
    {
        if ( url != p_url )
        {
            url = p_url;

            if ( is_inside_tree() )
            {
                reload();
            }
        }
    }

    String Tileset3D::get_url() const
    {
        return url;
    }

    void Tileset3D::set_maximum_screen_space_error( const double p_value )
    {
        maximum_screen_space_error = p_value;
    }

    double Tileset3D::get_maximum_screen_space_error() const
    {
        return maximum_screen_space_error;
    }

    void Tileset3D::set_debug_show_bounding_volume( const bool p_value )
    {
        if ( debug_show_bounding_volume != p_value )
        {
            debug_show_bounding_volume = p_value;

            if ( p_value )
            {
                // build_debug_mesh() early-outs while the aid is off (see there), so turning
                // it on has to (re)build the wireframe, not just flip a visibility flag.
                build_debug_mesh();
            }
            else if ( debug_mesh != nullptr )
            {
                debug_mesh->set_visible( false );
            }
        }
    }

    bool Tileset3D::get_debug_show_bounding_volume() const
    {
        return debug_show_bounding_volume;
    }

    void Tileset3D::set_debug_bounding_volume_scale( const double p_value )
    {
        if ( debug_bounding_volume_scale != p_value )
        {
            debug_bounding_volume_scale = p_value;
            build_debug_mesh();
        }
    }

    double Tileset3D::get_debug_bounding_volume_scale() const
    {
        return debug_bounding_volume_scale;
    }

    void Tileset3D::set_show( const bool p_value )
    {
        if ( show != p_value )
        {
            show = p_value;

            if ( debug_mesh != nullptr )
            {
                debug_mesh->set_visible( show && debug_show_bounding_volume );
            }
            for ( core::Tile *tile : loaded_tiles )
            {
                if ( auto *node = static_cast<godot::Node3D *>( tile->contentUserData ) )
                {
                    node->set_visible( show &&
                                      tile->contentState == core::ContentState::Ready );
                }
            }
        }
    }

    bool Tileset3D::get_show() const
    {
        return show;
    }

    void Tileset3D::set_auto_frame_on_load( const bool p_value )
    {
        auto_frame_on_load = p_value;
    }

    bool Tileset3D::get_auto_frame_on_load() const
    {
        return auto_frame_on_load;
    }

    void Tileset3D::set_debug_colorize_tiles( const bool p_value )
    {
        if ( debug_colorize_tiles != p_value )
        {
            debug_colorize_tiles = p_value;
            build_debug_mesh();
        }
    }

    bool Tileset3D::get_debug_colorize_tiles() const
    {
        return debug_colorize_tiles;
    }

    String Tileset3D::resolved_path() const
    {
        String path = url.strip_edges();

        // Accept the file:// and file:/// forms as well as a plain path; FileAccess takes
        // absolute OS paths and res:// paths directly. An http(s) URL is passed through
        // untouched - read_document routes it to HTTP.
        if ( path.begins_with( "file:///" ) )
        {
            path = path.substr( 8 );
        }
        else if ( path.begins_with( "file://" ) )
        {
            path = path.substr( 7 );
        }

        return path;
    }

    const Georeference3D *Tileset3D::find_georeference() const
    {
        for ( godot::Node *parent = get_parent(); parent != nullptr;
              parent = parent->get_parent() )
        {
            if ( const Georeference3D *reference = godot::Object::cast_to<Georeference3D>( parent ) )
            {
                return reference;
            }
        }
        return nullptr;
    }

    math::Mat4 Tileset3D::compute_model_matrix() const
    {
        if ( const Georeference3D *reference = find_georeference(); reference != nullptr )
        {
            // R is the georeference's local frame, so modelMatrix is ECEF -> R. The
            // traversal chain starts at the root transform, so the root tile ends up at
            // localToEcef^-1 * rootTransform: the dataset sits at its true position relative
            // to the georeference origin. Keeping that origin near the dataset is what keeps
            // the coordinates small enough for Godot's float32 transforms.
            return reference->ecef_to_local();
        }

        if ( root == nullptr )
        {
            return math::identity();
        }

        // Implicit per-tileset georeference (single tileset, no Georeference3D parent).
        //
        // Whether this dataset may be treated as georeferenced is decided by what the
        // tileset *declares*, never by inspecting the numbers - a bounding volume that
        // happens to sit on the ellipsoid is not the same as a dataset that says where it
        // is. The two declaring forms are:
        //
        //   * the root has a `region` bounding volume. A region is absolute EPSG:4979
        //     geodetic coordinates and, per the spec, is *not* run through the tile
        //     transform chain - so its place in the render frame is `modelMatrix * ECEF`.
        //     It can only be made renderable by supplying a modelMatrix, and ENU is that
        //     matrix.
        //
        //   * the root has a `transform`. The author is stating where the subtree sits
        //     relative to the frame its coordinates are expressed in, which for a
        //     georeferenced dataset is ECEF. `transform[3]` is then a real surface position
        //     and is exactly the anchor to build ENU from.
        //
        // Everything else is authored in its own coordinate space and must use the reference
        // implementation's model, `inverse(rootTransform)` - which for the usual identity
        // case simply keeps the content where the author put it. That includes the tricky
        // case this rule was written for: Aerometrex-SanFrancisco-2cm has a root `sphere`
        // centred on the ellipsoid (so it *looks* georeferenced) but no `transform`, and its
        // b3dm payloads carry full-ECEF `RTC_CENTER`. Running those through an ENU frame
        // while leaving RTC_CENTER in the tile's own space moves the tile tree ~6370 km away
        // from the content, and the dataset renders as an empty frame.
        //
        // The Z-up -> Y-up flip is baked into the ENU branch only. A dataset authored in its
        // own space has no declared up axis to reconcile with, and rotating it would move
        // content away from the origin it was authored around; `asset.gltfUpAxis` still
        // drives per-content correction in ContentFactory, which is where it belongs.
        //
        // The implicit frame is cached at load time (see below): the dataset's root bounding
        // volume is rewritten from region to box during load, so recomputing it afterwards
        // from the converted volume would be wrong.
        if ( model_matrix_.has_value() )
        {
            return *model_matrix_;
        }

        const bool rootHasRegion = root->boundingVolume.has_value() &&
            root->boundingVolume->type == math::BoundingVolume::Type::Region;

        if ( rootHasRegion || root->hasDeclaredTransform )
        {
            const math::Vec3 rootCenter =
                root->boundingVolume.has_value()
                    ? math::boundingVolumeCenter( *root->boundingVolume )
                    : math::Vec3( 0.0 );
            const math::Vec3 ecefCenter = math::transformPoint( root->transform, rootCenter );

            const math::Mat4 enuToEcef = math::eastNorthUpToFixedFrame( ecefCenter );
            const math::Mat4 ecefToEnu = math::invert( enuToEcef );
            const math::Mat4 model = math::multiply( z_up_to_y_up(), ecefToEnu );
            model_matrix_ = model;
            return model;
        }

        // Authored in its own space: mirror the reference's inverse(rootTransform). This is
        // always a well-defined affine transform, so it can never inject NaNs.
        const math::Mat4 model = math::invert( root->transform );
        model_matrix_ = model;
        return model;
    }

    bool Tileset3D::configuration_is_valid() const
    {
        // Rule ④: a Tileset3D must not be nested inside another Tileset3D.
        for ( godot::Node *parent = get_parent(); parent != nullptr;
              parent = parent->get_parent() )
        {
            if ( godot::Object::cast_to<Tileset3D>( parent ) != nullptr )
            {
                return false;
            }
        }

        // Rule ③: several Tileset3D without a shared Georeference3D each centre on their own
        // origin, which is only valid for a single standalone tileset.
        if ( !has_georeference_ancestor( this ) )
        {
            godot::SceneTree *tree = get_tree();
            godot::Node *root = tree != nullptr ? tree->get_edited_scene_root() : nullptr;
            if ( root != nullptr && count_unguided_tilesets( root, this ) >= 1 )
            {
                return false;
            }
        }

        return true;
    }

    godot::PackedStringArray Tileset3D::_get_configuration_warnings() const
    {
        godot::PackedStringArray warnings;

        // Rule ④: nested Tileset3D.
        for ( godot::Node *parent = get_parent(); parent != nullptr;
              parent = parent->get_parent() )
        {
            if ( godot::Object::cast_to<Tileset3D>( parent ) != nullptr )
            {
                warnings.push_back( "Tileset3D nodes must be siblings, not nested. Remove this "
                                    "Tileset3D from inside another Tileset3D." );
                break;
            }
        }

        // Rule ③: more than one Tileset3D without a shared Georeference3D.
        if ( !has_georeference_ancestor( this ) )
        {
            godot::SceneTree *tree = get_tree();
            godot::Node *root = tree != nullptr ? tree->get_edited_scene_root() : nullptr;
            if ( root != nullptr && count_unguided_tilesets( root, this ) >= 1 )
            {
                warnings.push_back(
                    "Multiple Tileset3D nodes without a shared Georeference3D: each will centre "
                    "on its own ECEF centre instead of being placed by real latitude/longitude. "
                    "Add one Georeference3D node and parent all tilesets to it for multi-scene "
                    "layouts." );
            }
        }

        return warnings;
    }

    void Tileset3D::frame_camera()
    {
#ifdef TILES3D_EDITOR_TARGET
        godot::Engine *engine = godot::Engine::get_singleton();
        if ( engine == nullptr || !engine->is_editor_hint() )
        {
            return;
        }
        if ( dataset_radius <= 0.0 )
        {
            return;
        }

        // A Globe3D in the same scene owns the editor camera; see has_globe_in_scene().
        godot::Node *scene_root = this;
        while ( scene_root->get_parent() != nullptr )
        {
            scene_root = scene_root->get_parent();
        }
        if ( has_globe_in_scene( scene_root ) )
        {
            return;
        }

        godot::EditorInterface *editor = godot::EditorInterface::get_singleton();
        if ( editor == nullptr )
        {
            return;
        }
        godot::SubViewport *viewport = editor->get_editor_viewport_3d();
        if ( viewport == nullptr )
        {
            return;
        }
        godot::Camera3D *camera = viewport->get_camera_3d();
        if ( camera == nullptr )
        {
            return;
        }

        const godot::Vector3 target = get_global_transform().origin;
        const double fov = static_cast<double>( camera->get_fov() );
        constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
        const double distance = ( dataset_radius / std::tan( ( fov * kDegToRad ) / 2.0 ) ) * 1.2;

        const godot::Vector3 eye =
            target + godot::Vector3( 0.0f, static_cast<float>( distance * 0.4f ),
                                    static_cast<float>( distance ) );
        const godot::Vector3 forward = ( target - eye ).normalized();
        const godot::Vector3 worldUp( 0.0f, 1.0f, 0.0f );
        const godot::Vector3 right = worldUp.cross( forward ).normalized();
        const godot::Vector3 up = forward.cross( right ).normalized();
        // Godot cameras look down their local -Z.
        const godot::Basis basis( right, up, -forward );
        camera->set_global_transform( godot::Transform3D( basis, eye ) );
#else
        (void)0;
#endif
    }

    void Tileset3D::clear_loaded()
    {
        // Cancel everything in flight first. The worker threads hold raw `core::Tile*` into
        // the tree that is about to be destroyed, so they have to be told to stop and reaped
        // before `root.reset()` runs. bump_generation() also makes any result that races in
        // afterwards recognisable as stale, so it can never be adopted.
        if ( loader != nullptr )
        {
            loader->bump_generation();
            loader->cancel_before_generation( loader->get_generation() );
            loader_generation = loader->get_generation();
        }

        // Content nodes go first: loaded_tiles holds pointers into the tile tree, so it has
        // to be drained before root is destroyed.
        for ( core::Tile *tile : loaded_tiles )
        {
            release_content( *tile );
        }
        loaded_tiles.clear();

        render_list.clear();
        load_queue.clear();

        if ( debug_mesh != nullptr )
        {
            // Detach before freeing so nothing is left pointing at it.
            if ( debug_mesh->get_parent() == this )
            {
                remove_child( debug_mesh );
            }
            debug_mesh->queue_free();
            debug_mesh = nullptr;
        }

        root.reset();
        asset_version = String();
        root_geometric_error = 0.0;
        tile_count = 0;
        maximum_depth = 0;
        last_error = String();
        base_directory = String();
        placed_by_georeference = false;
        frame_number = 0;
        last_rendered_count = 0;
        dataset_radius = 0.0;
        loaded_bytes = 0;
        needs_framing = false;
        model_matrix_.reset();
    }

    void Tileset3D::release_content( core::Tile &tile )
    {
        if ( tile.contentUserData == nullptr )
        {
            return;
        }

        auto *node = static_cast<godot::Node3D *>( tile.contentUserData );
        if ( node->get_parent() == this )
        {
            remove_child( node );
        }
        node->queue_free();

        tile.contentUserData = nullptr;
        tile.contentState = core::ContentState::Unloaded;
    }

    void Tileset3D::load()
    {
        if ( root != nullptr )
        {
            return;
        }

        if ( !configuration_is_valid() )
        {
            // The node-tree rules are violated (see _get_configuration_warnings). Refuse to
            // load so the misconfiguration cannot be missed at runtime; the editor already
            // surfaces it as a configuration warning.
            last_error = "invalid node configuration (see editor warnings)";
            UtilityFunctions::printerr( "[Tileset3D] ", last_error, ": '", url, "'" );
            emit_signal( "load_failed", last_error );
            return;
        }

        clear_loaded();

        const String path = resolved_path();
        if ( path.is_empty() )
        {
            last_error = "url is empty";
            UtilityFunctions::printerr( "[Tileset3D] ", last_error );
            emit_signal( "load_failed", last_error );
            return;
        }

        // Fetch the document. Only the small control files go through this path; payloads are
        // streamed by the loader. An http(s) URL is handled by a blocking request here because
        // the tile tree cannot exist until the root document is parsed - there is nothing to
        // show and nothing to schedule until then.
        String text;
        String readError;
        if ( !read_document( path, text, readError ) )
        {
            last_error = readError;
            UtilityFunctions::printerr( "[Tileset3D] ", last_error );
            emit_signal( "load_failed", last_error );
            return;
        }

        // allow_exceptions = false: the kernel layer is exception free, and a discarded
        // document is easier to report than a thrown parse error.
        nlohmann::json document = nlohmann::json::parse( text.utf8().get_data(), nullptr, false );
        if ( document.is_discarded() )
        {
            last_error = godot::vformat( "'%s' is not valid JSON", path );
            UtilityFunctions::printerr( "[Tileset3D] ", last_error );
            emit_signal( "load_failed", last_error );
            return;
        }

        core::TilesetParseResult parsed = core::parseTilesetJson( document );
        if ( !parsed )
        {
            last_error = String( parsed.error.c_str() );
            UtilityFunctions::printerr( "[Tileset3D] ", last_error );
            emit_signal( "load_failed", last_error );
            return;
        }

        root = std::move( parsed.root );
        asset_version = String( parsed.assetVersion.c_str() );
        root_geometric_error = parsed.geometricError;
        model_up_axis_ = parsed.modelUpAxis;
        next_tile_id_ = parsed.nextId;

        // Content URIs in a tileset are relative to the tileset document. Kept as the document
        // URL itself (not a directory) so core::resolveUrl can handle both an http base and a
        // Windows path without the caller guessing which separator applies.
        base_directory = path;

        // Splice in every nested tileset before anything else looks at the tree: the
        // traversal, the region conversion, the bounding volume debug mesh and the tile
        // count all have to see the real tree rather than a stump of unresolved
        // `tileset.json` references.
        expand_external_tilesets();

        placed_by_georeference = find_georeference() != nullptr;
        const math::Mat4 model = compute_model_matrix();

        // Regions are EPSG:4979 absolute coordinates and do not follow the transform chain,
        // so they are rewritten into the tile local frame once, here.
        core::convertRegionBoundingVolumes( *root, model, model );

        // Used by frame_camera() to fit the dataset in view after load.
        dataset_radius = root->boundingVolume.has_value()
                            ? math::boundingVolumeRadius( *root->boundingVolume )
                            : 0.0;

        // Flag the editor to frame the dataset once it has loaded (editor only).
        needs_framing = auto_frame_on_load && godot::Engine::get_singleton() != nullptr &&
                        godot::Engine::get_singleton()->is_editor_hint();

        count_tiles();
        build_debug_mesh();

        const char *upAxisName = "Y";
        if ( model_up_axis_ == core::ModelUpAxis::X )
        {
            upAxisName = "X";
        }
        else if ( model_up_axis_ == core::ModelUpAxis::Z )
        {
            upAxisName = "Z";
        }

        UtilityFunctions::print( godot::vformat(
            "[Tileset3D] loaded '%s': version=%s tiles=%d maxDepth=%d rootGE=%.3f upAxis=%s "
            "georeferenced=%s",
            path, asset_version, static_cast<int>( tile_count ), maximum_depth, root_geometric_error,
            upAxisName, placed_by_georeference ? "yes" : "no (origin-centred fallback)" ) );

        report_georeference();

        // Implicit tiling has no children until a subtree is decoded, so the tile count above
        // only covers the parsed stump. Print the declaration so it is obvious which mode the
        // tileset is in and whether the level bound was understood.
        if ( root->implicitTiling.has_value() )
        {
            const core::ImplicitTiling &implicit = *root->implicitTiling;
            UtilityFunctions::print( godot::vformat(
                "[Tileset3D] implicit tiling: scheme=%s subtreeLevels=%d availableLevels=%d "
                "levelCap=%d subtrees='%s' content='%s'",
                implicit.subdivisionScheme == core::ImplicitTiling::SubdivisionScheme::Octree
                    ? "octree"
                    : "quadtree",
                implicit.subtreeLevels, implicit.availableLevels, implicit.levelCap(),
                String( implicit.subtreeUriTemplate.c_str() ),
                implicit.contentUriTemplate.has_value()
                    ? String( implicit.contentUriTemplate->c_str() )
                    : String( "(none)" ) ) );
        }

        emit_signal( "tileset_loaded" );
    }

    void Tileset3D::report_georeference()
    {
        if ( root == nullptr || !root->boundingVolume.has_value() )
        {
            return;
        }

        // Where does this dataset actually say it is? The root tile's own transform maps
        // tile-local coordinates to *ECEF* - the top-level parent frame of a 3D Tiles tree is
        // the global one, well before any georeference is involved. worldMatrix is not usable
        // here: the traversal fills it in, and load() finishes long before that. Composing
        // the answer from ECEF also means it is the one number that can disagree with the
        // georeference, which is exactly the point of the check.
        const math::Vec3 local_center = math::boundingVolumeCenter( *root->boundingVolume );
        const math::Vec3 world_center = math::transformPoint( root->transform, local_center );
        const math::Vec3 geodetic = math::cartesianToWgs84( world_center );
        const double longitude = geodetic.x * 180.0 / math::kPi;
        const double latitude = geodetic.y * 180.0 / math::kPi;

        dataset_longitude_ = longitude;
        dataset_latitude_ = latitude;
        dataset_height_ = geodetic.z;
        anchor_separation_ = -1.0;

        UtilityFunctions::print( godot::vformat(
            "[Tileset3D] dataset centre: lon=%.5f lat=%.5f h=%.1f m (from its own transform)",
            longitude, latitude, geodetic.z ) );

        const Georeference3D *reference = find_georeference();
        if ( reference == nullptr )
        {
            return;
        }

        const math::Vec3 anchor = reference->origin_ecef();
        const double separation = glm::length( world_center - anchor );
        anchor_separation_ = separation;
        const double radius = root->boundingVolume.has_value()
                                  ? math::boundingVolumeRadius( *root->boundingVolume )
                                  : 0.0;

        UtilityFunctions::print( godot::vformat(
            "[Tileset3D] georeference anchor is %.1f km from the dataset centre (radius %.0f m)",
            separation / 1000.0, radius ) );

        // A dataset that is georeferenced somewhere else still loads, still reports every
        // tile as loaded, and still renders nothing - because it is simply off-screen. The
        // failure looks exactly like a rendering bug, so say it out loud instead.
        const double tolerance = std::max( 3.0 * radius, 10000.0 );
        if ( separation > tolerance )
        {
            UtilityFunctions::printerr( godot::vformat(
                "[Tileset3D] WARNING: '%s' is %.1f km from the Georeference3D anchor "
                "(lon=%.5f lat=%.5f). Nothing will be visible at the anchor. Either the "
                "dataset's tileset.json declares the wrong transform, or the anchor belongs "
                "somewhere else - the distance is far beyond the dataset's own %.0f m radius.",
                url, separation / 1000.0, longitude, latitude, radius ) );
        }
    }

    void Tileset3D::reload()
    {
        clear_loaded();
        load();
    }

    void Tileset3D::unload()
    {
        clear_loaded();
    }

    void Tileset3D::rebase( const Vector3 &p_parent_delta )
    {
        // The traversal's model matrix produces coordinates in the georeference frame and the
        // result is written straight onto content nodes parented to *this* node, so a cached
        // worldMatrix lives in this node's own space. Taking this node's transform back out is
        // what keeps the call correct if it is ever moved in the scene.
        const Vector3 local_delta = get_transform().basis.xform_inv( p_parent_delta );
        if ( local_delta.length_squared() <= 0.0f )
        {
            return;
        }

        const math::Vec3 delta( static_cast<double>( local_delta.x ),
                                static_cast<double>( local_delta.y ),
                                static_cast<double>( local_delta.z ) );

        for ( core::Tile *tile : loaded_tiles )
        {
            if ( tile == nullptr )
            {
                continue;
            }
            auto *node = static_cast<godot::Node3D *>( tile->contentUserData );
            if ( node == nullptr || !tile->worldMatrix.has_value() )
            {
                continue;
            }

            // Left-multiplying by a translation is exactly adding to the translation column.
            // Doing it to the cached matrix (rather than only to the node) matters because
            // sync_content_visibility() re-applies that matrix for every rendered tile, and
            // would otherwise silently undo the shift on the same frame.
            math::Vec4 &translation = ( *tile->worldMatrix )[3];
            translation.x += delta.x;
            translation.y += delta.y;
            translation.z += delta.z;

            node->set_transform( toGodotTransform( *tile->worldMatrix ) );
        }

        // Nothing else here needs invalidating. Tile bounding spheres are re-derived from the
        // bounding volume and the fresh model matrix by the traversal itself, and the debug
        // wireframe is a one-shot diagnostic snapshot whose rebuild is not worth paying on
        // every origin shift - it comes back correct on the next full rebuild.
    }

    void Tileset3D::expand_external_tilesets()
    {
        if ( root == nullptr )
        {
            return;
        }

        expand_external_tileset( *root, 0 );
    }

    void Tileset3D::expand_external_tileset( core::Tile &tile, int depth )
    {
        // A real dataset nests a handful of levels; anything past this is a cyclic or
        // runaway chain of references.
        constexpr int kMaxDepth = 32;
        if ( depth > kMaxDepth )
        {
            return;
        }

        if ( tile.content.has_value() && is_external_tileset_uri( tile.content->uri ) )
        {
            const String nestedPath = content_path( tile );

            bool merged = false;

            String text;
            String readError;
            if ( !read_document( nestedPath, text, readError ) )
            {
                UtilityFunctions::printerr( "[Tileset3D] cannot open external tileset '",
                                            nestedPath, "': ", readError );
            }
            else
            {
                nlohmann::json document =
                    nlohmann::json::parse( text.utf8().get_data(), nullptr, false );

                if ( document.is_discarded() )
                {
                    UtilityFunctions::printerr( "[Tileset3D] external tileset '", nestedPath,
                                                "' is not valid JSON" );
                }
                else
                {
                    core::TilesetParseResult external =
                        core::parseTilesetJson( document, tile.refine );

                    if ( !external )
                    {
                        UtilityFunctions::printerr( "[Tileset3D] external tileset '", nestedPath,
                                                    "': ", String( external.error.c_str() ) );
                    }
                    else
                    {
                        core::Tile &externalRoot = *external.root;

                        // URIs declared inside the external document are relative to *its*
                        // directory, not to the root tileset's, so the incoming subtree is
                        // rebased before it joins the tree.
                        rebase_content_uris( externalRoot, nestedPath );

                        // The container keeps its own bounding volume and geometric error:
                        // those describe the region in the *referencing* tileset's frame and
                        // are what the traversal culls and refines against. Everything that
                        // makes the tile renderable - transform, content, children - comes
                        // from the external document's root.
                        //
                        // isExternalTileset is deliberately NOT set: it forces unconditional
                        // refinement, which would descend to every leaf regardless of screen
                        // space error. A container that ended up with content must be allowed
                        // to render it until the error says otherwise.
                        tile.transform = math::multiply( tile.transform, externalRoot.transform );
                        tile.content = std::move( externalRoot.content );

                        for ( std::unique_ptr<core::Tile> &child : externalRoot.children )
                        {
                            tile.children.push_back( std::move( child ) );
                        }

                        merged = true;
                    }
                }
            }

            if ( !merged )
            {
                // Drop the unusable content so the loader stops fetching a tileset document
                // as if it were a mesh. The tile degenerates into a plain container.
                tile.content.reset();
            }
        }

        for ( const std::unique_ptr<core::Tile> &child : tile.children )
        {
            expand_external_tileset( *child, depth + 1 );
        }
    }

    void Tileset3D::rebase_content_uris( core::Tile &tile, const String &documentUrl )
    {
        if ( tile.content.has_value() )
        {
            const String uri( tile.content->uri.c_str() );
            const std::string resolved =
                core::resolveUrl( documentUrl.utf8().get_data(), uri.utf8().get_data() );
            tile.content->uri = resolved;
        }

        for ( const std::unique_ptr<core::Tile> &child : tile.children )
        {
            rebase_content_uris( *child, documentUrl );
        }
    }

    void Tileset3D::ensure_implicit_children( core::Tile &tile )
    {
        if ( !tile.implicitTiling.has_value() || tile.implicitChildrenMaterialized )
        {
            return;
        }

        // One attempt per tile. A subtree that cannot be opened will not start succeeding, and
        // retrying every frame would flood the output with the same message.
        tile.implicitChildrenMaterialized = true;

        if ( !tile.implicitCoordinates.has_value() )
        {
            return;
        }

        const core::ImplicitTiling &decl = *tile.implicitTiling;
        const int subtreeLevels = decl.subtreeLevels;
        if ( subtreeLevels <= 0 || decl.subtreeUriTemplate.empty() )
        {
            return;
        }

        const int level = tile.implicitCoordinates->level;
        const int cap = decl.levelCap();
        if ( cap >= 0 && level + 1 > cap )
        {
            return;
        }

        const bool isOctree =
            decl.subdivisionScheme == core::ImplicitTiling::SubdivisionScheme::Octree;
        const int branchingFactor = decl.branchingFactor();

        // Morton coordinates of the subtree that owns this tile: the level is floored to a
        // multiple of subtreeLevels and the in-plane coordinates to the same granularity
        // (bf^levelDiff), which is how the reference addresses a subtree from one of its tiles.
        const int rootLevel = ( level / subtreeLevels ) * subtreeLevels;
        std::int64_t scale = 1;
        for ( int i = level - rootLevel; i > 0; --i )
        {
            scale *= branchingFactor;
        }

        const core::ImplicitCoordinates &coords = *tile.implicitCoordinates;
        const int subtreeX = static_cast<int>( ( coords.x / scale ) * scale );
        const int subtreeY = static_cast<int>( ( coords.y / scale ) * scale );
        const int subtreeZ = isOctree ? static_cast<int>( ( coords.z / scale ) * scale ) : 0;

        const String subtreeUri =
            String( core::replaceTemplate( decl.subtreeUriTemplate, rootLevel, subtreeX, subtreeY,
                                           subtreeZ )
                        .c_str() );
        const String subtreePath = document_url( subtreeUri );

        // A subtree is binary and has to be decoded before any child tiles exist, so it goes
        // through the blocking path rather than the concurrent loader. Sizes are in the tens
        // of KB, and it is only fetched when a tile first wants to refine.
        godot::PackedByteArray bytes;
        String readError;
        if ( !read_binary_document( subtreePath, bytes, readError ) )
        {
            UtilityFunctions::printerr( "[Tileset3D] cannot open subtree '", subtreePath,
                                        "': ", readError );
            return;
        }

        core::SubtreeData subtree;
        std::string error;
        if ( !core::parseSubtree( reinterpret_cast<const std::uint8_t *>( bytes.ptr() ),
                                  static_cast<std::size_t>( bytes.size() ), subtree, error ) )
        {
            UtilityFunctions::printerr( "[Tileset3D] subtree '", subtreePath,
                                        "': ", String( error.c_str() ) );
            return;
        }

        core::expandImplicitSubtree( tile, subtree, decl, next_tile_id_ );
    }

    void Tileset3D::count_tiles()
    {
        tile_count = 0;
        maximum_depth = 0;

        if ( root != nullptr )
        {
            countTilesRecursive( *root, tile_count, maximum_depth );
        }
    }

    void Tileset3D::build_debug_mesh()
    {
        if ( debug_mesh != nullptr )
        {
            if ( debug_mesh->get_parent() == this )
            {
                remove_child( debug_mesh );
            }
            debug_mesh->queue_free();
            debug_mesh = nullptr;
        }

        if ( root == nullptr )
        {
            return;
        }

        // The wireframe is a debugging aid and this walk touches every tile of the tree -
        // 2600+ tiles on a mid-size dataset, every single load, including the synchronous
        // reload a dataset switch runs. Building it while debug_show_bounding_volume is off
        // only produced an invisible mesh that ate the whole cost, so it is authored on
        // demand: set_debug_show_bounding_volume(true) builds it lazily, and this call
        // becomes a no-op while the aid is off. Side benefit: the load-time mesh used to be
        // built against the anchor that was current *then*; a lazily built one is always
        // asked for after the anchor has settled.
        if ( !debug_show_bounding_volume )
        {
            return;
        }

        PackedVector3Array vertices;
        PackedColorArray colors;
        collectTileEdges( *root, compute_model_matrix(), debug_bounding_volume_scale,
                          debug_colorize_tiles, vertices, colors );

        if ( vertices.is_empty() )
        {
            return;
        }

        Array arrays;
        arrays.resize( Mesh::ARRAY_MAX );
        arrays[Mesh::ARRAY_VERTEX] = vertices;
        arrays[Mesh::ARRAY_COLOR] = colors;

        Ref<ArrayMesh> mesh;
        mesh.instantiate();
        mesh->add_surface_from_arrays( Mesh::PRIMITIVE_LINES, arrays );

        Ref<StandardMaterial3D> material;
        material.instantiate();
        material->set_shading_mode( BaseMaterial3D::SHADING_MODE_UNSHADED );
        // Depth colouring rides on the vertex colours. The albedo default is white, which is
        // what we want underneath them.
        material->set_flag( BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true );

        debug_mesh = memnew( MeshInstance3D );
        debug_mesh->set_name( "BoundingVolumeWireframe" );
        debug_mesh->set_mesh( mesh );
        debug_mesh->set_material_override( material );
        debug_mesh->set_visible( debug_show_bounding_volume );
        add_child( debug_mesh );
    }

    std::size_t Tileset3D::get_tile_count() const
    {
        return tile_count;
    }

    int Tileset3D::get_maximum_depth() const
    {
        return maximum_depth;
    }

    String Tileset3D::get_asset_version() const
    {
        return asset_version;
    }

    double Tileset3D::get_root_geometric_error() const
    {
        return root_geometric_error;
    }

    double Tileset3D::get_dataset_longitude() const
    {
        return dataset_longitude_;
    }

    double Tileset3D::get_dataset_latitude() const
    {
        return dataset_latitude_;
    }

    double Tileset3D::get_dataset_height() const
    {
        return dataset_height_;
    }

    double Tileset3D::get_dataset_radius() const
    {
        return dataset_radius;
    }

    double Tileset3D::get_anchor_separation() const
    {
        return anchor_separation_;
    }

    String Tileset3D::get_last_error() const
    {
        return last_error;
    }

    bool Tileset3D::is_placed_by_georeference() const
    {
        return placed_by_georeference;
    }

    void Tileset3D::dump_tree( const int max_depth ) const
    {
        if ( root == nullptr )
        {
            UtilityFunctions::print( "[Tileset3D] dump_tree: nothing loaded" );
            return;
        }

        UtilityFunctions::print( godot::vformat( "[Tileset3D] tree of '%s' (version %s, %d tiles)",
                                                 url, asset_version,
                                                 static_cast<int>( tile_count ) ) );
        dumpRecursive( *root, max_depth, 0 );
    }

    // ---------------------------------------------------------------------------
    // Property accessors added with the traversal
    // ---------------------------------------------------------------------------

    void Tileset3D::set_maximum_simultaneous_loads( const int p_value )
    {
        maximum_simultaneous_loads = std::max( 1, p_value );

        if ( loader != nullptr )
        {
            loader->set_max_concurrent( maximum_simultaneous_loads );
        }
    }

    int Tileset3D::get_maximum_simultaneous_loads() const
    {
        return maximum_simultaneous_loads;
    }

    std::size_t Tileset3D::get_loaded_tile_count() const
    {
        return loaded_tiles.size();
    }

    std::size_t Tileset3D::get_last_rendered_count() const
    {
        return last_rendered_count;
    }

    std::size_t Tileset3D::get_loaded_bytes() const
    {
        return loaded_bytes;
    }

    int Tileset3D::get_in_flight_count() const
    {
        return loader != nullptr ? static_cast<int>( loader->get_active_count() ) : 0;
    }

    // ---------------------------------------------------------------------------
    // Content IO
    // ---------------------------------------------------------------------------

    String Tileset3D::document_url( const String &reference ) const
    {
        // An already-absolute reference (full http URL, or a rooted local path) must not be
        // joined again. core::resolveUrl would treat a Windows path as scheme-less and
        // re-prefix it, so that case is filtered here.
        const std::string referenceUtf8 = reference.utf8().get_data();

        if ( core::hasUrlScheme( referenceUtf8 ) || reference.is_absolute_path() )
        {
            return reference;
        }

        // `base_directory` holds the root document's full URL (not a directory) so the same
        // arithmetic serves a Windows path and an http URL. Doing it in the kernel keeps this
        // resolution identical to the one the loader uses to decide local-vs-remote.
        const std::string base = base_directory.utf8().get_data();
        return String( core::resolveUrl( base, referenceUtf8 ).c_str() );
    }

    String Tileset3D::content_path( const core::Tile &tile ) const
    {
        if ( !tile.content.has_value() )
        {
            return String();
        }

        return document_url( String( tile.content->uri.c_str() ) );
    }

    bool Tileset3D::read_document( const String &path, String &out_text,
                                   String &out_error ) const
    {
        godot::PackedByteArray bytes;

        if ( !read_binary_document( path, bytes, out_error ) )
        {
            return false;
        }

        out_text = String::utf8( reinterpret_cast<const char *>( bytes.ptr() ),
                                 static_cast<int>( bytes.size() ) );
        return true;
    }

    bool Tileset3D::read_binary_document( const String &path, godot::PackedByteArray &out_bytes,
                                          String &out_error ) const
    {
        const std::string pathUtf8 = path.utf8().get_data();

        if ( core::isRemoteUrl( pathUtf8 ) )
        {
            // Synchronous by design: this is only used for the small control documents
            // (tileset.json, .subtree) that have to be parsed before anything can be
            // scheduled. Payloads go through the concurrent loader instead. A blocking
            // HTTPClient is the right tool - there is nothing to render while waiting.
            //
            // connect_to_host wants a host and a port, not a URL, so the URL is split first.
            // Doing the split in the kernel keeps it unit testable and keeps the "does this
            // URL have an authority" rule next to the rest of the URL logic.
            const core::UrlParts parts = core::splitUrl( pathUtf8 );
            if ( !parts.valid )
            {
                out_error = godot::vformat( "not a usable url: '%s'", path );
                return false;
            }

            // Ref<T>, not a stack object: HTTPClient derives from RefCounted, and a
            // stack-constructed Godot object dies with "Godot Object created without binding
            // callbacks" because the engine's construction hook never runs for it.
            //
            // The client is kept across calls (http_reuse_): one load reads the root
            // document plus every external tileset through here, and re-connecting per
            // document measured ~9 ms each on a local server - ~270 ms of pure handshake on
            // the weinan switch, versus ~2 ms per document over the kept-alive connection.
            // A connection the server has closed (or a different host/port) is detected by
            // the status check and rebuilt; HTTP/1.1 keep-alive makes the reuse the common
            // case.
            const godot::String host( parts.host.c_str() );
            godot::Ref<godot::HTTPClient> client;
            const bool reusable =
                http_reuse_.is_valid() && http_reuse_host_ == host &&
                http_reuse_port_ == parts.port &&
                http_reuse_->get_status() == godot::HTTPClient::STATUS_CONNECTED;
            if ( reusable )
            {
                client = http_reuse_;
            }
            else
            {
                client.instantiate();

                const godot::Error connectError =
                    client->connect_to_host( host, parts.port );
                if ( connectError != godot::OK )
                {
                    out_error = godot::vformat( "cannot connect to '%s' (error %d)", path,
                                                static_cast<int>( connectError ) );
                    return false;
                }

                while ( client->get_status() == godot::HTTPClient::STATUS_RESOLVING ||
                        client->get_status() == godot::HTTPClient::STATUS_CONNECTING )
                {
                    client->poll();
                }

                if ( client->get_status() != godot::HTTPClient::STATUS_CONNECTED )
                {
                    out_error = godot::vformat( "cannot connect to '%s' (status %d)", path,
                                                static_cast<int>( client->get_status() ) );
                    return false;
                }

                http_reuse_ = client;
                http_reuse_host_ = host;
                http_reuse_port_ = parts.port;
            }

            // A request target is a legal URI form, and HTTPClient writes it straight into the
            // request line. The kernel does not re-encode URLs when it joins them (a written
            // escape has to survive), so the encoding has to happen exactly here, once, on the
            // way out: the datasets here live under "3D Tiles/", and an unencoded space makes
            // the server see a request for "/3D" and answer 404.
            const std::string requestPath = core::encodeUrlPath( parts.path );

            // The path carries the query as well as the separator, which is what the request
            // target wants - both go in the same field.
            godot::String requestPathGodot = String( requestPath.c_str() );
            if ( !parts.scheme.empty() && parts.scheme != "http" && parts.scheme != "https" )
            {
                out_error =
                    godot::vformat( "unsupported scheme '%s'", String( parts.scheme.c_str() ) );
                return false;
            }

            const godot::Error requestError =
                client->request( godot::HTTPClient::METHOD_GET, requestPathGodot,
                                 godot::PackedStringArray() );
            if ( requestError != godot::OK )
            {
                out_error = godot::vformat( "cannot request '%s' (error %d)", path,
                                            static_cast<int>( requestError ) );
                return false;
            }

            // Accumulate the body across chunks; a tileset.json is small but the client hands
            // it back in pieces regardless.
            godot::PackedByteArray body;
            bool gotBody = false;

            while ( client->get_status() == godot::HTTPClient::STATUS_REQUESTING ||
                    client->get_status() == godot::HTTPClient::STATUS_BODY )
            {
                client->poll();

                if ( !client->has_response() )
                {
                    continue;
                }

                const int responseCode = client->get_response_code();
                if ( responseCode < 200 || responseCode >= 300 )
                {
                    out_error = godot::vformat( "HTTP %d for '%s'", responseCode, path );
                    return false;
                }

                godot::PackedByteArray chunk = client->read_response_body_chunk();
                if ( chunk.size() > 0 )
                {
                    body.append_array( chunk );
                    gotBody = true;
                }
            }

            if ( !gotBody || body.size() == 0 )
            {
                // Usually the server closed mid-response; the socket is not reusable.
                http_reuse_ = godot::Ref<godot::HTTPClient>();
                http_reuse_host_ = godot::String();
                http_reuse_port_ = -1;
                out_error = godot::vformat( "empty response body for '%s'", path );
                return false;
            }

            // A server that closes the connection after the response (Connection: close or
            // HTTP/1.0) leaves the client disconnected; drop it so the next call rebuilds
            // instead of trying to reuse a dead socket.
            if ( client->get_status() != godot::HTTPClient::STATUS_CONNECTED )
            {
                http_reuse_ = godot::Ref<godot::HTTPClient>();
                http_reuse_host_ = godot::String();
                http_reuse_port_ = -1;
            }

            out_bytes = body;
            return true;
        }

        const Ref<FileAccess> file = FileAccess::open( path, FileAccess::READ );
        if ( file.is_null() )
        {
            out_error = godot::vformat( "cannot open '%s' (error %d)", path,
                                        static_cast<int>( FileAccess::get_open_error() ) );
            return false;
        }

        out_bytes = file->get_buffer( file->get_length() );
        file->close();
        return true;
    }

    // ---------------------------------------------------------------------------
    // Traversal
    // ---------------------------------------------------------------------------

    Tileset3D::ViewState Tileset3D::current_view() const
    {
        ViewState view;

        godot::Viewport *viewport = get_viewport();

        if ( viewport == nullptr )
        {
            return view;
        }

        view.viewportHeight = static_cast<double>( viewport->get_visible_rect().size.y );

#ifdef TILES3D_EDITOR_TARGET
        // Inside the editor the camera the user actually flies is the editor viewport's own
        // camera. A Camera3D placed in the scene is a *static* node that does not move with
        // it and is picked up when the project is run - and get_viewport()->get_camera_3d()
        // does return it here, so a "use the editor camera only when null" fallback would
        // never reach it. The traversal would then keep reading a camera that never moves
        // and LOD would look completely dead. Prefer the editor camera in the editor.
        //
        // Height: SubViewport::get_size() is the real pixel size. Viewport::get_visible_rect()
        // returns a rect in the parent's coordinate space and measured 2.0 for the editor
        // viewport, which scales the screen space error down by ~400x and stops all
        // refinement dead (rootSSE 2.19 against a threshold of 16).
        godot::Engine *engine = godot::Engine::get_singleton();
        if ( engine != nullptr && engine->is_editor_hint() )
        {
            godot::SubViewport *editorViewport =
                godot::EditorInterface::get_singleton()->get_editor_viewport_3d();
            if ( editorViewport != nullptr && editorViewport->get_camera_3d() != nullptr )
            {
                viewport = editorViewport;
                view.viewportHeight = static_cast<double>( editorViewport->get_size().y );
            }
        }
#endif

        godot::Camera3D *camera = viewport->get_camera_3d();
        if ( camera == nullptr )
        {
            return view;
        }

        // The traversal works in this node's local space, so the camera position is brought
        // into it rather than the tiles into world space.
        view.position = fromGodotVector( to_local( camera->get_global_transform().origin ) );
        view.fovDegrees = static_cast<double>( camera->get_fov() );

        // Forward vector, also brought into local space. Only the direction matters, so the
        // translation part of the transform is irrelevant here.
        //
        // `to_local` takes a point in this node's *parent* space, so a world-space point has
        // to be pushed through to_global-style arithmetic: add the offset to the already
        // local-space camera position and map the result back, then subtract. Doing it with
        // Vector3 rather than the kernel Vec3 keeps the two to_local calls in the same space.
        const godot::Vector3 forwardGlobal = -camera->get_global_transform().basis.get_column( 2 );
        const godot::Vector3 cameraLocal = to_local( camera->get_global_transform().origin );
        const godot::Vector3 forwardLocal =
            to_local( camera->get_global_transform().origin + forwardGlobal ) - cameraLocal;

        if ( forwardLocal.length_squared() > 0.0f )
        {
            view.forward = fromGodotVector( forwardLocal.normalized() );
        }

        return view;
    }

    void Tileset3D::update_tiles()
    {
        if ( root == nullptr )
        {
            return;
        }

        // Editor convenience: fly the camera to frame the dataset once after it loads.
        if ( needs_framing )
        {
            frame_camera();
            needs_framing = false;
        }

        ++frame_number;

        const ViewState view = current_view();
        if ( view.viewportHeight <= 0.0 )
        {
            // No camera to rank against, so nothing may be requested, and whatever is already
            // in flight is for a view that no longer exists.
            cancel_stale_loads();
            load_queue.clear();
            return;
        }

        render_list.clear();
        load_queue.clear();

        // The traversal starts from the render frame: modelMatrix is the parent of the root's
        // own transform. "No conditional ancestor yet" is +infinity, which makes the root
        // decide purely on its screen space error - any finite seed would either force the
        // root to refine unconditionally or stop it refining at all.
        traverse_tile( *root, compute_model_matrix(), view,
                       std::numeric_limits<double>::infinity() );

        // The traversal has refreshed every priority this frame, so this is the point where
        // the ranking is meaningful. Loading itself happens on worker threads and over HTTP;
        // the two calls below only hand work over and take results back.
        dispatch_loads();
        adopt_completed_loads();
        sync_content_visibility();

        last_rendered_count = render_list.size();

        // LOD diagnostic. Fires when the selected set changes, plus at most once every
        // kReportInterval frames so a parked camera does not flood the output.
        //
        // The earlier variant printed only the first 5 frames, which is useless: at that
        // point nothing has loaded yet, so every line read "loaded=1 renderList=1" no
        // matter whether refinement works. Printing the three inputs that drive the screen
        // space error (camera position, viewport height, fov) alongside the resulting SSE
        // is what actually separates "no camera", "camera too far" and "SSE wrong".
        {
            static std::uint64_t lastReportFrame = 0;
            static std::size_t lastReportedRendered = 0;
            constexpr std::uint64_t kReportInterval = 60;

            if ( last_rendered_count != lastReportedRendered ||
                 frame_number - lastReportFrame >= kReportInterval )
            {
                lastReportFrame = frame_number;
                lastReportedRendered = last_rendered_count;

                UtilityFunctions::print( godot::vformat(
                    "[Tileset3D] LOD f=%d cam=(%s,%s,%s) vpH=%s fov=%s rootSSE=%s maxSSE=%s "
                    "render=%d loaded=%d inFlight=%d bytes=%s",
                    static_cast<int>( frame_number ),
                    godot::String::num( view.position.x, 1 ),
                    godot::String::num( view.position.y, 1 ),
                    godot::String::num( view.position.z, 1 ),
                    godot::String::num( view.viewportHeight, 1 ),
                    godot::String::num( view.fovDegrees, 1 ),
                    godot::String::num( root->screenSpaceError, 2 ),
                    godot::String::num( maximum_screen_space_error, 1 ),
                    static_cast<int>( render_list.size() ),
                    static_cast<int>( loaded_tiles.size() ),
                    loader != nullptr ? loader->get_active_count() : 0,
                    godot::String::num_int64( static_cast<std::int64_t>( loaded_bytes ) ) ) );
            }
        }
    }

    void Tileset3D::traverse_tile( core::Tile &tile, const math::Mat4 &parent_world,
                                   const ViewState &view, const double nearest_conditional_ge )
    {
        tile.touchedFrame = frame_number;
        tile.visible = false;

        const math::Mat4 world = math::multiply( parent_world, tile.transform );
        tile.worldMatrix = world;

        if ( !tile.boundingVolume.has_value() )
        {
            return;
        }

        const math::Vec3 localCenter = math::boundingVolumeCenter( *tile.boundingVolume );
        const math::Vec3 worldCenter = math::transformPoint( world, localCenter );

        const double radius = tile.boundingVolumeRadius() * math::maxScale( world );
        tile.worldRadius = radius;

        // No frustum culling yet. Refinement is still bounded by the screen space error, so
        // off-screen geometry stops refining once it is far enough away; what is missing is
        // the extra saving from not visiting it at all.
        const double distanceToCenter = glm::length( worldCenter - view.position );

        const double surfaceDistance = math::computeBvSurfaceDistance(
            &( *tile.boundingVolume ), world, view.position,
            math::computeSurfaceDistance( distanceToCenter, radius ) );
        tile.distanceToCamera = surfaceDistance;

        const double sse = math::computeScreenSpaceError( tile.geometricError, surfaceDistance,
                                                          view.viewportHeight, view.fovDegrees );
        tile.screenSpaceError = sse;

        const bool hasContentUri = tile.content.has_value();
        const bool contentReady = tile.contentState == core::ContentState::Ready;

        // Ported line for line from the reference scheduler
        // (web-spatial-examples/.../threeDTiles/index.ts:1801-1815). The reference is the
        // authority for traversal semantics, not Cesium - and even where the reference
        // deliberately adopted Cesium/NASA behaviour, that decision is recorded *in the
        // reference*, so cite the reference and keep the pointer to it.

        // A tile that declares no content and is not ready must keep refining, otherwise it
        // would be a dead end with nothing to show. (reference: "对齐 Cesium")
        const bool forceRefine = !contentReady && !hasContentUri && tile.geometricError > 0.0;

        // The reference aligned this rule with NASA's canUnconditionallyRefine
        // (traverseFunctions.js:85-104). When a dataset's geometric errors are not
        // monotonically decreasing, a parent passing the SSE test says nothing about its
        // children, so refinement continues down to the level where the error converges.
        const bool unconditionallyRefine =
            !hasContentUri || tile.geometricError >= nearest_conditional_ge;

        const bool wantsRefine = tile.isExternalTileset || forceRefine || unconditionallyRefine ||
                                 sse > maximum_screen_space_error;

        // Implicit tiles have no children until their subtree is decoded. Materialise on the
        // first refinement so the tree below this tile is real before anything walks it; doing
        // it here rather than at load is what keeps a deep implicit root cheap to open.
        if ( wantsRefine && tile.implicitTiling.has_value() )
        {
            ensure_implicit_children( tile );
        }

        const double childConditionalGe =
            ( hasContentUri && !unconditionallyRefine ) ? tile.geometricError : nearest_conditional_ge;

        const auto selectSelf = [&]() {
            if ( contentReady || hasContentUri )
            {
                tile.visible = true;
                render_list.push_back( &tile );

                // Load priority, in the same order the reference scheduler ranks by: how
                // central the tile is to the view first, then how close it is. The foveated
                // factor is Cesium's simplified `_foveatedFactor` - 0 dead-centre, 1 at the
                // screen edge - and it matters more than distance because a tile behind the
                // camera is both far and useless.
                LoadPriority priority;
                priority.distance = surfaceDistance;
                priority.depth = tile.depth;

                const math::Vec3 toTile = worldCenter - view.position;
                const double toTileLength = glm::length( toTile );
                if ( toTileLength > 0.0 )
                {
                    const double alignment =
                        glm::dot( toTile / toTileLength, view.forward );
                    priority.foveated = 1.0 - std::abs( alignment );
                }

                request_content( tile, priority );
            }
        };

        if ( tile.refine == core::RefineMode::Add )
        {
            // ADD renders the parent and its children at the same time.
            selectSelf();

            if ( wantsRefine )
            {
                for ( const std::unique_ptr<core::Tile> &child : tile.children )
                {
                    traverse_tile( *child, world, view, childConditionalGe );
                }
            }
            return;
        }

        // REPLACE.
        if ( !wantsRefine )
        {
            selectSelf();
            return;
        }

        if ( tile.children.empty() )
        {
            // Wants to refine but has nothing below it: degrade to showing itself.
            selectSelf();
            return;
        }

        const bool hasRenderableContent = contentReady;

        // A parent only stops rendering once every child that has content is ready. Until
        // then it stays on screen and covers the gap the missing children would leave.
        bool refines = hasRenderableContent;

        for ( const std::unique_ptr<core::Tile> &childPtr : tile.children )
        {
            core::Tile &child = *childPtr;

            const bool childAvailable =
                !child.content.has_value() || child.contentState == core::ContentState::Ready;

            if ( hasRenderableContent && !childAvailable )
            {
                refines = false;
            }

            traverse_tile( child, world, view, childConditionalGe );
        }

        // Parent fallback to avoid holes while the children load.
        if ( hasRenderableContent && !refines )
        {
            tile.visible = true;
            render_list.push_back( &tile );
        }
    }

    void Tileset3D::request_content( core::Tile &tile, const LoadPriority &priority )
    {
        if ( tile.contentState != core::ContentState::Unloaded || tile.isExternalTileset ||
             !tile.content.has_value() )
        {
            return;
        }

        // Marked Loading right away so the same tile cannot be queued twice in one frame. The
        // loader tracking is separate: this queue is rebuilt every frame from the traversal,
        // whereas the loader holds the requests that are genuinely in flight.
        tile.contentState = core::ContentState::Loading;
        load_queue.push_back( QueuedLoad{ &tile, priority } );
    }

    void Tileset3D::dispatch_loads()
    {
        if ( loader == nullptr || load_queue.empty() )
        {
            return;
        }

        // Rank by what the camera is actually looking at. This matters because the queue can
        // hold far more entries than the concurrency budget allows: on a dense dataset it
        // reaches well over a thousand, and a FIFO would spend its bandwidth on tiles the
        // camera has already flown past, so the visible centre of the screen would arrive
        // last. Sorting here - after the traversal refreshed every priority this frame - is
        // what keeps the comparison from going stale.
        std::sort( load_queue.begin(), load_queue.end(),
                   []( const QueuedLoad &a, const QueuedLoad &b )
                   { return isHigherLoadPriority( a.priority, b.priority ); } );


        const int budget = maximum_simultaneous_loads;

        // Snapshot before the loop: entries that do not fit the budget are handed back to
        // Unloaded, and those must not be recorded as requested.
        std::vector<core::Tile *> requested;
        requested.reserve( load_queue.size() );

        // Anything the loader is already fetching is wanted by definition - the traversal no
        // longer queues it because it is in the Loading state, not because it lost interest.
        // It has to be recorded before the loop and before the budget check, otherwise the
        // cancel pass below would tear down a perfectly good in-flight request every frame.
        for ( core::Tile *tile : requested_last_frame )
        {
            if ( loader->has_request( tile ) )
            {
                requested.push_back( tile );
            }
        }

        for ( const QueuedLoad &queued : load_queue )
        {
            if ( loader->has_request( queued.tile ) )
            {
                // Already in flight; recorded by the pass above.
                continue;
            }

            if ( static_cast<int>( loader->get_active_count() ) >= budget )
            {
                // Out of budget. Hand the leftover tiles back so a later frame re-queues them
                // instead of leaving them stuck in Loading forever.
                queued.tile->contentState = core::ContentState::Unloaded;
                continue;
            }

            const String path = content_path( *queued.tile );
            if ( path.is_empty() )
            {
                queued.tile->contentState = core::ContentState::Failed;
                continue;
            }

            if ( loader->request( queued.tile, path, model_up_axis_ ) )
            {
                requested.push_back( queued.tile );
            }
            else
            {
                // Saturated or already requested; retry next frame.
                queued.tile->contentState = core::ContentState::Unloaded;
            }
        }


        // Everything asked for this frame is live; anything left over from last frame that is
        // absent here is a tile the traversal no longer wants and gets cancelled.
        for ( core::Tile *tile : requested_last_frame )
        {
            if ( std::find( requested.begin(), requested.end(), tile ) == requested.end() )
            {
                loader->cancel( tile );
            }
        }

        requested_last_frame = std::move( requested );
    }

    void Tileset3D::cancel_stale_loads()
    {
        if ( loader == nullptr || requested_last_frame.empty() )
        {
            return;
        }

        // Called when this frame produced no render list (camera hidden, tileset hidden), so
        // every in-flight fetch is by definition not for anything about to be drawn.
        for ( core::Tile *tile : requested_last_frame )
        {
            loader->cancel( tile );
        }

        requested_last_frame.clear();
    }

    void Tileset3D::adopt_completed_loads()
    {
        if ( loader == nullptr )
        {
            return;
        }

        std::vector<CompletedLoad> finished = loader->collect_completed();

        // Work left over from an earlier frame's budget goes first: it is strictly older than
        // anything that just arrived, and its decode has already been paid for.
        if ( !deferred_loads.empty() )
        {
            finished.insert( finished.begin(),
                             std::make_move_iterator( deferred_loads.begin() ),
                             std::make_move_iterator( deferred_loads.end() ) );
            deferred_loads.clear();
        }

        if ( finished.empty() )
        {
            return;
        }

        // The loader already did the expensive half on a worker thread; all that is left here
        // is instantiating nodes, which cannot be moved off the main thread. Bounded per frame
        // so a burst of arrivals cannot spike the frame time - the rest waits for the next
        // frame, and the loader keeps fetching behind it.
        int uploaded = 0;

        for ( CompletedLoad &load : finished )
        {
            core::Tile &tile = *load.tile;

            if ( load.generation != loader_generation )
            {
                // From a previous load() call; the tree it referred to is gone.
                continue;
            }

            if ( load.state != LoadState::Ready || load.prepared == nullptr )
            {
                tile.loadErrorCount += 1;

                if ( tile.loadErrorCount >= 3 )
                {
                    tile.contentState = core::ContentState::Failed;
                    UtilityFunctions::printerr( "[Tileset3D] giving up on '", load.source,
                                                "': ", load.error );
                }
                else
                {
                    // Retry: a transient failure should not permanently blank a tile.
                    tile.contentState = core::ContentState::Unloaded;
                }
                continue;
            }

            if ( uploaded >= maximum_uploads_per_frame )
            {
                // Out of this frame's budget. Rather than throwing the work away, park it in
                // the loader's own completion list and pick it up next frame - the decode is
                // already paid for, so discarding it would pay for it twice.
                tile.contentState = core::ContentState::Loading;
                deferred_loads.push_back( std::move( load ) );
                continue;
            }

            ++uploaded;

            const math::Mat4 world = tile.worldMatrix.value_or( math::identity() );
            const ContentNode created = assembleContentNode( *load.prepared, world );

            if ( !created )
            {
                tile.loadErrorCount += 1;

                if ( tile.loadErrorCount >= 3 )
                {
                    tile.contentState = core::ContentState::Failed;
                    UtilityFunctions::printerr( "[Tileset3D] giving up on '", load.source,
                                                "': ", String( created.error.c_str() ) );
                }
                else
                {
                    tile.contentState = core::ContentState::Unloaded;
                }
                continue;
            }

            // Hidden until sync decides, so a freshly attached tile does not flash for one
            // frame before its siblings arrive.
            created.node->set_visible( false );
            add_child( created.node );

            tile.contentUserData = created.node;
            tile.contentState = core::ContentState::Ready;
            tile.contentBytes = load.byteCount;
            loaded_tiles.push_back( &tile );
            loaded_bytes += load.byteCount;
        }
    }

    void Tileset3D::sync_content_visibility()
    {
        // Hide everything that is loaded, then reveal the selection. Walking the loaded list
        // rather than the tile tree keeps this proportional to what is actually attached.
        for ( core::Tile *tile : loaded_tiles )
        {
            auto *node = static_cast<godot::Node3D *>( tile->contentUserData );
            if ( node != nullptr )
            {
                node->set_visible( false );
            }
        }

        // The master `show` toggle suppresses the whole subtree; otherwise reveal the tiles
        // the traversal selected for rendering this frame.
        if ( show )
        {
            for ( core::Tile *tile : render_list )
            {
                if ( tile->contentUserData == nullptr ||
                     tile->contentState != core::ContentState::Ready )
                {
                    continue;
                }

                auto *node = static_cast<godot::Node3D *>( tile->contentUserData );
                node->set_visible( true );

                // Re-apply the world matrix: cheap, and it keeps content correct if the
                // georeference or this node moves after the tile was loaded.
                if ( tile->worldMatrix.has_value() )
                {
                    node->set_transform( toGodotTransform( *tile->worldMatrix ) );
                }
            }
        }

        if ( debug_mesh != nullptr )
        {
            debug_mesh->set_visible( show && debug_show_bounding_volume );
        }
    }

} // namespace tiles3d
