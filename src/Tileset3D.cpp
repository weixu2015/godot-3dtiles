// SPDX-License-Identifier: Unlicense

#include "Tileset3D.h"

#include "ContentFactory.h"
#include "GodotMathConvert.h"

#include "core/math/BoundingVolume.h"
#include "core/math/Mat4.h"
#include "core/math/ScreenSpaceError.h"
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
#include "godot_cpp/classes/mesh.hpp"
#include "godot_cpp/classes/standard_material3d.hpp"
#include "godot_cpp/classes/viewport.hpp"
#include "godot_cpp/classes/scene_tree.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/memory.hpp"
#include "godot_cpp/variant/array.hpp"
#include "godot_cpp/variant/color.hpp"
#include "godot_cpp/variant/packed_color_array.hpp"
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
        ClassDB::bind_method( D_METHOD( "dump_tree", "max_depth" ), &Tileset3D::dump_tree );

        ClassDB::bind_method( D_METHOD( "get_tile_count" ), &Tileset3D::get_tile_count );
        ClassDB::bind_method( D_METHOD( "get_maximum_depth" ), &Tileset3D::get_maximum_depth );
        ClassDB::bind_method( D_METHOD( "get_asset_version" ), &Tileset3D::get_asset_version );
        ClassDB::bind_method( D_METHOD( "get_root_geometric_error" ),
                              &Tileset3D::get_root_geometric_error );
        ClassDB::bind_method( D_METHOD( "get_last_error" ), &Tileset3D::get_last_error );
        ClassDB::bind_method( D_METHOD( "is_placed_by_georeference" ),
                              &Tileset3D::is_placed_by_georeference );
        ClassDB::bind_method( D_METHOD( "get_loaded_tile_count" ),
                              &Tileset3D::get_loaded_tile_count );
        ClassDB::bind_method( D_METHOD( "get_last_rendered_count" ),
                              &Tileset3D::get_last_rendered_count );

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
                // The traversal runs every frame through NOTIFICATION_PROCESS rather than a
                // _process override: godot-cpp does not declare _process as a virtual on
                // Node, so overriding it would not be called.
                set_process( true );

                // Convenience for editor testing: a node with a url in the scene loads as
                // soon as it is ready.
                if ( !url.strip_edges().is_empty() )
                {
                    load();
                }
                break;

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

            if ( debug_mesh != nullptr )
            {
                debug_mesh->set_visible( p_value );
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
        // absolute OS paths and res:// paths directly.
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
        // Build an ENU frame at the dataset's own ECEF centre so coordinates stay small
        // (float32 safe) and the dataset is centred on this node's origin. The fallback this
        // used to use - inverse(root->transform) - is identity for region datasets, which
        // left every coordinate at full ECEF magnitude (~6.4e6 m) and blew up float32
        // precision. We also bake in the Z-up -> Y-up flip so the result is oriented exactly
        // like a tileset placed under an explicit Georeference3D.
        //
        // The implicit frame is cached at load time (see below): the dataset's root bounding
        // volume is rewritten from region to box during load, so recomputing it afterwards
        // from the converted volume would be wrong.
        if ( model_matrix_.has_value() )
        {
            return *model_matrix_;
        }

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

        Ref<FileAccess> file = FileAccess::open( path, FileAccess::READ );
        if ( file.is_null() )
        {
            last_error = godot::vformat( "cannot open '%s' (error %d)", path,
                                         static_cast<int>( FileAccess::get_open_error() ) );
            UtilityFunctions::printerr( "[Tileset3D] ", last_error );
            emit_signal( "load_failed", last_error );
            return;
        }

        const String text = file->get_as_text();
        file->close();

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

        // Content URIs in a tileset are relative to the tileset document.
        base_directory = path.get_base_dir();

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

        UtilityFunctions::print( godot::vformat(
            "[Tileset3D] loaded '%s': version=%s tiles=%d maxDepth=%d rootGE=%.3f "
            "georeferenced=%s",
            path, asset_version, static_cast<int>( tile_count ), maximum_depth, root_geometric_error,
            placed_by_georeference ? "yes" : "no (origin-centred fallback)" ) );

        emit_signal( "tileset_loaded" );
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

    // ---------------------------------------------------------------------------
    // Content IO
    // ---------------------------------------------------------------------------

    String Tileset3D::content_path( const core::Tile &tile ) const
    {
        if ( !tile.content.has_value() )
        {
            return String();
        }

        // A content URI is relative to the tileset document.
        const String uri( tile.content->uri.c_str() );
        return uri.is_absolute_path() ? uri : base_directory.path_join( uri );
    }

    PackedByteArray Tileset3D::read_tile_payload( const core::Tile &tile ) const
    {
        const String path = content_path( tile );
        if ( path.is_empty() )
        {
            return PackedByteArray();
        }

        Ref<FileAccess> file = FileAccess::open( path, FileAccess::READ );
        if ( file.is_null() )
        {
            return PackedByteArray();
        }

        const PackedByteArray bytes =
            file->get_buffer( static_cast<std::int64_t>( file->get_length() ) );
        file->close();
        return bytes;
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

        process_load_queue();
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
                    "render=%d loaded=%d",
                    static_cast<int>( frame_number ),
                    godot::String::num( view.position.x, 1 ),
                    godot::String::num( view.position.y, 1 ),
                    godot::String::num( view.position.z, 1 ),
                    godot::String::num( view.viewportHeight, 1 ),
                    godot::String::num( view.fovDegrees, 1 ),
                    godot::String::num( root->screenSpaceError, 2 ),
                    godot::String::num( maximum_screen_space_error, 1 ),
                    static_cast<int>( render_list.size() ),
                    static_cast<int>( loaded_tiles.size() ) ) );
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

        const double childConditionalGe =
            ( hasContentUri && !unconditionallyRefine ) ? tile.geometricError : nearest_conditional_ge;

        const auto selectSelf = [&]() {
            if ( contentReady || hasContentUri )
            {
                tile.visible = true;
                render_list.push_back( &tile );
                request_content( tile );
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

    void Tileset3D::request_content( core::Tile &tile )
    {
        if ( tile.contentState != core::ContentState::Unloaded || tile.isExternalTileset ||
             !tile.content.has_value() )
        {
            return;
        }

        // Marked Loading right away so the same tile cannot be queued twice in one frame.
        tile.contentState = core::ContentState::Loading;
        load_queue.push_back( &tile );
    }

    void Tileset3D::process_load_queue()
    {
        int started = 0;

        for ( core::Tile *tile : load_queue )
        {
            if ( started >= maximum_simultaneous_loads )
            {
                // Hand it back so a later frame picks it up again.
                tile->contentState = core::ContentState::Unloaded;
                continue;
            }

            ++started;

            const PackedByteArray bytes = read_tile_payload( *tile );

            bool ok = bytes.size() > 0;
            String failure;

            if ( ok )
            {
                const math::Mat4 world = tile->worldMatrix.value_or( math::identity() );
                const ContentNode created = createContentNode( bytes, base_directory, world );

                if ( created )
                {
                    // Hidden until sync decides, so a freshly attached tile does not flash
                    // for one frame before its siblings arrive.
                    created.node->set_visible( false );
                    add_child( created.node );

                    tile->contentUserData = created.node;
                    tile->contentState = core::ContentState::Ready;
                    tile->contentBytes = static_cast<std::size_t>( bytes.size() );
                    loaded_tiles.push_back( tile );
                }
                else
                {
                    ok = false;
                    failure = String( created.error.c_str() );
                }
            }
            else
            {
                failure = "unreadable";
            }

            if ( ok )
            {
                continue;
            }

            // Retry a couple of times before giving up, so a transient problem does not
            // permanently blank a tile.
            tile->loadErrorCount += 1;

            if ( tile->loadErrorCount >= 3 )
            {
                tile->contentState = core::ContentState::Failed;
                UtilityFunctions::printerr( "[Tileset3D] giving up on '", content_path( *tile ), "': ",
                                            failure );
            }
            else
            {
                tile->contentState = core::ContentState::Unloaded;
            }
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
