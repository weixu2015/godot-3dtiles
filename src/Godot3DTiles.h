// SPDX-License-Identifier: Unlicense

#ifndef GODOT_3DTILES_H
#define GODOT_3DTILES_H

#include "godot_cpp/classes/object.hpp"
#include "godot_cpp/variant/string.hpp"

namespace tiles3d
{
    /// Extension metadata, exposed to GDScript as the `Godot3DTiles` class.
    ///
    /// This used to be called `Cesium`; the rename is part of dropping the Cesium
    /// dependency entirely (docs/REFACTOR_PLAN.md D-4).
    class Godot3DTiles : public godot::Object
    {
        GDCLASS( Godot3DTiles, godot::Object )

    public:
        /// Version string of this extension, generated from git by cmake.
        static godot::String version();

        /// Version string of the godot-cpp build this extension links against.
        static godot::String godotCPPVersion();

    private:
        static void _bind_methods();
    };

} // namespace tiles3d

#endif
