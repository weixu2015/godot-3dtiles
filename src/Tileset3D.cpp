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
#include "godot_cpp/classes/rendering_server.hpp"
#include "godot_cpp/classes/sub_viewport.hpp"
#endif
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/time.hpp"
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

    /// Frames the editor framing pose is re-applied for before the camera is released.
    constexpr int kFramingFrames = 5;
    /// Upper bound on that hold, so a camera that never resolves is not pinned forever.
    constexpr int kFramingFrameCap = 600;
    /// Radians per degree, for the frustum maths.
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    /// Slack on the fitted framing distance. The fit is exact, so this is only there to keep
    /// the wireframe off the very edge of the viewport where it reads as "cut off".
    constexpr double kFramingMargin = 1.15;
    /// Frames between rebuilds of the debug wireframe while the aid is on.
    constexpr int kDebugMeshRefreshFrames = 15;

    /// Monotonic seconds, for the request deadline and the socket idle limit. Time::get_ticks_msec
    /// is monotonic where OS::get_ticks_usec is wall-clock, and this is a duration, not a date.
    double document_now_seconds()
    {
        const godot::Time *clock = godot::Time::get_singleton();
        return clock != nullptr ? static_cast<double>( clock->get_ticks_msec() ) / 1000.0 : 0.0;
    }

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

        /// True for a tile whose content is on screen or about to be replaced. A subtree
        /// below one of these is not reachable in any state the scheduler can produce, because
        /// a parent is always loaded before its children.
        bool tileIsLoaded( const core::Tile &tile )
        {
            return tile.contentState == core::ContentState::Ready ||
                   tile.contentState == core::ContentState::Expired;
        }

        /// Wireframe edges for the LOADED tiles only, and pruning at the first unloaded tile.
        ///
        /// Drawing the whole tree was never useful: a dataset with a few thousand tiles turns
        /// into a solid mat of wire that hides the thing it is supposed to be helping with, and
        /// the boxes of tiles that are not on screen describe nothing that is there. What the
        /// aid is for is "where did the data I can see just come from", so that is what it draws.
        void collectTileEdges( const core::Tile &tile, const math::Mat4 &parentWorld, double scale,
                               bool colorize, PackedVector3Array &vertices,
                               PackedColorArray &colors )
        {
            if ( !tileIsLoaded( tile ) )
            {
                return;
            }

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
        // Inspector layout. The pre-refactor Cesium3DTileset exposed 18 knobs; the self-hosted
        // loader started with 8, which is not enough to answer the questions this node gets asked
        // (why is this tile here, why is that one missing, what is the frame spending itself on).
        // Grouped so the Inspector reads as four questions rather than a wall: what to load, how
        // hard to refine, what it costs per frame, and what the loader currently holds.
        ADD_GROUP( "Tileset", "" );
        ClassDB::bind_method( D_METHOD( "set_url", "p_url" ), &Tileset3D::set_url );
        ClassDB::bind_method( D_METHOD( "get_url" ), &Tileset3D::get_url );
        ClassDB::add_property( "Tileset3D", PropertyInfo( Variant::STRING, "url" ), "set_url",
                               "get_url" );

        ADD_GROUP( "Refinement", "" );
        ClassDB::bind_method( D_METHOD( "set_maximum_screen_space_error", "p_value" ),
                              &Tileset3D::set_maximum_screen_space_error );
        ClassDB::bind_method( D_METHOD( "get_maximum_screen_space_error" ),
                              &Tileset3D::get_maximum_screen_space_error );
        ClassDB::add_property( "Tileset3D",
                               PropertyInfo( Variant::FLOAT, "maximum_screen_space_error",
                                             godot::PROPERTY_HINT_RANGE, "0.5,128,0.5,or_greater" ),
                               "set_maximum_screen_space_error",
                               "get_maximum_screen_space_error" );

        ClassDB::bind_method( D_METHOD( "set_maximum_level", "p_value" ),
                              &Tileset3D::set_maximum_level );
        ClassDB::bind_method( D_METHOD( "get_maximum_level" ), &Tileset3D::get_maximum_level );
        ClassDB::add_property( "Tileset3D",
                               PropertyInfo( Variant::INT, "maximum_level", godot::PROPERTY_HINT_RANGE,
                                             "-1,24,1" ),
                               "set_maximum_level", "get_maximum_level" );

        ClassDB::bind_method( D_METHOD( "set_suspend_update", "p_value" ),
                              &Tileset3D::set_suspend_update );
        ClassDB::bind_method( D_METHOD( "get_suspend_update" ), &Tileset3D::get_suspend_update );
        ClassDB::add_property( "Tileset3D", PropertyInfo( Variant::BOOL, "suspend_update" ),
                               "set_suspend_update", "get_suspend_update" );

        ADD_GROUP( "Loading", "" );
        ClassDB::bind_method( D_METHOD( "set_maximum_simultaneous_loads", "p_value" ),
                              &Tileset3D::set_maximum_simultaneous_loads );
        ClassDB::bind_method( D_METHOD( "get_maximum_simultaneous_loads" ),
                              &Tileset3D::get_maximum_simultaneous_loads );
        ClassDB::add_property( "Tileset3D",
                               PropertyInfo( Variant::INT, "maximum_simultaneous_loads",
                                             godot::PROPERTY_HINT_RANGE, "1,128,1" ),
                               "set_maximum_simultaneous_loads",
                               "get_maximum_simultaneous_loads" );

        ClassDB::bind_method( D_METHOD( "set_maximum_uploads_per_frame", "p_value" ),
                              &Tileset3D::set_maximum_uploads_per_frame );
        ClassDB::bind_method( D_METHOD( "get_maximum_uploads_per_frame" ),
                              &Tileset3D::get_maximum_uploads_per_frame );
        ClassDB::add_property( "Tileset3D",
                               PropertyInfo( Variant::INT, "maximum_uploads_per_frame",
                                             godot::PROPERTY_HINT_RANGE, "1,64,1" ),
                               "set_maximum_uploads_per_frame",
                               "get_maximum_uploads_per_frame" );

        ADD_GROUP( "Debug", "" );
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
        ClassDB::bind_method( D_METHOD( "set_manage_editor_clip", "p_value" ),
                              &Tileset3D::set_manage_editor_clip );
        ClassDB::bind_method( D_METHOD( "get_manage_editor_clip" ),
                              &Tileset3D::get_manage_editor_clip );
        ClassDB::add_property( "Tileset3D", PropertyInfo( Variant::BOOL, "manage_editor_clip" ),
                               "set_manage_editor_clip", "get_manage_editor_clip" );

        // Bound so the RenderingServer frame_pre_draw Callable can resolve it by name.
        ClassDB::bind_method( D_METHOD( "_on_frame_pre_draw" ), &Tileset3D::_on_frame_pre_draw );
        ADD_SIGNAL( godot::MethodInfo( "framing_released" ) );
        ClassDB::bind_method( D_METHOD( "set_dataset_up_axis", "p_value" ),
                              &Tileset3D::set_dataset_up_axis );
        ClassDB::bind_method( D_METHOD( "get_dataset_up_axis" ), &Tileset3D::get_dataset_up_axis );
        ClassDB::add_property( "Tileset3D", PropertyInfo( Variant::VECTOR3, "dataset_up_axis" ),
                               "set_dataset_up_axis", "get_dataset_up_axis" );

        ClassDB::bind_method( D_METHOD( "set_debug_print_lod", "p_value" ),
                              &Tileset3D::set_debug_print_lod );
        ClassDB::bind_method( D_METHOD( "get_debug_print_lod" ), &Tileset3D::get_debug_print_lod );
        ClassDB::add_property( "Tileset3D", PropertyInfo( Variant::BOOL, "debug_print_lod" ),
                               "set_debug_print_lod", "get_debug_print_lod" );

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

        ClassDB::bind_method( D_METHOD( "set_debug_colorize_tiles", "p_value" ),
                              &Tileset3D::set_debug_colorize_tiles );
        ClassDB::bind_method( D_METHOD( "get_debug_colorize_tiles" ),
                              &Tileset3D::get_debug_colorize_tiles );
        ClassDB::add_property( "Tileset3D", PropertyInfo( Variant::BOOL, "debug_colorize_tiles" ),
                               "set_debug_colorize_tiles", "get_debug_colorize_tiles" );

        // Read-only, and worth having in the Inspector rather than only on the console: these are
        // the numbers that answer "is it loading", "is it in the frustum", "where does it think
        // it is". Bound with an empty setter, which is how Godot spells a read-only property.
        ADD_GROUP( "Runtime (read-only)", "" );
        const auto readonly_string = []( const char *p_name ) {
            return PropertyInfo( Variant::STRING, p_name, godot::PROPERTY_HINT_NONE, "",
                                 godot::PROPERTY_USAGE_EDITOR | godot::PROPERTY_USAGE_READ_ONLY );
        };
        const auto readonly_int = []( const char *p_name ) {
            return PropertyInfo( Variant::INT, p_name, godot::PROPERTY_HINT_NONE, "",
                                 godot::PROPERTY_USAGE_EDITOR | godot::PROPERTY_USAGE_READ_ONLY );
        };
        const auto readonly_float = []( const char *p_name ) {
            return PropertyInfo( Variant::FLOAT, p_name, godot::PROPERTY_HINT_NONE, "",
                                 godot::PROPERTY_USAGE_EDITOR | godot::PROPERTY_USAGE_READ_ONLY );
        };
        const auto readonly_bool = []( const char *p_name ) {
            return PropertyInfo( Variant::BOOL, p_name, godot::PROPERTY_HINT_NONE, "",
                                 godot::PROPERTY_USAGE_EDITOR | godot::PROPERTY_USAGE_READ_ONLY );
        };
        const auto readonly_vector3 = []( const char *p_name ) {
            return PropertyInfo( Variant::VECTOR3, p_name, godot::PROPERTY_HINT_NONE, "",
                                 godot::PROPERTY_USAGE_EDITOR | godot::PROPERTY_USAGE_READ_ONLY );
        };

        ClassDB::bind_method( D_METHOD( "get_asset_version" ), &Tileset3D::get_asset_version );
        ClassDB::add_property( "Tileset3D", readonly_string( "asset_version" ), "",
                               "get_asset_version" );
        ClassDB::bind_method( D_METHOD( "get_tile_count" ), &Tileset3D::get_tile_count );
        ClassDB::add_property( "Tileset3D", readonly_int( "tile_count" ), "", "get_tile_count" );
        ClassDB::bind_method( D_METHOD( "get_maximum_depth" ), &Tileset3D::get_maximum_depth );
        ClassDB::add_property( "Tileset3D", readonly_int( "declared_maximum_depth" ), "",
                               "get_maximum_depth" );
        ClassDB::bind_method( D_METHOD( "get_root_geometric_error" ),
                              &Tileset3D::get_root_geometric_error );
        ClassDB::add_property( "Tileset3D", readonly_float( "root_geometric_error" ), "",
                               "get_root_geometric_error" );

        ClassDB::bind_method( D_METHOD( "get_loaded_tile_count" ),
                              &Tileset3D::get_loaded_tile_count );
        ClassDB::add_property( "Tileset3D", readonly_int( "loaded_tile_count" ), "",
                               "get_loaded_tile_count" );
        ClassDB::bind_method( D_METHOD( "get_last_rendered_count" ),
                              &Tileset3D::get_last_rendered_count );
        ClassDB::add_property( "Tileset3D", readonly_int( "last_rendered_count" ), "",
                               "get_last_rendered_count" );
        ClassDB::bind_method( D_METHOD( "get_in_flight_count" ), &Tileset3D::get_in_flight_count );
        ClassDB::add_property( "Tileset3D", readonly_int( "in_flight_count" ), "",
                               "get_in_flight_count" );
        ClassDB::bind_method( D_METHOD( "get_loaded_bytes" ), &Tileset3D::get_loaded_bytes );
        ClassDB::add_property( "Tileset3D", readonly_int( "loaded_bytes" ), "",
                               "get_loaded_bytes" );

        ClassDB::bind_method( D_METHOD( "get_dataset_longitude" ),
                              &Tileset3D::get_dataset_longitude );
        ClassDB::add_property( "Tileset3D", readonly_float( "dataset_longitude" ), "",
                               "get_dataset_longitude" );
        ClassDB::bind_method( D_METHOD( "get_dataset_latitude" ), &Tileset3D::get_dataset_latitude );
        ClassDB::add_property( "Tileset3D", readonly_float( "dataset_latitude" ), "",
                               "get_dataset_latitude" );
        ClassDB::bind_method( D_METHOD( "get_dataset_height" ), &Tileset3D::get_dataset_height );
        ClassDB::add_property( "Tileset3D", readonly_float( "dataset_height" ), "",
                               "get_dataset_height" );
        ClassDB::bind_method( D_METHOD( "get_dataset_radius" ), &Tileset3D::get_dataset_radius );
        ClassDB::add_property( "Tileset3D", readonly_float( "dataset_radius" ), "",
                               "get_dataset_radius" );
        // The dataset centre in this node's local space, i.e. the point automatic framing
        // aims at. Exposed because a script that wants to orbit or dolly around the data -
        // which is the only way to look at a single dataset interactively - needs somewhere
        // to orbit around, and this is that point. Under the implicit frame it is millions
        // of metres from the node origin, so it cannot be guessed from the transform.
        ClassDB::bind_method( D_METHOD( "get_dataset_center_local" ),
                              &Tileset3D::get_dataset_center_local );
        ClassDB::add_property( "Tileset3D", readonly_vector3( "dataset_center_local" ), "",
                               "get_dataset_center_local" );
        ClassDB::bind_method( D_METHOD( "get_anchor_separation" ),
                              &Tileset3D::get_anchor_separation );
        ClassDB::add_property( "Tileset3D", readonly_float( "anchor_separation_m" ), "",
                               "get_anchor_separation" );
        ClassDB::bind_method( D_METHOD( "is_placed_by_georeference" ),
                              &Tileset3D::is_placed_by_georeference );
        ClassDB::add_property( "Tileset3D", readonly_bool( "placed_by_georeference" ), "",
                               "is_placed_by_georeference" );
        ClassDB::bind_method( D_METHOD( "get_last_error" ), &Tileset3D::get_last_error );
        ClassDB::add_property( "Tileset3D", readonly_string( "last_error" ), "", "get_last_error" );

        ClassDB::bind_method( D_METHOD( "load" ), &Tileset3D::load );
        ClassDB::bind_method( D_METHOD( "reload" ), &Tileset3D::reload );
        ClassDB::bind_method( D_METHOD( "unload" ), &Tileset3D::unload );
        ClassDB::bind_method( D_METHOD( "rebase", "p_parent_delta" ), &Tileset3D::rebase );
        ClassDB::bind_method( D_METHOD( "dump_tree", "max_depth" ), &Tileset3D::dump_tree );

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
#ifdef TILES3D_EDITOR_TARGET
                // Re-assert the clip planes the instant before every frame is drawn. The
                // editor writes its own 0.1/4000 while navigating, so a write from the load
                // frame loses that race for the whole inertia tail - and a dataset framed
                // from kilometres away is exactly what a 4000 m far plane erases. A scene
                // with no globe has no GlobeTileLayer to do this, which is why it is here.
                godot::Engine *ready_engine = godot::Engine::get_singleton();
                if ( ready_engine != nullptr && ready_engine->is_editor_hint() &&
                     !pre_draw_connected_ )
                {
                    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
                    if ( rs != nullptr )
                    {
                        rs->connect( "frame_pre_draw",
                                     godot::Callable( this, "_on_frame_pre_draw" ) );
                        pre_draw_connected_ = true;
                    }
                }
#endif
                break;
            }

            case NOTIFICATION_PROCESS:
                update_tiles();
                break;

            case NOTIFICATION_EXIT_TREE:
                // The RenderingServer outlives the scene; a Callable left pointing at a
                // freed node is a use-after-free on the next editor frame.
                disconnect_pre_draw();
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

    void Tileset3D::set_maximum_uploads_per_frame( const int p_value )
    {
        maximum_uploads_per_frame = std::max( 1, p_value );
    }

    int Tileset3D::get_maximum_uploads_per_frame() const
    {
        return maximum_uploads_per_frame;
    }

    void Tileset3D::set_maximum_level( const int p_value )
    {
        maximum_level = p_value;
    }

    int Tileset3D::get_maximum_level() const
    {
        return maximum_level;
    }

    void Tileset3D::set_suspend_update( const bool p_value )
    {
        suspend_update = p_value;
    }

    bool Tileset3D::get_suspend_update() const
    {
        return suspend_update;
    }

    void Tileset3D::set_debug_print_lod( const bool p_value )
    {
        debug_print_lod = p_value;
    }

    bool Tileset3D::get_debug_print_lod() const
    {
        return debug_print_lod;
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

    void Tileset3D::set_manage_editor_clip( const bool p_value )
    {
        manage_editor_clip = p_value;
    }

    bool Tileset3D::get_manage_editor_clip() const
    {
        return manage_editor_clip;
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
        if ( dataset_radius <= 0.0 )
        {
            return;
        }

        // Whichever camera this framing is allowed to move: the editor's free camera while
        // editing, or the scene's own camera at runtime, so a single-dataset scene opens
        // looking at its data instead of at the world origin from inside it. Null when a
        // Globe3D owns the camera, or when there is no camera at all.
        godot::Camera3D *camera = resolve_framing_camera();
        if ( camera == nullptr )
        {
            return;
        }

        // Only the implicit frame gets automatic framing. With a Georeference3D ancestor the
        // dataset's position in the render frame is a number a person put there, and that
        // scene brings its own camera to match; re-framing it would throw their calibration
        // away on every load. This mirrors the `auto_frame_on_load` property's own default of
        // not touching anything the engine was not asked to place.
        if ( placed_by_georeference )
        {
            return;
        }

        const godot::Vector3 target =
            get_global_transform().xform( toGodotVector( dataset_center_ ) );

        // The eight box corners in world space, as offsets from the target. Framing a SPHERE
        // of the same radius is what this used to do, and it is not enough: a cube inscribed
        // in that sphere has corners sqrt(3) further out than the sphere, so up to 73% of the
        // wireframe fell outside the frustum - which is exactly the "bounding box is cut off"
        // report. The corners are the honest bound.
        godot::Vector3 corners[8];
        if ( !dataset_box_corners( corners ) )
        {
            return;
        }

        // Up. The model's own up, so the view is not arbitrarily rolled: for a dataset left
        // in ECEF by the implicit frame that is the geodetic up at its own centre, which is
        // the radial direction - 37.8 degrees away from ECEF's own Z axis at San Francisco,
        // enough to make a "level" horizon visibly tilted. `dataset_up_axis` overrides it for
        // a dataset authored in some other space. This only ever steers a camera; it cannot
        // move content, which is why the ECEF-scale test behind the default is acceptable here
        // when the same test is forbidden for placement.
        godot::Vector3 up_axis = dataset_up_axis;
        if ( up_axis.length_squared() < 1e-12f )
        {
            // glm::length(), NOT the member .length(): on this GLM (1.0.1) the member on a
            // dvec3 hands back 3.0 for a vector of 6.37e6, which made the ECEF test below
            // always fail and silently framed every dataset with a world +Y up. The free
            // function is what normalizeSafe() uses, and it is right.
            const double radial_length = glm::length( dataset_center_ );
            up_axis = radial_length > 1.0e6
                          ? get_global_transform().basis.xform(
                                toGodotVector( math::normalizeSafe( dataset_center_ ) ) )
                          : godot::Vector3( 0.0f, 1.0f, 0.0f );
        }
        up_axis = up_axis.normalized();

        // The horizontal half-angle comes from the viewport's aspect, so a tall narrow editor
        // viewport fits the dataset too instead of overflowing sideways.
        godot::SubViewport *viewport = editor_viewport();
        const double fov_v = static_cast<double>( camera->get_fov() ) * kDegToRad;
        double tan_half_v = std::tan( fov_v * 0.5 );
        if ( viewport != nullptr )
        {
            const godot::Rect2 rect = viewport->get_visible_rect();
            if ( rect.size.y > 0.0f )
            {
                tan_half_v *= static_cast<double>( rect.size.x / rect.size.y );
            }
        }
        const double tan_half_h = tan_half_v;

        // The view direction, looking DOWN at the model: `eye = target - forward * distance`,
        // so a forward with a POSITIVE up component would put the eye below the data and the
        // view underneath it - a city model is a plate, and from below it is a sliver on the
        // horizon. Hence the negative coefficient, which is the whole difference between
        // looking at a model and looking at its underside.
        const godot::Vector3 side = up_axis.cross( godot::Vector3( 0.0f, 0.0f, 1.0f ) );
        const godot::Vector3 reference = side.length_squared() > 1e-6f
                                              ? side.normalized()
                                              : godot::Vector3( 1.0f, 0.0f, 0.0f );
        const godot::Vector3 forward = ( reference * 0.55f - up_axis * 0.45f ).normalized();

        // The camera's up has to be ORTHOGONALISED against the view before it goes into a
        // Basis. Handing Basis(right, up, -forward) a up that is 63 degrees off forward builds
        // a sheared frame - the projection is then skewed and the dataset lands off in a
        // corner of the viewport instead of centred. set_camera_pose() does this internally for
        // its callers; building the Transform3D by hand does not, so it is done here.
        godot::Vector3 camera_up = up_axis - forward * up_axis.dot( forward );
        camera_up = camera_up.length_squared() > 1e-12f
                        ? camera_up.normalized()
                        : reference.normalized();
        // Godot's own convention, from set_camera_pose(): right = up x back, back = -forward.
        const godot::Vector3 right = camera_up.cross( -forward ).normalized();

        // Smallest distance at which every corner is inside both half-angles. For a corner at
        // offset v from the target, its depth past the eye is (d + v.forward) and its offsets
        // are v.right and v.up, so the binding corner gives d directly. No trigonometry to get
        // wrong and no assumption that the box is axis aligned.
        double distance = 0.0;
        for ( int i = 0; i < 8; ++i )
        {
            const godot::Vector3 v = corners[i] - target;
            const double depth = static_cast<double>( v.dot( forward ) );
            distance = std::max( distance,
                                 static_cast<double>( std::abs( v.dot( right ) ) ) / tan_half_h -
                                     depth );
            distance = std::max( distance,
                                 static_cast<double>( std::abs( v.dot( camera_up ) ) ) / tan_half_v -
                                     depth );
        }
        distance = std::max( distance, dataset_radius ) * kFramingMargin;
        // The fit above can leave the eye inside the box on a dataset that is much wider than
        // it is deep; one radius of clearance keeps the view outside the data.
        distance = std::max( distance, dataset_radius * 1.05 );

        const godot::Vector3 eye = target - forward * static_cast<float>( distance );
        // Godot cameras look down their local -Z.
        const godot::Basis basis( right, camera_up, -forward );
        camera->set_global_transform( godot::Transform3D( basis, eye ) );
        apply_framing_clip( camera );

        // Once per load, in the editor and at runtime alike. This is the line that settles
        // "why is the viewport empty": if it never appears the framing code did not run at
        // all (stale extension binary, a Globe3D in the scene owns the camera, or
        // auto_frame_on_load is off), and if it does, the numbers say where the camera went.
        if ( framing_frames_ == 0 )
        {
            UtilityFunctions::print( godot::vformat(
                "[Tileset3D] editor framing: cam=(%.1f, %.1f, %.1f) target=(%.1f, %.1f, %.1f) "
                "dist=%.1f m radius=%.1f m up=(%.3f, %.3f, %.3f) tan_half_h=%.4f tan_half_v=%.4f "
                "near=%.1f far=%.1f up_source=%s",
                camera->get_global_position().x, camera->get_global_position().y,
                camera->get_global_position().z, target.x, target.y, target.z, distance,
                dataset_radius, camera_up.x, camera_up.y, camera_up.z, tan_half_h, tan_half_v,
                static_cast<double>( camera->get_near() ),
                static_cast<double>( camera->get_far() ),
                dataset_up_axis.length_squared() < 1e-12f ? "geodetic" : "dataset_up_axis" ) );
        }
    }

    godot::SubViewport *Tileset3D::editor_viewport() const
    {
#ifdef TILES3D_EDITOR_TARGET
        // The guard matters now that frame_camera() is not editor-only and asks for this
        // first: EditorInterface::get_singleton() outside the editor logs an engine error
        // before it hands back null, and it ran once per re-applied framing frame.
        godot::Engine *engine = godot::Engine::get_singleton();
        if ( engine == nullptr || !engine->is_editor_hint() )
        {
            return nullptr;
        }
        godot::EditorInterface *editor = godot::EditorInterface::get_singleton();
        return editor != nullptr ? editor->get_editor_viewport_3d() : nullptr;
#else
        return nullptr;
#endif
    }

    bool Tileset3D::dataset_box_corners( godot::Vector3 r_corners[8] ) const
    {
        if ( dataset_corners_valid_ )
        {
            const godot::Transform3D to_world = get_global_transform();
            for ( int i = 0; i < 8; ++i )
            {
                r_corners[i] = to_world.xform( toGodotVector( dataset_corners_[i] ) );
            }
            return true;
        }
        return false;
    }

    void Tileset3D::set_dataset_up_axis( const godot::Vector3 &p_value )
    {
        dataset_up_axis = p_value;
    }

    godot::Vector3 Tileset3D::get_dataset_up_axis() const
    {
        return dataset_up_axis;
    }

    godot::Camera3D *Tileset3D::resolve_framing_camera() const
    {
        if ( godot::Camera3D *editor_camera = resolve_editor_camera() )
        {
            return editor_camera;
        }
        godot::Engine *engine = godot::Engine::get_singleton();
        if ( engine == nullptr || engine->is_editor_hint() )
        {
            // In the editor the free camera is the only one on offer, and resolve_editor_camera
            // has already had its say about it.
            return nullptr;
        }
        // At runtime, the scene's own camera - unless a Globe3D owns it, which is the same
        // rule the editor path follows.
        godot::Node *scene_root = const_cast<Tileset3D *>( this );
        while ( scene_root->get_parent() != nullptr )
        {
            scene_root = scene_root->get_parent();
        }
        if ( has_globe_in_scene( scene_root ) )
        {
            return nullptr;
        }
        godot::Viewport *viewport = get_viewport();
        return viewport != nullptr ? viewport->get_camera_3d() : nullptr;
    }

    godot::Camera3D *Tileset3D::resolve_editor_camera() const
    {
#ifdef TILES3D_EDITOR_TARGET
        godot::Engine *engine = godot::Engine::get_singleton();
        if ( engine == nullptr || !engine->is_editor_hint() )
        {
            return nullptr;
        }
        // A Globe3D in the same scene owns the editor camera; see has_globe_in_scene().
        godot::Node *scene_root = const_cast<Tileset3D *>( this );
        while ( scene_root->get_parent() != nullptr )
        {
            scene_root = scene_root->get_parent();
        }
        if ( has_globe_in_scene( scene_root ) )
        {
            return nullptr;
        }
        godot::SubViewport *viewport = editor_viewport();
        return viewport != nullptr ? viewport->get_camera_3d() : nullptr;
#else
        return nullptr;
#endif
    }

    void Tileset3D::assert_editor_clip( godot::Camera3D *p_camera )
    {
        if ( p_camera == nullptr || !manage_editor_clip || !is_inside_tree() )
        {
            return;
        }
        godot::Engine *engine = godot::Engine::get_singleton();
        if ( engine == nullptr || !engine->is_editor_hint() )
        {
            return;
        }
        apply_framing_clip( p_camera );
    }

    void Tileset3D::apply_framing_clip( godot::Camera3D *p_camera )
    {
        if ( p_camera == nullptr || dataset_radius <= 0.0 )
        {
            return;
        }
        const godot::Vector3 center_world =
            get_global_transform().xform( toGodotVector( dataset_center_ ) );
        const double center_distance = std::max(
            1.0, static_cast<double>( ( center_world - p_camera->get_global_position() ).length() ) );

        // Same shape as GlobeTileLayer::assert_editor_clip, with the dataset in place of the
        // planet: near grows with the distance so the depth ratio stays usable, and far has to
        // clear the far side of the bounding sphere rather than a multiple of the distance,
        // because a dataset framed from its own centre distance sits well inside that.
        p_camera->set_near(
            static_cast<float>( std::clamp( center_distance * 1e-4, 1.0, 1.0e6 ) ) );
        p_camera->set_far( static_cast<float>(
            std::max( center_distance * 4.0, center_distance + 3.0 * dataset_radius ) ) );
    }

    void Tileset3D::_on_frame_pre_draw()
    {
        assert_editor_clip( resolve_editor_camera() );
    }

    void Tileset3D::disconnect_pre_draw()
    {
        if ( !pre_draw_connected_ )
        {
            return;
        }
        pre_draw_connected_ = false;

        godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
        if ( rs == nullptr )
        {
            // Already tearing down; the server drops its own connections with it.
            return;
        }
        const godot::Callable callable( this, "_on_frame_pre_draw" );
        if ( rs->is_connected( "frame_pre_draw", callable ) )
        {
            rs->disconnect( "frame_pre_draw", callable );
        }
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
        dataset_center_ = math::Vec3( 0.0 );
        dataset_center_raw_ = math::Vec3( 0.0 );
        dataset_root_is_region_ = false;
        dataset_corners_valid_ = false;
        loaded_bytes = 0;
        needs_framing = false;
        framing_frames_ = 0;
        tiles_rendered_once_ = false;
        framing_released_emitted_ = false;
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

        // Find the nested tilesets now and read them over the next frames. The traversal, the
        // region conversion and the debug mesh all cope with a tree that is still filling in -
        // that is the same state the tile content itself is in while it streams - whereas doing
        // all of the reads here meant a dataset with hundreds of nested tilesets (taiwan: 2603)
        // blocked the editor's scene reopen for a minute.
        queue_external_tilesets();

        placed_by_georeference = find_georeference() != nullptr;
        const math::Mat4 model = compute_model_matrix();

        // The dataset centre, measured BEFORE the region conversion below rewrites the root
        // volume, because which composition puts it in the render frame depends on the form
        // the root declared: a region is absolute and only goes through the model matrix,
        // while anything else goes through the tile transform chain as well.
        const bool root_is_region =
            root->boundingVolume.has_value() &&
            root->boundingVolume->type == math::BoundingVolume::Type::Region;
        const math::Vec3 root_center_raw =
            root->boundingVolume.has_value() ? math::boundingVolumeCenter( *root->boundingVolume )
                                              : math::Vec3( 0.0 );

        // Regions are EPSG:4979 absolute coordinates and do not follow the transform chain,
        // so they are rewritten into the tile local frame once, here.
        core::convertRegionBoundingVolumes( *root, model, model );

        // Used by frame_camera() to fit the dataset in view after load.
        dataset_radius = root->boundingVolume.has_value()
                            ? math::boundingVolumeRadius( *root->boundingVolume )
                            : 0.0;

        // And the point it is fitted around, in this node's local space. The traversal starts
        // from the render frame and applies the root's own transform under it, so that is the
        // composition the tiles really land in - which is NOT this node's origin whenever the
        // implicit frame leaves the content in the tileset's own coordinates.
        dataset_center_raw_ = root_center_raw;
        dataset_root_is_region_ = root_is_region;
        dataset_center_ = current_dataset_center_local();

        // The same composition for the eight corners, which is what the editor framing
        // actually fits: a sphere of the root's radius is not a bound on a box, because a
        // cube inscribed in it has corners sqrt(3) further out than the sphere.
        dataset_corners_valid_ = false;
        if ( root->boundingVolume.has_value() )
        {
            const math::BoundingVolume &volume = *root->boundingVolume;
            math::Vec3 axes[3] = { math::Vec3( 0.0 ), math::Vec3( 0.0 ), math::Vec3( 0.0 ) };
            double sphere_radius = 0.0;
            if ( volume.type == math::BoundingVolume::Type::Box )
            {
                for ( int i = 0; i < 3; ++i )
                {
                    axes[i] = volume.boxHalfAxis( i ) * debug_bounding_volume_scale;
                }
            }
            else if ( volume.type == math::BoundingVolume::Type::Sphere ||
                      volume.type == math::BoundingVolume::Type::Region )
            {
                sphere_radius = math::boundingVolumeRadius( volume ) * debug_bounding_volume_scale;
            }
            if ( volume.type == math::BoundingVolume::Type::Box || sphere_radius > 0.0 )
            {
                const math::Mat4 to_render = root_is_region
                                                 ? model
                                                 : math::multiply( model, root->transform );
                for ( int index = 0; index < 8; ++index )
                {
                    math::Vec3 corner = root_center_raw;
                    if ( sphere_radius > 0.0 )
                    {
                        corner += math::Vec3( ( index & 1 ) != 0 ? sphere_radius : -sphere_radius,
                                              ( index & 2 ) != 0 ? sphere_radius : -sphere_radius,
                                              ( index & 4 ) != 0 ? sphere_radius : -sphere_radius );
                    }
                    else
                    {
                        corner += axes[0] * ( ( index & 1 ) != 0 ? 1.0 : -1.0 ) +
                                  axes[1] * ( ( index & 2 ) != 0 ? 1.0 : -1.0 ) +
                                  axes[2] * ( ( index & 4 ) != 0 ? 1.0 : -1.0 );
                    }
                    dataset_corners_[static_cast<std::size_t>( index )] =
                        math::transformPoint( to_render, corner );
                }
                dataset_corners_valid_ = true;
            }
        }

        // Flag the editor to frame the dataset once it has loaded (editor only).
        // Not editor-only: a single-dataset scene has no camera of its own until the author
        // adds one, and update_tiles() refuses to request anything without a camera to rank
        // against, so the same opening pose is what makes such a scene run at all. Scenes with
        // a georeference or a globe are left alone further down.
        needs_framing = auto_frame_on_load;
        framing_frames_ = 0;
        tiles_rendered_once_ = false;
        framing_released_emitted_ = false;

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

        // The local centre is what frame_camera() aims at and what assert_editor_clip() brackets,
        // and it is the one number that says whether the two agree with the node: a dataset
        // whose implicit frame leaves it in its own coordinates sits millions of metres from
        // this node's origin, and every "why is the editor viewport empty" question ends here.
        UtilityFunctions::print( godot::vformat(
            "[Tileset3D] render-frame centre: local=(%.1f, %.1f, %.1f) radius=%.1f m "
            "node_origin=(%.1f, %.1f, %.1f)",
            dataset_center_.x, dataset_center_.y, dataset_center_.z, dataset_radius,
            get_global_transform().origin.x, get_global_transform().origin.y,
            get_global_transform().origin.z ) );

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
        // Before clear_loaded(): the queue holds raw pointers into the tree that is about to be
        // destroyed.
        external_queue_.clear();
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

    void Tileset3D::queue_external_tilesets()
    {
        if ( root == nullptr )
        {
            return;
        }

        // Discovery only, no I/O: this runs inside load() and therefore inside the editor's
        // scene reopen, where anything blocking is charged to the editor's startup. The reads
        // themselves happen in process_external_tileset_queue(), a few per frame.
        external_queue_.clear();
        enqueue_external_descendants( *root );
    }

    void Tileset3D::enqueue_external_descendants( core::Tile &tile )
    {
        if ( tile.content.has_value() && is_external_tileset_uri( tile.content->uri ) )
        {
            // Once only. A container that already carries the external root's children has been
            // merged, and queueing it again would read the same document forever.
            if ( tile.children.empty() )
            {
                external_queue_.push_back( &tile );
            }
        }

        for ( const std::unique_ptr<core::Tile> &child : tile.children )
        {
            enqueue_external_descendants( *child );
        }
    }

    void Tileset3D::process_external_tileset_queue()
    {
        if ( external_queue_.empty() )
        {
            return;
        }

        // A time budget, not a document count: these documents differ in size by orders of
        // magnitude, and the thing to protect is the frame, not the number of round trips.
        const double deadline =
            document_now_seconds() + kExternalQueueBudgetSeconds;
        int merged_this_frame = 0;
        while ( !external_queue_.empty() && document_now_seconds() < deadline )
        {
            core::Tile *tile = external_queue_.front();
            external_queue_.pop_front();
            if ( tile == nullptr )
            {
                continue;
            }
            merge_external_tileset( *tile );
            // The document just spliced in brings its own children, and those may hold further
            // nested tilesets.
            for ( const std::unique_ptr<core::Tile> &child : tile->children )
            {
                enqueue_external_descendants( *child );
            }
            ++merged_this_frame;
        }

        if ( merged_this_frame > 0 )
        {
            // A merged document changes the tree the traversal sees, so the counts derived from
            // it (tile count, bounding volumes, the debug mesh) are stale until the next rebuild.
            // Nothing has to be told: the traversal reads the tree directly every frame.
        }
    }

    void Tileset3D::merge_external_tileset( core::Tile &tile )
    {
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

        // Park the wireframe's ORIGIN on the dataset instead of on this node, and move the
        // vertices the opposite way. Under the implicit frame the two are millions of metres
        // apart, and it is the origin the editor draws the selection gizmo at and the pivot
        // the user drags - a wireframe you can see but not click, whose gizmo is somewhere
        // off past the horizon, is the worst of both. Node-space offsets, so the node's own
        // authored transform still places the whole thing.
        const math::Vec3 origin_offset = dataset_center_;
        for ( int i = 0; i < vertices.size(); ++i )
        {
            const godot::Vector3 shifted = vertices[i] - toGodotVector( origin_offset );
            vertices[i] = shifted;
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

        // Rebuilds happen while the aid is on (see the caller in update_tiles), and the set of
        // loaded tiles changes as the camera moves, so the previous wireframe has to go rather
        // than accumulate as a stack of stale ones.
        if ( debug_mesh != nullptr )
        {
            remove_child( debug_mesh );
            memdelete( debug_mesh );
            debug_mesh = nullptr;
        }

        debug_mesh = memnew( MeshInstance3D );
        debug_mesh->set_name( "BoundingVolumeWireframe" );
        debug_mesh->set_mesh( mesh );
        debug_mesh->set_material_override( material );
        debug_mesh->set_visible( debug_show_bounding_volume );
        debug_mesh->set_position( toGodotVector( origin_offset ) );
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

    math::Vec3 Tileset3D::current_dataset_center_local() const
    {
        // Re-derived through the CURRENT frame, which is what makes it survive a re-anchor. The
        // frame's local zero is the anchor, so a node-local coordinate is only meaningful
        // relative to the anchor that was in place when it was written.
        if ( root == nullptr || !root->boundingVolume.has_value() )
        {
            return dataset_center_;
        }
        const math::Mat4 model = compute_model_matrix();
        return dataset_root_is_region_
                   ? math::transformPoint( model, dataset_center_raw_ )
                   : math::transformPoint( math::multiply( model, root->transform ),
                                           dataset_center_raw_ );
    }

    godot::Vector3 Tileset3D::get_dataset_center_local() const
    {
        return toGodotVector( current_dataset_center_local() );
    }

    double Tileset3D::get_anchor_separation() const
    {
        // Computed live rather than reported from the load-time snapshot taken in
        // report_georeference(). The anchor MOVES: switching a dataset re-anchors the frame onto
        // it (GlobeCameraController::reanchor_preserving_view), and a snapshot from before that
        // keeps saying the dataset is four thousand kilometres from where it actually is. The
        // demo's status panel prints this number with a warning beside it, so the stale value
        // reads as "the placement is broken" for a dataset that is placed perfectly - it is a
        // warning about a problem that was fixed the moment the anchor moved.
        //
        // The Georeference3D node sits at the world origin and the frame's local zero IS the
        // anchor, so the separation is simply the distance from the world origin to the dataset
        // centre in world space.
        const Georeference3D *reference = find_georeference();
        if ( reference == nullptr || dataset_radius <= 0.0 )
        {
            return -1.0;
        }
        const godot::Vector3 centre_world =
            get_global_transform().xform( toGodotVector( current_dataset_center_local() ) );
        return static_cast<double>( centre_world.length() );
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

    void Tileset3D::drop_http_reuse() const
    {
        http_reuse_ = godot::Ref<godot::HTTPClient>();
        http_reuse_host_ = godot::String();
        http_reuse_port_ = -1;
        http_reuse_last_use_ = -1e30;
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
                                         String &out_error, const bool p_retried ) const
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
            // The idle limit is not paranoia, it is the other half of the "empty response body"
            // story: a server closes an idle keep-alive socket on its own schedule (nginx's
            // keepalive_timeout is 75 s by default), the client cannot see that until it writes to
            // it, and then the request sits in STATUS_REQUESTING for ever. Reusing only a socket
            // that has been used recently keeps that window shut for all but a genuine race.
            const bool recently_used =
                document_now_seconds() - http_reuse_last_use_ < kReuseIdleLimitSeconds;
            const bool reusable =
                http_reuse_.is_valid() && http_reuse_host_ == host &&
                http_reuse_port_ == parts.port && recently_used &&
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
            bool sawErrorStatus = false;
            int errorCode = 0;
            bool timedOut = false;
            const double requestDeadline = document_now_seconds() + kDocumentRequestTimeoutSeconds;

            while ( client->get_status() == godot::HTTPClient::STATUS_REQUESTING ||
                    client->get_status() == godot::HTTPClient::STATUS_BODY )
            {
                // A request on a socket the server has already closed never gets a response: the
                // write succeeds into the dead connection and the client sits in STATUS_REQUESTING
                // for ever. Without a deadline this loop is the hang; measured on the taiwan
                // dataset, one such document used to spin until the traversal gave up on it.
                if ( document_now_seconds() > requestDeadline )
                {
                    timedOut = true;
                    break;
                }

                client->poll();

                // Read the body only while there IS one, and keep polling while the request is
                // still being written.
                //
                // The state to guard against is not "no response yet" but "the body is over":
                // poll() takes the client out of STATUS_BODY the moment the last chunk is in (a
                // Content-Length response goes straight to STATUS_DISCONNECTED), and reading then
                // is an engine-level ERR_FAIL that prints
                // `Condition "status != STATUS_BODY" is true` once per request - which is the
                // editor's error flood on the first load. has_response() is not a substitute: it
                // is also true in STATUS_DISCONNECTED, which is exactly when there is nothing
                // left to read.
                const godot::HTTPClient::Status status_after_poll = client->get_status();
                if ( status_after_poll == godot::HTTPClient::STATUS_REQUESTING )
                {
                    continue;
                }
                if ( status_after_poll != godot::HTTPClient::STATUS_BODY )
                {
                    break;
                }

                const int responseCode = client->get_response_code();
                if ( responseCode < 200 || responseCode >= 300 )
                {
                    // Not an early return: the body still has to be read out. A keep-alive socket
                    // is only reusable once its response has been consumed to the end, and leaving
                    // an error page on it makes the NEXT request on that socket parse those bytes
                    // as a status line - which is how a single 404 turned into every following
                    // external tileset failing with an empty body.
                    sawErrorStatus = true;
                    errorCode = responseCode;
                }

                godot::PackedByteArray chunk = client->read_response_body_chunk();
                if ( chunk.size() > 0 )
                {
                    body.append_array( chunk );
                    gotBody = true;
                }
            }

            if ( sawErrorStatus )
            {
                out_error = godot::vformat( "HTTP %d for '%s'", errorCode, path );
                return false;
            }

            if ( timedOut )
            {
                // The socket is not reusable after a stalled request, and saying so is more use
                // than "empty response body": the first tells the reader to look at keep-alive
                // timeouts, the second looks like a parse bug.
                drop_http_reuse();
                out_error = godot::vformat( "timed out after %.0f s waiting for '%s'",
                                            kDocumentRequestTimeoutSeconds, path );
                return false;
            }

            if ( !gotBody || body.size() == 0 )
            {
                // A kept-alive socket the server closed between documents still accepts the
                // request and never answers it - the write lands in a dead connection - which is
                // the whole of "empty response body" here. Losing a document means losing the
                // subtree under it, so this gets exactly one retry on a fresh connection; a file
                // that is genuinely missing costs two attempts and then reports the real reason.
                if ( !p_retried )
                {
                    drop_http_reuse();
                    return read_binary_document( path, out_bytes, out_error, true );
                }
                out_error = godot::vformat( "empty response body for '%s'", path );
                return false;
            }

            // A server that closes the connection after the response (Connection: close or
            // HTTP/1.0) leaves the client disconnected; drop it so the next call rebuilds
            // instead of trying to reuse a dead socket.
            if ( client->get_status() != godot::HTTPClient::STATUS_CONNECTED )
            {
                drop_http_reuse();
            }
            else
            {
                http_reuse_last_use_ = document_now_seconds();
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

        // Nested tileset documents first, so the traversal this frame sees as much of the real
        // tree as the budget allowed. Bounded by time, so a frame is never spent on documents
        // alone - which is what used to stall the editor's scene reopen.
        process_external_tileset_queue();

        // suspend_update: hold the selection where it is. The tree stays loaded and the
        // visibility of what is already selected is untouched, so the camera can be moved
        // (and the previous frame's result photographed) without the LOD chasing it - which is
        // the whole point when the question is "is this what I asked for".
        if ( suspend_update )
        {
            return;
        }

        // Editor convenience: fly the camera to frame the dataset once after it loads.
        // The wireframe draws the LOADED tiles (see collectTileEdges), and that set grows as
        // the camera moves, so a mesh built once at load time would be frozen at "the root only".
        // Rebuilt on a throttle: the walk now prunes at the first unloaded tile, so it costs
        // what the loaded subtree costs and not what the whole tree costs.
        if ( debug_show_bounding_volume && frame_number % kDebugMeshRefreshFrames == 0 )
        {
            build_debug_mesh();
        }

        if ( needs_framing )
        {
            // Re-applied for several frames, and held until something is actually on screen.
            // A single write is not enough: the editor drives its own viewport camera, so the
            // pose is gone again by the next navigation tick or viewport rebuild, and on the
            // first frames there is nothing to see anyway because no tile has been submitted
            // yet. GlobeTileLayer works the same way, for the same reason.
            frame_camera();
            ++framing_frames_;
            const bool hold_until_loaded = !tiles_rendered_once_ && framing_frames_ < kFramingFrameCap;
            if ( framing_frames_ >= kFramingFrames && !hold_until_loaded )
            {
                needs_framing = false;
            }
        }

        // Once the camera has been released, and only once per load. A script that wants to
        // take the view over has to be told, not left to guess from the frame count: polling
        // the camera's distance works right up until the frame timing shifts by one, and then
        // it reads the pose from before the framing ran.
        if ( !needs_framing && dataset_radius > 0.0 && !framing_released_emitted_ )
        {
            framing_released_emitted_ = true;
            emit_signal( "framing_released" );
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
        if ( last_rendered_count > 0 )
        {
            tiles_rendered_once_ = true;
        }

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

            if ( debug_print_lod &&
                 ( last_rendered_count != lastReportedRendered ||
                   frame_number - lastReportFrame >= kReportInterval ) )
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

        // A container for a nested tileset document is not content, and must never be requested
        // as if it were: the document arrives through the external-tileset queue (see
        // queue_external_tilesets), one budgeted read per frame. While that is still pending the
        // tile has a .json "content" URI, and asking the mesh pipeline for it produces a parse
        // failure that then counts as a spent retry - which is how a tree that is still filling in
        // ends up selected-but-empty (measured on taiwan: 1008 tiles rendered, 321 with content,
        // because the rest had already burned their attempt on a tileset.json).
        const bool content_is_document =
            tile.content.has_value() && is_external_tileset_uri( tile.content->uri );
        const bool hasContentUri = tile.content.has_value() && !content_is_document;
        const bool contentReady =
            tile.contentState == core::ContentState::Ready && !content_is_document;

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

        // maximum_level: the user's hard floor on depth. Applied last so it overrides every
        // reason to refine above it - including the unconditional ones, which exist to rescue
        // content that would otherwise be a dead end, and which would otherwise make the cap
        // unreachable on exactly the datasets that need it most. -1 means "no extra cap", and
        // the tileset's own levelCap() (implicit tiling) still applies as before.
        const bool under_level_cap = maximum_level < 0 || tile.depth < maximum_level;
        const bool refine_allowed = wantsRefine && under_level_cap;

        // Implicit tiles have no children until their subtree is decoded. Materialise on the
        // first refinement so the tree below this tile is real before anything walks it; doing
        // it here rather than at load is what keeps a deep implicit root cheap to open.
        if ( refine_allowed && tile.implicitTiling.has_value() )
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

            if ( refine_allowed )
            {
                for ( const std::unique_ptr<core::Tile> &child : tile.children )
                {
                    traverse_tile( *child, world, view, childConditionalGe );
                }
            }
            return;
        }

        // REPLACE.
        if ( !refine_allowed )
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
