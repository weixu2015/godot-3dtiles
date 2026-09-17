// SPDX-License-Identifier: Unlicense
//
// GDExtension entry point: registers this extension's classes with Godot's ClassDB.

#include "gdextension_interface.h"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/defs.hpp"
#include "godot_cpp/godot.hpp"

#include "Georeference3D.h"
#include "Godot3DTiles.h"
#include "OriginAuthority.h"
#include "Tileset3D.h"

/// @file
/// Register our classes with Godot.

using namespace tiles3d;

namespace
{
    /// @brief Called by Godot to let us register our classes with Godot.
    ///
    /// @param p_level the level being initialized by Godot
    ///
    /// @see GDExtensionInit
    void initializeExtension( godot::ModuleInitializationLevel p_level )
    {
        if ( p_level != godot::MODULE_INITIALIZATION_LEVEL_SCENE )
        {
            return;
        }

        // Types that do not depend on the 3D Tiles scheduler.
        godot::ClassDB::register_class<Godot3DTiles>();
        godot::ClassDB::register_class<LongitudeLatitudeHeight>();
        godot::ClassDB::register_class<EarthCenteredEarthFixed>();

        // The georeference and the tileset node. Tileset3D currently parses a tileset and
        // draws its bounding volumes as wireframes; content rendering arrives with phases 3
        // and 4 of docs/REFACTOR_PLAN.md.
        godot::ClassDB::register_class<Georeference3D>();
        godot::ClassDB::register_class<Tileset3D>();
    }

    /// @brief Called by Godot to let us do any cleanup.
    ///
    /// @see GDExtensionInit
    void uninitializeExtension( godot::ModuleInitializationLevel p_level )
    {
        if ( p_level != godot::MODULE_INITIALIZATION_LEVEL_SCENE )
        {
            return;
        }
    }
}

extern "C"
{
    /// @brief This is the entry point for the shared library.
    ///
    /// @note The name of this function must match the "entry_symbol" in
    /// templates/template.*.gdextension.in
    ///
    /// @param p_get_proc_address the interface (need more info)
    /// @param p_library the library (need more info)
    /// @param r_initialization the intialization (need more info)
    ///
    /// @returns GDExtensionBool
    GDExtensionBool GDE_EXPORT GDExtensionInit(
        GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library,
        GDExtensionInitialization *r_initialization )
    {
        {
            godot::GDExtensionBinding::InitObject init_obj( p_get_proc_address, p_library,
                                                            r_initialization );

            init_obj.register_initializer( initializeExtension );
            init_obj.register_terminator( uninitializeExtension );
            init_obj.set_minimum_library_initialization_level(
                godot::MODULE_INITIALIZATION_LEVEL_SCENE );

            return init_obj.init();
        }
    }
}
