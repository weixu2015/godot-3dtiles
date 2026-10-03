# godot-3dtiles 架构

面向 **Godot 4.X** 的 [OGC 3D Tiles](https://github.com/CesiumGS/3d-tiles) 加载与渲染GDExtension。**不依赖 cesium-native** —— 是一份从 3D Tiles 规范与参考实现出发的独立 C++20 实现。

---

## 1. 项目定位与范围

### 1.1 面向的场景

**以单个 3D Tiles 数据集为主体的区域级数字孪生应用。** 典型项目形态是：
一块园区、工程或城市级的实景三维 / 倾斜摄影数据，空间范围通常在几平方公里到几十平方公里；城市级项目可达几十到几百平方公里，但空间跨度一般不超过几十公里。

该数据集作为场景主体，用于展示、交互，并叠加 BIM、矢量、POI、传感器等业务图层。

本项目不面向“全球尺度、多数据集同时在线”的数字地球。**主分支不包含 `Globe3D`**，Globe 相关能力作为实验分支或未来扩展。

> 注意：区域级场景仍需正确处理 `RTC_CENTER` / `tileset.transform` 与局部坐标转换，避免 ECEF 大坐标直接进入单精度渲染管线；但通常不需要 Globe 级的 Origin Shift 或双精度引擎。

### 1.2 为什么主分支不做 globe

| 理由         | 说明                                                                                          |
| ------------ | --------------------------------------------------------------------------------------------- |
| 场景不匹配   | 绝大多数数字孪生项目只需要一块数据，椭球地表与全球四叉树 LOD 是纯开销                         |
| 复杂度隔离   | 全球尺度引入的问题（Origin Shift、椭球精度、跨数据集坐标统一）在小范围场景下不存在            |
| 已有独立分支 | `Globe3D` 的基础实现已完成，见 `feat/globe` 分支（P0 椭球地表 + P1 影像四叉树 + P3 轨道相机） |

如果确实需要数字地球，切到 `feat/globe` 分支继续。两边共享同一个 `src/core/` 内核，
`Globe3D` 上线后再合并回主分支的成本是可控的。

### 1.3 功能范围（明确不做）

| 不做                                                 | 原因                                                                          |
| ---------------------------------------------------- | ----------------------------------------------------------------------------- |
| `pnts` / `i3dm` / `cmpt`                             | 本项目的实际数据源（Cesium ion 摄影测量管线、geotwin 转换产物）不产出这些格式 |
| 3D Tiles 样式引擎（`tileset.json` 的 `styles`）      | 业务着色在 Godot 侧用材质做，更直接                                           |
| 全局优先级请求调度                                   | 单数据集场景下请求量可控，按遍历顺序即可                                      |
| 瓦片元数据（`EXT_structural_metadata` 等）的完整解析 | 仅解析到不影响渲染的程度                                                      |

---

## 2. 架构分层

```
┌──────────────────────────────────────────────────────────────┐
│  Godot 层（src/*.cpp，链接 godot-cpp）                        │
│  Tileset3D / Georeference3D / OriginAuthority / ContentFactory│
│  ── 节点、场景装配、double→float 窄化、帧同步                  │
├──────────────────────────────────────────────────────────────┤
│  内核层（src/core/，静态库 tiles3d_core）                      │
│  math/    tiles/    content/                                  │
│  ── 纯 C++20，不 include 任何 godot_cpp/* 头文件               │
└──────────────────────────────────────────────────────────────┘
```

**硬约束：`src/core/` 永远不得包含 `godot_cpp/*`。** 这条约束有两个作用：

1. 内核可以脱离引擎单测（`tests/` 直接链 `tiles3d_core`，不初始化 Godot）；
2. 内核可以安全地在 worker 线程调用 —— Godot 的 API 大多不是线程安全的。

该约束由 `src/core/CMakeLists.txt` 的注释与代码评审保证（无自动化门禁）。

### 2.1 `src/core/` 模块

| 路径                                       | 职责                                                                         |
| ------------------------------------------ | ---------------------------------------------------------------------------- |
| `math/GeoMath`                             | WGS84 椭球、大地坐标 ↔ ECEF、ENU 帧（`eastNorthUpToFixedFrame`）、region→OBB |
| `math/Mat4`                                | 4×4 双精度矩阵（列主序），`invert` / `multiply` / `transformPoint`           |
| `math/BoundingVolume`                      | `box` / `sphere` / `region` 三态包围体，中心 / 半径 / 细分                   |
| `math/ScreenSpaceError`                    | SSE 计算与雾效因子                                                           |
| `tiles/Tile`                               | 瓦片树节点：变换、包围体、几何误差、隐式瓦片元数据、内容状态                 |
| `tiles/TilesetJson`                        | `tileset.json` 解析（含坏数据的容错与报错）                                  |
| `tiles/TilesetJson` / `resolveModelUpAxis` | `asset.gltfUpAxis` 解析（缺失/非法回退 Y）                                   |
| `tiles/Subtree`                            | 1.1 隐式瓦片的 `.subtree` 二进制解码与展开                                   |
| `content/B3dmParser`                       | `b3dm` 容器拆包、Feature/Batch Table                                         |
| `content/GltfReader`                       | GLB 容器解析、typed accessor 读取、Draco 解码、材质/纹理/场景图              |
| `content/Ktx2Decoder`                      | `KHR_texture_basisu` → RGBA8                                                 |

### 2.2 `src/` Godot 层

| 文件                      | 职责                                                                      |
| ------------------------- | ------------------------------------------------------------------------- |
| `Godot3DTiles.cpp`        | 扩展入口、节点注册                                                        |
| `Tileset3D.{h,cpp}`       | 主节点：加载、遍历调度、内容装配驱动、调试线框                            |
| `Georeference3D.{h,cpp}`  | 显式地理参考帧（多 Tileset3D 共享）                                       |
| `OriginAuthority.{h,cpp}` | 原点来源（`LongitudeLatitudeHeight` / `EarthCenteredEarthFixed`）         |
| `ContentFactory.{h,cpp}`  | `GltfModel` → Godot `MeshInstance3D` / `ArrayMesh` / `StandardMaterial3D` |
| `GodotMathConvert.h`      | **唯一的** double→float 窄化点                                            |
| `FileHelper.cpp`          | 本地文件与 URL 读取                                                       |

---

## 3. 坐标与帧约定（最容易出错的部分）

### 3.1 内核是 Z-up ECEF

内核全程使用 **Z-up 的 ECEF**（地心地固坐标系）与**双精度**。Godot 使用 Y-up 与 **float32**，
两者的换算集中在两处：

- **Z-up → Y-up 翻转**：由 `Tileset3D::z_up_to_y_up()` 烘焙，等价于 **-90° 绕 X**
  （`M(v) = (vx, vz, -vy)`）。注意 **Godot 的 `Basis` 是行主序**，
  `Transform3D(1,0,0, 0,0,1, 0,-1,0, ...)` 的参数顺序是 `(xx,xy,xz, yx,yy,yz, zx,zy,zz)`。
- **double → float**：只允许在 `GodotMathConvert.h` 里发生。

### 3.2 精度：为什么必须有 ENU 帧

ECEF 坐标量级约 `6.4e6` 米。float32 在 6.4e6 处的分辨率约 **0.5 米** —— 足以让摄影测量数据
肉眼可见地散架。所以任何进入 Godot 渲染的坐标，都必须先被压到"数据集自身尺度"。

做法是在数据集中心建一个 **ENU（东-北-天）局部帧**，让坐标落在 `|coord| < 数据集半径`。

### 3.3 隐式地理参考的判定：**声明式，不是数值启发式**

`Tileset3D::compute_model_matrix()` 在没有 `Georeference3D` 父节点时，按下表决策：

| 条件                         | 模型矩阵                              | 理由                                                                                                                                                |
| ---------------------------- | ------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| 根节点有 `region` 包围体     | `z_up_to_y_up() · inverse(enuToEcef)` | region 是绝对 EPSG:4979 坐标，**规范明确规定不经过 tile 的 transform 链**，其渲染帧位置恒为 `modelMatrix × ECEF`；只能靠 modelMatrix 才能进入渲染帧 |
| 根节点**声明了** `transform` | 同上                                  | 作者声明了数据集相对 ECEF 的位姿，`transform[3]` 就是真实地表锚点                                                                                   |
| 两者都没有                   | `inverse(rootTransform)`              | 数据集以自己的坐标空间创作，对齐参考实现 `mat4Inverse(rootTile.transform)`；identity 时即保持作者坐标                                               |

**这是一条必须靠"声明"而非"数值"判断的规则。** 曾经用过"包围盒中心离地心距离是否接近
地球半径"的启发式，它把 Aerometrex 数据集误判成 georeferenced，后果见 §6.1。

实现细节：`Tile::hasDeclaredTransform` 由 `parseTilesetJson` 在真正读到 `transform` 时置 `true`。
**必须用 flag 而不是比较矩阵** —— 省略 `transform` 与显式写 identity 在解析后完全一样，
但语义不同。隐式瓦片（来自 `.subtree`）从不自带 `transform`，flag 恒为 `false`，这是正确的。

### 3.4 内容上轴校正

tile tree 恒定为 Z-up，但**内容的**上轴必须读 `asset.gltfUpAxis`
（`core::resolveModelUpAxis` 解析，缺失/非法回退 `Y` 以保持 1.0 行为）：

| 声明值      | 校正      | 矩阵效果               |
| ----------- | --------- | ---------------------- |
| `Z`         | 不旋转    | 恒等                   |
| `Y`（默认） | +90° 绕 X | `M(v) = (vx, -vz, vy)` |
| `X`         | -90° 绕 Y | `M(v) = (-vz, vy, vx)` |

由 `ContentFactory::createContentNode` 按上轴条件化应用，**绝不能写死**。原因见 §6.2。

### 3.5 `RTC_CENTER` 的合成顺序

`RTC_CENTER` 表达在 **tile 坐标系**里，因此它**不随上轴旋转**，而是作为内容根的**原点**
在旋转之后合成：

```
contentRoot.transform = <up-axis correction>，origin = RTC_CENTER
```

---

## 4. 渲染管线

### 4.1 内容装配：自建，不用 `GLTFDocument`

Godot 的 `GLTFDocument` 在这条链路上有两个硬伤：

1. 它把网格生成为 `ImporterMeshInstance3D`（一个**不渲染**的占位节点），转换成
   `MeshInstance3D` 由 `GLTFDocumentExtensionConvertImporterMesh` 完成，而该扩展在
   `modules/gltf/register_types.cpp` 里被 **`is_editor_hint()` 门控注册** ——
   **编辑器进程内永远不转换**。`@tool` 的 `Tileset3D` 在编辑器里拿到的全是占位节点，
   `countMeshInstances` 恒为 0。
2. 内嵌图片在编辑器里会走 `ResourceImporterTexture` 重导入管线，basePath 在项目外时
   产生 `Can't find file ... during reimport` 噪音。

因此走**自建装配**：`GltfReader`（内核）解析出 `GltfModel`，`ContentFactory` 手工组装
`ArrayMesh` + `StandardMaterial3D` + `ImageTexture`。

代价（已接受）：材质只覆盖摄影测量子集 —— `baseColorTexture` / `baseColorFactor` /
`KHR_materials_unlit` / `alphaMode` / `doubleSided` / sampler filter+wrap。
**不支持的能力（sparse、外部 buffer/图片 URI、meshopt、basisu）一律报名失败**，而不是静默错绘。

### 4.2 Draco 与 KTX2 必须内置

| 扩展                         | 为什么必须自己实现                                                                                                                                                |
| ---------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `KHR_draco_mesh_compression` | Cesium ion 管线产物**必带**；Godot 核心 glTF 导入器不实现（[godot#73738](https://github.com/godotengine/godot/issues/73738)），不是构建开关问题                   |
| `KHR_texture_basisu`         | 3D Tiles 1.1 的 glb `extensionsRequired` **必含**，图片是 `image/ktx2`；Godot 4.7 的 C++ 绑定**没有 KTX2 解码**（`image.hpp` 只有 `load_ktx_from_buffer` = KTX1） |

两者的实现分别见 `extern/third_party/draco/`（vendored）与 `extern/third_party/basisu/`。
KTX2 必须在进入 Godot 之前转成 RGBA8（`core::decodeKtx2`），**不能**走
`load_png_from_buffer` / `load_jpg_from_buffer` 分支。

### 4.3 遍历与 LOD

按 REPLACE 细化规则遍历，包含两条不显然的语义：

- 子瓦片内容仍在加载时，**父瓦片继续渲染**以填补空洞；
- 只在细化停止时（或对正在细化的子节点）才发出内容请求 ——
  所以相机靠近时，**根瓦片的 payload 通常永远不会被请求**。

---

## 5. 构建与测试

### 5.1 环境

| 项     | 值                                                                |
| ------ | ----------------------------------------------------------------- |
| Godot  | **4.7.2**（标准版，非 .NET）                                      |
| 绑定   | godot-cpp `10.0.0-stable`（git submodule，`extern/godot-cpp`）    |
| 编译器 | C++20。Windows 用 MSVC（**VS 2022 Community，MSVC 14.36.32532**） |
| SDK    | Windows SDK **10.0.22621.0**                                      |
| 生成器 | **Ninja（单配置）**                                               |
| CMake  | 3.22+                                                             |
| Python | 3.x（godot-cpp 绑定生成器需要）                                   |

> MSVC 与 SDK 版本必须与 `build/windows-editor/CMakeCache.txt` 一致 —— CMake 缓存了编译器
> 路径，指向不同 toolset 会拒绝重新配置。

### 5.2 命令

```bat
REM 只含 godot-cpp
git submodule update --init extern/godot-cpp

scripts\configure_debug.bat
scripts\debug_build_install.bat
```

或走预设：

```bash
cmake --preset windows-editor
cmake --build --preset windows-editor --parallel
cmake --install build/windows-editor
ctest --preset windows-editor
```

安装步骤把扩展与动态库写进 **`demo/addons`**，`demo/` 项目从这里加载 ——
所以在编辑器里打开 `demo/` 就能用上新构建的插件。

`scripts\msvc_env.bat` 用 `vswhere` 定位 Visual Studio 并激活 MSVC 环境。
**该脚本在受限/沙箱环境里会失败**：安全策略把 **`reg.exe` 列入程序黑名单**，而
`vcvars64.bat` / `VsDevCmd.bat` 靠注册表 `KitsRoot10` 定位 Windows SDK —— `reg.exe` 被拦后
SDK 路径不会注入 PATH，`rc.exe` / `mt.exe` 找不到，于是 CMake 的编译器自检在链接阶段失败
（`--mt=CMAKE_MT-NOTFOUND`、`RC Pass 1 ... failed: no such file or directory`），
或者 INCLUDE 被静默漏设，症状是全量 `fatal error C1083: stddef.h`。

**这是环境假故障，不是配置错误**（`rc.exe` / `mt.exe` 实际存在，`cl` / `link` 也能找到，
只有 SDK 那一段缺失）。两条出路：

- 在**不受该黑名单约束的普通终端**里构建（Developer Command Prompt，或先
  `call vcvars64.bat`），即下面的标准流程；
- 或改用 **`scripts\sandbox_msvc_env.bat`** —— 它手写 `PATH` / `INCLUDE` / `LIB` 全部路径，
  完全绕过 `vswhere` → `reg.exe`。

### 5.3 测试

`tests/` 是 doctest 套件，直接链 `tiles3d_core`，不需要 Godot 运行时。

```bash
build/windows-editor/tests/tiles3d_tests.exe            # 全量
build/windows-editor/tests/tiles3d_tests.exe -tc="*geomath*"   # 按用例名过滤
```

**基线提示**：`test_gltf_reader.cpp` 有 **6 个用例 / 7 个断言**长期失败（历史遗留）。
判断"是不是我改坏了"时，必须与该基线对照，而不是假定全绿。

### 5.4 真机渲染自测

`--headless` 是 dummy renderer，**截不出图**；要真实 GPU 渲染必须给 `--rendering-driver`：

```bash
"E:/Games/godot/Godot_v4.7.2-stable_win64_console.exe" --path demo \
  --rendering-driver opengl3 res://<scene>.tscn
```

现成场景：

| 场景                       | 用途                                                                    |
| -------------------------- | ----------------------------------------------------------------------- |
| `demo/dataset_audit.tscn`  | 18 个数据集全量鲁棒性审计，自动按数据集半径取景，输出 ok/warn/fail 汇总 |
| `demo/implicit_audit.tscn` | 单数据集（1.1 Photogrammetry）渲染验证，存图后退出                      |
| `demo/single_tileset.tscn` | 手动加载单个 tileset                                                    |
| `demo/node_3d.tscn`        | 基础场景                                                                |

审计脚本用 `get_tree().quit()` 自终止。**不要把编辑器（`-e`）留在前台等** —— 它会永久阻塞。

---

## 6. 避坑清单（血泪教训）

### 6.1 隐式帧不能用数值启发式判断

**症状**：整场景空帧，但调度报告 `loaded > 0`。

**根因**：曾用"根包围盒中心离地心的距离是否落在 `[极半径, 3×长半轴]`"来判断数据集是否
georeferenced。`Aerometrex-SanFrancisco-2cm` 的根是 `sphere`，其中心长度**恰好等于 WGS84
长半轴**（就是地表），于是被判为 georeferenced 走了 ENU 分支 —— 但它的根**没有 `transform`**，
其 b3dm 的 `RTC_CENTER` 携带**完整 ECEF 平移**，且 `RTC_CENTER` **不会**被 `ecefToEnu` 作用。
结果 tile 树被搬进 ENU 而内容留在 ECEF，两者错位约 **6370 km**。

**定位手段**：dump 每个 content 节点的 `global_position`，看到
`TileContent pos=(0, -6370199, 0)` 即为该症状。

**结论**：见 §3.3 —— 只认声明（`region` / `transform`）。

### 6.2 上轴校正绕 glTF 原点，写死会甩飞内容

**症状**：整场景全黑，但调度报告"已加载"。

**根因**：上轴校正是一个**绕 glTF 原点 `(0,0,0)` 的旋转**（`RTC_CENTER` 在旋转之外）。
顶点远离原点的内容（例如 taiwan 的顶点在 `(38722, 119689, 119)`）一旦被误旋转，会被甩到
**~169k 单位**外，超过相机 `far` 面。

**结论**：必须读 `asset.gltfUpAxis`，见 §3.4。

### 6.3 `eastNorthUpToFixedFrame` 在 ECEF 原点退化

在 `(0,0,0)` 处地表法线无定义：`normalizeSafe((0,0,0))` 会**原样返回零向量**，
于是 `up`、`east`、`north` 全部坍塌为零向量，旋转块成为全零矩阵 → 奇异 →
`invert()` 产出 **NaN** → Godot 报 `Condition "!v.is_finite()" is true`
（`instance_set_transform`），每个实例都失败。

**触发条件**：数据集的根 `transform` 是 identity，且根包围体以原点为中心
（典型是本地坐标系创作的测试数据集，如 `Icospheres`）。

**结论**：见 §3.3。该锐边已有单测固定
（`eastNorthUpToFixedFrame collapses at the ECEF origin`）。

### 6.4 godot-cpp / Godot 侧

| 坑                                                    | 说明                                                                                                       |
| ----------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| `_get_configuration_warnings` 等虚函数必须 **public** | 基类 `Node` 声明为 public，`register_virtuals` 模板需要访问；否则编译失败                                  |
| `RefCounted` 派生对象**禁止栈构造**                   | `SurfaceTool` 等会触发 "Godot Object created without binding callbacks"，必须 `Ref<T> x; x.instantiate();` |
| `SurfaceTool::generate_normals()` 只支持三角形        | 对 `PRIMITIVE_LINES` 报 engine error                                                                       |
| `Node3D::to_global()` 的参数是**节点本地坐标**        | 它会叠加自身全局变换，**不是**"把父空间点转世界"。误用后果隐蔽（`look_at` 目标错位、距离漏算）             |
| `SceneTree::get_root()` 返回 `Window*`                | 跨 include upcast 需要 `window.hpp` 完整类型；建议避开，用 `get_edited_scene_root()`                       |
| 图标 SVG 的 `width` 会变成基准尺寸                    | `icon.svg` 不要写固定小 `width`（如 `width=32`），否则 32 成为 Godot 的基准尺寸；给 `viewBox` 即可         |
| 改完资源必须 `--import`                               | 直接跑场景不触发重导入，会报 `Unable to open file: res://.godot/imported/<res>-<hash>.ctex`                |

### 6.5 MSVC `/W4 /WX` 下

| 坑                                  | 说明                                                                                                                                          |
| ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| 字面量窄化是 error                  | `C4305`：`Color{0.09,...}` 必须写 `0.09f`；`set_roughness(0.9)` → `0.9f`                                                                      |
| `D9025 overriding '/W4' with '/W3'` | CMake 既有的**无害**警告，不是错误                                                                                                            |
| 换新编译器时 `/WX` 易失败           | 一长串 `/w14xxx` 告警编号在新 toolset 下可能已不存在（`C4619`）。先临时 `-Dgodot-3dtiles_WARNING_AS_ERROR=OFF` 定位，再逐条处理，不要整体关掉 |

### 6.5.1 CMake 目标顺序（改根 `CMakeLists.txt` 时必看）

- `extern` 必须在 `src` **之前** subdirectory —— 否则 `tiles3d_core` 链接不到
  `tiles3d_third_party`。
- `ccache` 必须在目标创建**之前**设置 —— 否则 `CMAKE_CXX_COMPILER_LAUNCHER` 对主目标无效。

### 6.6 第三方依赖的构建陷阱

**Draco**

- `draco_add_library` 把 include dirs 标为 `PRIVATE`，唯一的 `PUBLIC` 是
  `$<INSTALL_INTERFACE:include>`（构建期为空的生成表达式）→ **`draco::draco` 不导出
  构建期 include**。本项目通过 `tiles3d_third_party` 这个 interface 目标重新导出。
- `add_subdirectory` 必须给独立 binary 目录（Draco 拒绝在源码树内配置）。
- 9 个必须显式关闭的选项，否则 configure 直接 `FATAL_ERROR`：
  `DRACO_TRANSCODER_SUPPORTED`、`DRACO_TESTS`、`DRACO_INSTALL`、`DRACO_GLTF`、
  `DRACO_GLTF_BITSTREAM`、`DRACO_MAYA_PLUGIN`、`DRACO_UNITY_PLUGIN`、`DRACO_JS_GLUE`、
  `DRACO_VERBOSE`。

**basisu / zstd**

- `.c` 源不能用：项目从未 `enable_language(C)`，加 `.c` 源会在 generate 期报
  `CMAKE_C_COMPILE_OBJECT` 未设置。解法：`set_source_files_properties(... LANGUAGE CXX)`。
- `basisu_transcoder.cpp` 有 24000 行，**必须 `/bigobj`**（否则 C1128）。
- include 目录要拆对：`transcoder` 必须是 **PUBLIC**，`zstd` 是 **PRIVATE**。
- **`basisu_transcoder_init()` 必须且只能调一次**。漏调会命中
  `assert(g_transcoder_initialized)` → **SIGABRT 直接崩进程**（不是返回 false）；
  调两次会报错日志。用 `std::call_once` 包裹。

**godot-cpp v10**

- 强制的 MSVC 静态 CRT `/MT`，与配置前创建的 consumer target 存在 runtime mismatch。
- v10 起 CMake 选项**全部改名**（如 `GODOT_ENABLE_HOT_RELOAD` → `GODOTCPP_USE_HOT_RELOAD`），
  用旧名字会**静默失效**。

### 6.7 GLM / doctest

- `doctest::Approx` 只有相对容差（`epsilon` / `scale`），**没有** Catch2 的 `margin()`。
  近零比较必须写显式绝对差：`std::abs(x) < tol`。
- GLM 1.0 的实验性扩展必须在**任何** GLM include 之前 `#define GLM_ENABLE_EXPERIMENTAL`。

### 6.8 数据集本身的陷阱

`E:/GISData/3D Tiles/Aerometrex-SanFrancisco-2cm` 是**损坏的样本**，不是代码问题：

- 根 `content.uri` 写 `Cesium_SF_2cm_mSL.b3d`，磁盘上却是**同名目录**，真文件在内部叫
  `Cesium_SF_2cm_mSL.b3dm`（少一层目录 + 扩展名少一个 `m`）。
- 深层引用的数百个外部 tileset（`Data/*/500/**/L22_*.json`）**整棵树不存在**
  （`find -name "L22*"` 返回 0）。

审计时把它当已知坏样本标注即可。

---

## 7. 决策记录

| #    | 决策点                      | 结论                                                                                        | 日期       |
| ---- | --------------------------- | ------------------------------------------------------------------------------------------- | ---------- |
| D-1  | 坐标系方案                  | 保留地理参考节点，`modelMatrix = localToEcef⁻¹ · inverse(root.transform)`                   | 2026-09-16 |
| D-2  | 第三方依赖                  | glm / nlohmann_json / doctest / `WorkerThreadPool`                                          | 2026-09-16 |
| D-3  | 类名与 API                  | **彻底去 Cesium 命名**                                                                      | 2026-09-16 |
| D-4  | 功能范围                    | 不做 `pnts`/`i3dm`/`cmpt`/样式引擎/全局优先级调度（见 §1.3）                                | 2026-09-16 |
| D-5  | 目标引擎版本                | **Godot 4.7.2**（不再兼容 4.3）                                                             | 2026-09-16 |
| D-6  | godot-cpp 版本              | **`10.0.0-stable`**（`GODOTCPP_API_VERSION` 选 API，无 4.6/4.7 分支）                       | 2026-09-16 |
| D-7  | 构建预设                    | **单配置 Ninja**（放弃 `Ninja Multi-Config`）                                               | 2026-09-16 |
| D-8  | Hot reload                  | 默认 **OFF**（`reloadable = false`）                                                        | 2026-09-16 |
| D-9  | `src/core/` 是否用 C++ 异常 | **完全不用**，前置条件违反用 `assert` + 定义明确的兜底返回                                  | 2026-09-17 |
| D-10 | 单测框架用法                | 只用 doctest 支持的断言；近零比较写显式绝对差（见 §6.7）                                    | 2026-09-17 |
| D-11 | glTF 内容装配路线           | **自建装配**（`GltfReader` + 手工 `ArrayMesh`），`GLTFDocument` 整条路径删除（理由见 §4.1） | 2026-09-17 |
| D-12 | Draco 引入方式              | **vendored Google Draco 1.5.7**，在 `GltfReader` 内直接解码为 SoA 顶点，不重序列化 GLB      | 2026-09-17 |
| D-13 | KTX2 支持                   | 自持 basisu transcoder + zstd，进 Godot 前转 RGBA8                                          | 2026-10-03 |
| D-14 | 隐式地理参考判定            | **声明式**（`region` 或声明的 `transform`），禁止数值启发式（理由见 §6.1）                  | 2026-10-03 |
| D-15 | 主分支是否包含 globe        | **不含**。主分支面向单数据集小范围场景；`Globe3D` 在 `feat/globe` 分支（理由见 §1.2）       | 2026-10-03 |

**D-9 的理由**：Godot 自身以禁用 C++ 异常的方式构建；godot-cpp 的默认
`GODOTCPP_DISABLE_EXCEPTIONS=ON` 会给消费者加 `_HAS_EXCEPTIONS=0`。在这条链接链上的库靠
`throw` 表达前置条件是隐患 —— 异常穿过不启用异常编译的代码是 UB 级风险。而内核里这些前置
条件违反（非 box 做细分、层级为负）本质是**调用方编程错误**，`assert` 才是对的工具。
附带收益：doctest 的 `CHECK_THROWS_*` 不再需要。

---

## 8. 验证方式

改动涉及坐标 / 帧 / 内容装配后，按顺序跑：

1. **单测**：`tests/tiles3d_tests.exe`，并与 §5.3 的基线对照（6 例失败是既有的）。
2. **全数据集审计**：`demo/dataset_audit.tscn`，要求
   **`is_finite` 错误 = 0、`ERROR` 计数 = 0、`fail = 0`**。
3. **代表性截图**：`demo/audit_shots/` 下的输出应能看到真实几何
   （摄影测量正射影像、城市模型、地形）。
4. **零回归对照**：改动前后用 `git stash` 暂存 `src/` `tests/`，重建跑基线，
   比对失败数与位置是否完全一致。

---

_本文档描述**当前**架构。历史实施过程不在此保留 —— 已完成阶段的叙述性计划没有长期价值，
且容易与代码脱节。有长期价值的只有：决策记录（§7）、避坑清单（§6）、以及根因分析。_
