# godot-3dtiles

A Godot 4.7 GDExtension that loads and renders [OGC 3D Tiles](https://github.com/CesiumGS/3d-tiles) directly — **no Cesium dependency**.

The plugin is a self-contained C++20 implementation: an engine-agnostic scheduling kernel plus a thin Godot layer. It reads `tileset.json`, walks the tile tree with screen-space-error refinement, fetches `b3dm` payloads, decodes Draco-compressed meshes and assembles them into Godot nodes.

![Demo](screenshot.png)

## Status

Working end to end: a local photogrammetry tileset (373 `b3dm` tiles, Draco compressed) loads, refines by screen-space error and renders.

This project does **not** use `cesium-native` or any Cesium code. It is an independent implementation of the 3D Tiles specification. (See [Credits](#credits) for the reference implementation used to pin down traversal semantics.)

## Registered nodes

| Node | Purpose |
|---|---|
| `Tileset3D` | Loads a `tileset.json`, runs the traversal/load scheduler. Exposes the tile bounding boxes as a depth-coloured wireframe for debugging |
| `Georeference3D` | Defines the render frame. Its transform carries the Z-up (tile) to Y-up (Godot) conversion |
| `Godot3DTiles` | Version/build information |
| `LongitudeLatitudeHeight` | Geodetic origin authority |
| `EarthCenteredEarthFixed` | ECEF origin authority |

## Architecture

```
src/core/    engine-agnostic kernel (static library tiles3d_core)
             tiles, tileset.json parsing, math (double precision),
             b3dm parsing, glTF reading, Draco decoding
             -> must NOT include godot_cpp/* : the kernel is unit tested
                without an engine and must never touch Godot API from a
                worker thread
src/         Godot layer: nodes, content assembly (ArrayMesh +
             StandardMaterial3D + ImageTexture), double->float conversion
```

The kernel works in `double` throughout. Godot's `real_t` is `float`, which at ECEF magnitudes (~6.4e6 m) has a resolution of about half a metre — enough to visibly scatter photogrammetry data. `src/GodotMathConvert.h` is the single place where narrowing happens.

Traversal follows the REPLACE refinement rules, including the non-obvious ones: a parent keeps rendering to cover holes while a child's content is still loading, and `requestContent` is only issued when refinement stops or for children being refined — so the root tile's payload is typically never fetched when the camera is close.

## Requirements

- **Godot 4.7.2** (standard build)
- **CMake** 3.22+
- A C++20 compiler — on Windows, MSVC (tested with VS 2022 / VS 2026)
- **Ninja** (the presets are single-config Ninja)
- **Python** 3.x (used by the godot-cpp binding generator)
- (Optional) **ccache**, **clang-format**

## Dependencies

| Dependency | How it is obtained |
|---|---|
| [godot-cpp](https://github.com/godotengine/godot-cpp) `10.0.0-stable` | git submodule at `extern/godot-cpp` |
| [GLM](https://github.com/g-truc/glm) 1.0.1 | fetched at configure time |
| [nlohmann/json](https://github.com/nlohmann/json) v3.11.3 | fetched at configure time |
| [Google Draco](https://github.com/google/draco) 1.5.7 | **vendored** in `extern/third_party/draco` (trimmed, Apache-2.0) |
| [doctest](https://github.com/doctest/doctest) v2.4.11 | fetched at configure time, tests only |

### Why Draco is bundled

Every tileset produced by the Cesium ion tiling pipeline uses `KHR_draco_mesh_compression`, and Godot's core glTF importer does not implement that extension ([godot#73738](https://github.com/godotengine/godot/issues/73738)). It is not a build flag or an editor/runtime difference — the decoder has to ship with the plugin. Draco is vendored rather than fetched because it is a native library and the build machine may have no network access.

Note that Draco does not export its include directories to consumers: its `add_library` macro sets them `PRIVATE`, and the only `PUBLIC` include is a `$<INSTALL_INTERFACE:include>` expression that is empty at build time. The plugin re-exports Draco's own include roots through the `tiles3d_third_party` interface target instead.

## Getting started

### Clone

```bash
git clone --recurse-submodules https://github.com/weixu2015/godot-3dtiles.git
```

If you forgot `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

### Build (Windows / MSVC)

```bat
scripts\configure_debug.bat
scripts\debug_build_install.bat
```

`scripts\msvc_env.bat` locates Visual Studio via `vswhere` and activates the MSVC environment, so run the scripts from a plain terminal.

For a release build use `configure_release.bat` / `release_build_install.bat`.

### Build (any platform, via presets)

```bash
cmake --preset windows-editor
cmake --build --preset windows-editor --parallel
cmake --install build/windows-editor
```

Available presets: `windows-editor`, `windows-release`, `linux-editor`, `linux-release`, `macOS-editor`, `macOS-release`. Windows is the actively developed and tested path.

The install step writes the extension and its library to **`demo/addons`**, which is where the bundled `demo` project loads it from — so opening `demo/` in the editor picks up the freshly built plugin.

### Run the tests

```bash
ctest --preset windows-editor
```

### Debugging in Visual Studio

1. Right-click the project and select `Properties`.
2. Set the debugging parameters:
   - **Command**: `${YourGodotEngineExePathIncludeFileName}`
   - **Command Arguments**: `-e --path ${YourGodotProjectDirectoryPath}`

## Credits

- Based on the GDExtension [template](https://github.com/asmaloney/GDExtensionTemplate) for CMake.
- Traversal and LOD semantics were derived from a production Three.js 3D Tiles scheduler; where Cesium's and that implementation's behaviour diverge, the latter is treated as the reference.
- Bundles [Google Draco](https://github.com/google/draco) (Apache-2.0).
