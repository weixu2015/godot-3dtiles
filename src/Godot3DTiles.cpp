// SPDX-License-Identifier: Unlicense

#include "Godot3DTiles.h"

#include "Version.h"

#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/version.hpp"

namespace tiles3d
{
    godot::String Godot3DTiles::version()
    {
        return VersionInfo::VERSION_STR.data();
    }

    godot::String Godot3DTiles::godotCPPVersion()
    {
        return "godot-cpp v" + godot::uitos( GODOT_VERSION_MAJOR ) + "." +
               godot::uitos( GODOT_VERSION_MINOR ) + "." + godot::uitos( GODOT_VERSION_PATCH ) + "-" +
               GODOT_VERSION_STATUS;
    }

    void Godot3DTiles::_bind_methods()
    {
        godot::ClassDB::bind_static_method( "Godot3DTiles", godot::D_METHOD( "version" ),
                                            &Godot3DTiles::version );
        godot::ClassDB::bind_static_method( "Godot3DTiles",
                                            godot::D_METHOD( "godot_cpp_version" ),
                                            &Godot3DTiles::godotCPPVersion );
    }

} // namespace tiles3d
