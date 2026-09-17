# 3dtiles-godot 重构计划：移除 cesium-native，自研 3D Tiles 调度器并对接 Godot 渲染管线

- **分支**：`dev`
- **参考实现**：`D:\Develop\codespace\web-spatial-examples\apps\main\src\views\threeDTiles`（Three.js + TypeScript 自研调度器，~2870 行 `index.ts` + 配套模块）
- **目标**：用 **C++20** 复刻该调度器，删除 `cesium-native` 依赖，并将"瓦片内容"对接为 **Godot 原生节点树**（`Node3D` / `MeshInstance3D`）
- **状态**：计划已评审，4 项关键决策已确认（见 §8）。**Phase 0 已就绪，尚未编码。**

---

## 0. 现状勘察结论（事实，非推测）

### 0.1 仓库实际状态

| 项 | 实况（2026-09-16，Phase 0 执行后） |
|---|---|
| 当前分支 | `dev` |
| `extern/godot-cpp` | 目录仅含 `.git`，工作树为空；gitlink 停在 `fbbf9ec`（`godot-4.3-stable`）**待升级到 `10.0.0-stable`** |
| `extern/cesium-native` | **已移除**（`.gitmodules` 条目 / gitlink / 目录 / 构建脚本引用全部清除） |
| `build/` | 存在（内容为旧 4.3 时代的构建，建议清理重建） |
| 仓库根 `CMakeFiles/` | **已移除**（4 个文件曾被误提交，已 `git rm`；`.gitignore` 已补规则） |
| Cesium 耦合源文件 | **已删除 19 个**（~2900 行），保留 4 个 Cesium-free 文件 |
| 新增 | `extern/third_party/`、`src/core/`（静态库）、`tests/`（doctest） |
| 最近提交 | `16645f9 doc: Update README`（Phase 0 改动尚未提交） |

> ⚠️ 结论：**当前仓库无法构建**。子模块为空 + `extern/CMakeLists.txt` 第 4 行对 `godot-cpp/Makefile` 做 `FATAL_ERROR` 校验。因此"恢复可构建基线"必须先于一切改造。

### 0.2 Cesium 耦合面清点（按 src 行数）

| 文件 | 行数 | 对 cesium-native 的耦合 | 处置 |
|---|---|---|---|
| `Cesium3DTileset.cpp/.h` | 653 / 105 | **重度**：`Tileset` / `ViewUpdateResult` / `ViewState` / `TileLoadState` / `TilesetOptions` / `SupportedGpuCompressedPixelFormats` | **重写** |
| `GodotPrepareRendererResources.cpp/.h` | 1306 / 178 | **重度**：`IPrepareRendererResources` / `CesiumGltf::Model` / `AccessorView` / `ImageAsset` / `GltfUtilities` | **删除**，职责转交 `ContentFactory`（路线见 §2.D2 / §4.S1） |
| `CameraManager.cpp/.h` | 136 / 16 | **重度**：`ViewState` / `LocalHorizontalCoordinateSystem` / `GlobeTransforms` | **重写** |
| `GodotTilesetExternals.cpp/.h` | 66 / 31 | **重度**：`TilesetExternals` / `AsyncSystem` / `CachingAssetAccessor` / `SqliteCache` / `CreditSystem` | **删除** |
| `GodotAssetAccessor.cpp/.h` | 403 / 35 | **重度**：`IAssetAccessor` / `AsyncSystem::Future` / `Promise` | **重写**（底层仍用 Godot `HTTPClient`） |
| `CesiumGeoreference.cpp/.h` | 137 / 36 | **中度**：`LocalHorizontalCoordinateSystem` / `Cartographic` | **改写**（需自带 LHCS） |
| `CesiumOriginAuthority.cpp/.h` | 132 / 70 | 无（纯 Godot Resource） | **保留**（仅改名） |
| `CesiumEllipsoid.cpp/.h` | 127 / 97 | **轻度**：椭球/大地坐标 → 由自研 `GeoMath` 顶替 | **重写为 core 层** |
| `GodotTaskProcessor.cpp/.h` | 9 / 12 | **中度**：`ITaskProcessor` | **删除**，改用 `WorkerThreadPool` |
| `CesiumTilesetExcluder.cpp` / `CesiumTileExcluder.h` | 14 / 13 | **轻度**（仅 TODO 空实现） | **删除** |
| `ThreadUtils.hpp` | 206 | 无（自研 thread_pool，含 busy-yield 轮询） | **删除**，改用 `WorkerThreadPool` |
| `Cesium.cpp/.h` | 40 / 17 | 无（版本信息） | **保留**（仅改名） |
| `FileHelper.cpp/.h` | 49 / 15 | 无 | **保留**（并入 `core/net/`） |
| `RegisterExtension.cpp` | 71 | **轻度**：`registerAllTileContentTypes()` | **改写** |

**合计需重写/删除：实际删除 5786 行**（19 个文件，以 `git diff --cached --stat` 为准）；净新增预计 **4000–5500 行**（含 ~1200 行测试与 ~300 行 spike 代码）。

> 注：上表中各文件的"行数"来自 `Measure-Object -Line`，它**不计空行**，故单文件数字与总计偏小；真实总量以 git 统计的 5786 行为准。

### 0.3 参考实现的可移植性评估

参考实现的 `index.ts` 分为两类，**这是整个方案成立的基石**：

| 类别 | 组成 | 移植方式 |
|---|---|---|
| **引擎无关（约 65%）** | `parseTilesetJson`/`parseTile`、`BitStream`、`SubtreeReader`、`ImplicitTileManager`、`B3dmParser`、包围体与 region→ECE 数学、SSE/表面距离/fog、`Tileset::traverse*` 全部遍历决策、`TilesetCache`+`DoublyLinkedList`、加载队列/并发/取消 | **近乎 1:1 直译**为 C++20，进 `src/core/`（不含任何 Godot 头文件，可独立单测） |
| **引擎相关（约 35%）** | `getGltfLoader()`（GLTFLoader+DRACO+KTX2）、`THREE.Frustum/Matrix4/Vector3`、`contentData: Object3D`、`scene.add/visible/matrix`、`DebugTilesHelpers`（LineSegments+Sprite+canvas 标签）、PBR 材质转换 | **替换**为 Godot 对应物（见 §3） |

**关键判断**：调度算法本身与渲染引擎解耦，这是本重构风险可控的根本原因。真正需要"重新设计"的只有**内容装配**与**帧/坐标系**两处。

### 0.4 现有内容装配代码的缺陷清单（决定 §2.D2 的取舍依据）

对 `GodotPrepareRendererResources.cpp` 逐函数核对，确认以下问题：

| # | 位置 | 问题 | 严重度 |
|---|---|---|---|
| 1 | `loadTexture` L411-427 | 命中 `Model::getSafe(&model.textures, idx)` 返回 `nullptr` 时，**未判空即取 `pTexture->source`** → UB | **P0** |
| 2 | `setGltfMaterialParameterValues` | `KHR_texture_transform` 写成 `material->set_uv1_scale/offset`，这是**材质级**而非**贴图级**；多贴图各自 transform 不同时结果错误 | P1 |
| 3 | 同上 | sampler 只映射 `minFilter`，`magFilter` 仅在不设 minFilter 时用；**wrapS/wrapT 完全未处理** | P1 |
| 4 | `prepareInMainThread` | `primitiveInfo.containsPoints` **从未被赋值 true**，点云分支为死代码 | P2 |
| 5 | `extractVertexData` | 把刚构建的交错顶点缓冲**再解析一遍**成 4 个 `std::vector`，重复劳动 | P2 |
| 6 | 全局 | 无 skinning / morph target / sparse accessor / `KHR_materials_*`（transmission、clearcoat、sheen、specular、iridescence）/ `KHR_lights_punctual` | P2 |
| 7 | `free()` L1366-1406 | `tiles_destroyed` 早退会**直接泄漏** `pLoadThreadResult`；`isFreed` 检查 + `delete pGltfNode` 与 Godot `queue_free()` 混用，存在**双重释放**路径 | **P0** |
| 8 | `GodotAssetAccessor::get` L363-455 | 异步 lambda 内 **busy-poll `httpClient->poll()`**；且 `promise.resolve(...)` **之后未 `return`**，必然继续执行到 `promise.reject(...)` | **P0** |
| 9 | `create_physics_meshes` 默认 true | 每个 primitive 调 `create_convex_collision()`，瓦片多时构建开销极大 | P1 |

> 这张表是 §2.D2 判断的直接依据：既有的"可控性"优势伴随真实缺陷成本。

---

## 1. 架构目标

### 1.1 分层

```
┌──────────────────────────────────────────────────────────────────┐
│  Godot 引擎（Node3D / RenderingServer / Resource）                │
└────────────────────────────▲─────────────────────────────────────┘
                             │ Godot C++ API（仅主线程）
┌────────────────────────────┴─────────────────────────────────────┐
│  src/godot/  —— Godot 集成层（GDExtension）                       │
│  Tileset3D(Node3D) · Georeference3D(Node3D)                      │
│  AssetAccessor · ContentFactory · SceneSync                      │
│  CameraManager · FrameBudgetQueue · DebugTilesHelpers            │
└────────────────────────────▲─────────────────────────────────────┘
                             │ 纯 C++20 接口（无 Godot 依赖）
┌────────────────────────────┴─────────────────────────────────────┐
│  src/core/  —— 调度器内核（可独立单测，可复用于其他引擎）          │
│  tiles/  Tileset · Tile · TilesetCache · TilesetJson             │
│  implicit/ BitStream · SubtreeReader · ImplicitTileManager       │
│  content/ ContentType · B3dmParser · GlbReader                   │
│  math/   Mat4 · BoundingVolume · GeoMath · ScreenSpaceError      │
│  net/    IAssetAccessor · RequestScheduler · FileHelper          │
└──────────────────────────────────────────────────────────────────┘
```

**铁律**：`src/core/` 内**禁止**出现任何 `godot_cpp/*` 与 `Cesium*` 头文件。用 CMake 静态库边界 + CI 检查（见 §6.4）强制。

### 1.2 三类线程职责

| 线程 | 职责 | 允许触及 |
|---|---|---|
| **主线程**（`_process`） | `updateWithCamera` 遍历、加载队列出队派发、**内容装配**（`GLTFDocument` 或 `ArrayMesh`）、场景同步、LRU 淘汰、`HTTPClient::poll` | Godot API + core 只读 |
| **Worker**（`WorkerThreadPool`） | HTTP 下载、`.subtree`/`b3dm` 字节级解析、GLB 抽取、Morton 展开、包围体数学、（若走路线 B）`ArrayMesh` 构建 | 仅 core（**纯字节/纯数学**） |
| **渲染线程** | Godot 自身 | — |

> ⚠️ 若走 `GLTFDocument` 路线，`append_from_buffer` / `generate_scene` 会创建 `Resource`/`Node`，**必须在主线程**。这直接决定 §4.R3 的主线程预算设计，也是 §4.S1 spike 的核心测量目标。

### 1.3 定标原则：以参考实现为唯一真值

凡涉及调度决策（细化、裁剪、优先级、淘汰时机）的分歧，一律以 `web-spatial-examples/.../threeDTiles` 的行为为准，**不**以 Cesium 官方源码为准。理由：参考实现已经过 6 个 spec 文件的验证，且其行为是本次要对齐的验收标准。

---

## 2. 关键技术决策

### D1. 坐标系与变换链 ✅ **已定：方案 A（保留 Georeference）**

**背景**：参考实现完全不使用 ECEF/地理参考。它定义"渲染世界帧" `R`，并取

```
modelMatrix = inverse(root.transform)          // computeModelMatrix()
⇒ 根瓦片 worldMatrix = modelMatrix × rootTransform = identity
```

即把整个数据集平移/旋转到**数据集原点**。这么做不是审美偏好，而是**精度刚需**：Godot 的 `Vector3`/`real_t` 默认 float32，在 ECEF 量级（6.4e6 m）下分辨率仅约 0.5 m，地形与摄影测量会直接抖散。

**已确认方案**（公式已于 2026-09-17 修正，见下）：

```
R  := Georeference3D 的本地坐标系（LHCS 的 local 端，Z-up ENU）
modelMatrix = localToEcef⁻¹
```

> ⚠️ **更正**：本节原写作 `modelMatrix = localToEcef⁻¹ · inverse(root.transform)`，**是错的**。
> 调度器的 `worldMatrix` 链本身就是从 `rootTransform` 开始逐级累积的（`modelMatrix · rootTransform · t₁ · t₂ · …`），所以再乘一个 `inverse(root.transform)` 会把它抵消掉，等于**丢弃 rootTransform**——对带旋转的 rootTransform（真实数据集都是）会把数据集摆错位置。
>
> 正确形式是**只乘 `localToEcef⁻¹`**，于是：
> ```
> root.worldMatrix = localToEcef⁻¹ · rootTransform
> ```
> 把 georeference 原点设在数据集的 ECEF 位置上，这个式子就退化为**纯旋转**（≈单位阵），瓦片坐标落在原点附近——这正是让 Godot 的 float32 `Transform3D` 保持精度的关键。
>
> **实测验证**（`D:\GISData\3D Tiles\1.0\Photogrammetry`）：其 root transform 的第三列 = `(0.1904, -0.7415, 0.6433)`，而 `normalize(rootTransform.translation)` ≈ `(0.1905, -0.7418, 0.6392)` —— root transform 确实是 ENU→ECEF 帧，吻合到千分位。`demo/node_3d.tscn` 的 georeference 原点已按该 root transform 的平移精确设定。

- 瓦片 BV 与内容一律表达在 **R** 中，直接挂在 `Tileset3D` 节点下；
- 相机由 Godot 世界变换到 R：`camera_R = godotWorldToTileset · camera_godot`（`CameraManager` 现状已具备该能力，**去掉** `localPositionToEcef` 那一步）；
- `region` 包围体的 ECEF OBB 通过 `R⁻¹ = root.transform · localToEcef` 转入 R。

**验证手段（必须做，构成 Phase 1/3 的验收）**：
1. 单测断言 `R` 中根瓦片 `worldMatrix == identity`；
2. 单测断言 region 派生的 AABB 与同瓦片真实 glb accessor AABB 对齐（参考实现 `up-axis.spec.ts` 的做法）；
3. 用 §6.3 的 trace-diff 工具，逐瓦片比对 C++ 与 TS 的 `worldMatrix` 数值。

### D2. glTF / b3dm 内容装配路线 ⏳ **待 spike 决定（§4.S1）**

纠正常见直觉：现有手工装配的**真正独有优势只有一条——跑在 worker 线程**（`populateMeshDataArray` 位于 `prepareInLoadThread`）。而"精确控制顶点布局"这条，本质是在重实现 Godot 导入器已有的能力，并附带 §0.4 的 9 项缺陷。

| 选项 | 内容 | 优势 | 代价 |
|---|---|---|---|
| **A. Godot `GLTFDocument`** | `Ref<GLTFState>` + `append_from_buffer(glbBytes, basePath, state)` + `generate_scene(state)` | 零新增原生依赖；完整 glTF 2.0 语义（材质扩展、sampler wrap、sparse、skinning、morph、动画、Draco、Basis/KTX2）；直接产出 `Node3D`+`MeshInstance3D`，接入 Godot 正常渲染管线（阴影/GI）；**删除约 1000 行装配代码** | **仅主线程**；扩展支持面受引擎版本限制（`EXT_meshopt_compression` 待核实）；错误信息不受控 |
| **B. 自建 `ArrayMesh`** | 自解析 glTF（`cgltf`/`tinygltf`）+ 自建 mesh/材质/贴图 | 可在 worker 线程构建，无主线程压力；完全可控 | 需自实现材质/贴图/UV/sampler；新增原生依赖；延续 §0.4 缺陷；约 1300 行维护成本 |
| **C. 混合** | 常见摄影测量 b3dm（POSITION/NORMAL/TEXCOORD_0/COLOR_0 + u16/u32 index）走自建 worker 快路径；含扩展的走 `GLTFDocument` | 兼顾吞吐与覆盖率 | **两套代码都要维护**，长期成本最高 |

**当前倾向**：A。**但必须由 spike 数据裁决**（详见 §4.S1 的 8 项指标与 5 条定量门槛）。若门槛 1/3 失败则改选 B；若仅门槛 4 失败则选 A + 强制每帧预算队列。

### D3. 第三方依赖 ✅ **已定**

| 用途 | 选定 | 理由 |
|---|---|---|
| 线性代数 | **`glm`**（header-only，pin tag） | 需 **double 精度**（`glm::dvec3`/`dmat4`）；参考实现即 double 语义 |
| JSON | **`nlohmann/json`**（header-only，pin tag） | core 层需在 worker 线程解析 `tileset.json` / `.subtree` JSON 段 / 外部 tileset，不能依赖 Godot 的 `JSON` 类（主线程 + Godot API） |
| 单测 | **`doctest`**（header-only，仅测试目标链接） | 编译极快，比 Catch2 更轻，足矣 |
| gzip 解压 | **Godot `PackedByteArray::decompress(GZIP)`** | 替代原 `GunzipAssetAccessor`，零新增依赖 |
| 线程池 | **Godot `WorkerThreadPool`** | 替代自研 `ThreadUtils.hpp` 与 `GodotTaskProcessor`，且随引擎正确关停 |

获取方式：优先 `FetchContent`；若目标环境网络受限，改为 vendor 到 `extern/third_party/`（三库源码合计约 3 MB）。

### D4. 命名 ✅ **已定：彻底去 Cesium 命名**

类名、属性名、命名空间全部 Cesium-free，并同步迁移 `demo/node_3d.tscn`。完整映射表见 **§7**。

### D5. 功能范围 ✅ **已定**

**本轮纳入**（全量对齐参考实现）：

- [x] 显式瓦片树；隐式瓦片（`3DTILES_implicit_tiling` / 1.1 core `implicitTiling` + `.subtree` 二进制）；外部 tileset 嵌套
- [x] 内容格式 `b3dm` / 二进制 `glb` / JSON `glTF`（含 `RTC_CENTER`、BatchTable 读取）
- [x] Cesium 风格调度参数：SSE、foveation、`cullRequestsWhileMoving`、`dynamicScreenSpaceError`、`skipLevelOfDetail`、`preferLeaves`、`cacheBytes`/`maxCachedTiles`
- [x] LRU 缓存、出视锥请求取消、内存账务、`TilesetStats` 诊断、`onTileLoad`/`onProgress`/`onError` 回调
- [x] 调试可视：包围体线框、GE/统计/内存/URL 标签、染色、冻结帧

**本轮不做**（明确排除，避免范围蔓延）：`pnts` 点云、`i3dm` 实例化、`cmpt` 复合、样式引擎、全局跨 tileset 请求优先级调度（参考实现本身也未实现）、栅格 overlay、地形（`QuantizedMeshTerrain`）。

---

## 3. 模块映射表（TS → C++）

| 参考实现（TS） | 目标（C++） | 精度要求 | 备注 |
|---|---|---|---|
| `mat4Mul` / `mat4Inverse` / `identityMatrix4` / `getMatrixMaxScale` | `core/math/Mat4.{h,cpp}` | double | 用 `glm::dmat4` |
| `computeScreenSpaceError` / `computeSurfaceDistance` / `computeBvSurfaceDistance` / `fog` | `core/math/ScreenSpaceError.{h,cpp}` | double | |
| `wgs84ToCartesian` / `wgs84SurfaceNormal` / `regionToEcefObb` / `convertRegionBoundingVolumes` | `core/math/GeoMath.{h,cpp}` + `core/math/BoundingVolume.{h,cpp}` | double | `regionToEcefObb` 精确复刻 `OrientedBoundingBox.fromRectangle`（≤π 用中心 ENU 切平面，>π 用赤道平面） |
| `subdivideBox` / `getBoundingVolumeCenter` / `getBoundingVolumeRadius` | 同上 | double | |
| `Tile`（class） | `core/tiles/Tile.{h,cpp}` | double | 含内容状态机 `UNLOADED/LOADING/PROCESSING/READY/FAILED` |
| `parseTilesetJson` / `parseTile` | `core/tiles/TilesetJson.{h,cpp}` | — | |
| `Tileset`（traverse/traverseAll/traverseSkipLevels/foveation/dynamicSSE） | `core/tiles/Tileset.{h,cpp}` | double | **核心，~1400 行** |
| `TilesetCache` + `DoublyLinkedList` | `core/tiles/TilesetCache.{h,cpp}` + `DoublyLinkedList.{h,cpp}` | — | 直译 |
| `defaultTilesetOptions` / `TilesetOptions` / `TilesetStats` | `core/tiles/TilesetOptions.h` / `TilesetStats.h` | — | 用 `std::optional` 表达可选字段 |
| `consts.ts` | `core/tiles/Consts.h` | — | `Axis`/`LogLevel`/并发上限/重试/节流间隔 |
| `BitStream` / `SubtreeReader` / `ImplicitTileManager` | `core/implicit/*.{h,cpp}` | — | |
| `B3dmParser` | `core/content/B3dmParser.{h,cpp}` | — | 8 字节对齐提取内嵌 GLB |
| （无）内容类型分派 | `core/content/ContentType.{h,cpp}` | — | 幻数：`b3dm`/`glTF`/JSON |
| `fetch` + `AbortController` | `core/net/IAssetAccessor.h` + `godot/AssetAccessor.{h,cpp}` | — | 抽象放 core，实现在 godot 层 |
| `loadQueue` / `activeLoads` / `activeTiles` / `enqueueLoads` / `processLoadQueue` | `core/net/RequestScheduler.{h,cpp}` | — | 后续可升级为全局跨实例调度 |
| `getGltfLoader`（GLTFLoader+DRACO+KTX2） | `godot/ContentFactory.{h,cpp}` | — | 路线见 §2.D2 / §4.S1 |
| `applyModelAxisCorrection` | `core/math/Mat4.h` 提供旋转矩阵 + `ContentFactory` 应用 | double | 上轴校正**只作用于内容**，不作用于 BV |
| `applyPBRMaterials` / `ensurePBRMaterial` | **删除**（路线 A 下由 `GLTFDocument` 直接产出材质） | — | |
| `tileObjectMap` / `scene.add` / `visible` / `matrix` 同步 | `godot/SceneSync.{h,cpp}` | — | 常驻 + 只切 `visible` |
| `unloadTile` / `disposeObject3D` | `godot/SceneSync.cpp` | — | **必须复位** `contentReady=false; contentData=null` |
| `DebugTilesHelpers`（LineSegments + Sprite + canvas 标签） | `godot/DebugTilesHelpers.{h,cpp}` | — | 改用 `ImmediateMesh`/`MeshInstance3D`；标签用 `Label3D` |
| `use3DTilesRenderer.ts`（业务封装） | `godot/Tileset3D.{h,cpp}` 的 Node API | — | |
| `__tests__/*.spec.ts`（6 个） | `tests/*.cpp`（doctest） | — | 见 §6 |

---

## 4. 风险登记册

| ID | 风险 | 影响 | 缓解 |
|---|---|---|---|
| **R1** | **坐标帧/变换链理解偏差** → 几何与包围盒错位、90° 翻转、SSE 失真 | 致命（视觉全错） | D1 方案 + 三条验证断言；先做 §6.3 trace-diff；提供"三板斧"排查序：① region 是否已转 R；② 内容上轴校正是否加；③ `RTC_CENTER` 是否被误乘进矩阵（参考实现明确：`RTC_CENTER` **不**随上轴旋转） |
| **R2** | `GLTFDocument` 对 3D Tiles 常见扩展支持不全（`EXT_meshopt_compression`、部分 KHR 材质扩展） | 部分数据集加载失败或外观异常 | §4.S1 spike 先行验证；准备自建 `ArrayMesh` fallback 通道（§2.D2 选项 B）；对不支持的扩展记录明确日志而非静默失败 |
| **R3** | 内容装配必须主线程 → 大批瓦片同帧到达时**主线程卡顿** | 体验劣化 | `FrameBudgetQueue`：每帧处理预算（`Time::get_ticks_usec` 计时，沿用 `main_thread_loading_time_limit` 语义）+ 每帧上限 + `maximum_simultaneous_tile_loads` 限流；worker 线程完成字节级预解析（b3dm 头/FT/BT、GLB 段抽取）后再交主线程，主线程只做最后一步 |
| **R4** | §0.4 #7：`free()` 中 `tiles_destroyed` 早退泄漏 + `delete` 与 `queue_free()` 混用的**双重释放** | 崩溃/内存泄漏 | 新实现中单一释放路径：`GodotGltfNode` 只经 `Node::queue_free()` 释放场景节点，包装结构用 `std::unique_ptr` 持有于瓦片，**绝不**混用 `delete` |
| **R5** | §0.4 #8：`GodotAssetAccessor::get` busy-poll + `resolve` 后未 `return`（真实缺陷） | 请求异常/重复回调 | 整体重写为：主线程每帧 `poll()` 一次（非阻塞）+ 显式状态机；`resolve`/`reject` 二选一且立即 `return` |
| **R6** | core 层误引入 Godot API 导致 worker 线程崩溃 | 随机崩溃 | CMake 目标隔离 + CI grep 门禁（§6.4） |
| **R7** | `.subtree` 缓存与失败退避缺失 → 反复请求坏子树 | 带宽浪费 | 移植 `MAX_SUBTREE_LOAD_ERRORS`（默认 3）与 `failedSubtrees` 计数 |
| **R8** | 内存账务口径：Godot 侧拿不到真实 GPU 占用 | 淘汰策略失真 | 按 **CPU 端字节**（下载字节 + 顶点/索引/贴图解码后字节）估算，并在 `TilesetStats` 中明确标注为估算；不做虚假精度承诺 |
| **R9** | §0.4 #9：`create_convex_collision()` 每 primitive 调用 | 加载期开销极大 | 新实现**默认关闭**物理碰撞，改为显式开关 `create_physics_meshes = false` |
| **R10** | 依赖获取（FetchContent 需联网） | 构建受阻 | 优先 FetchContent；受限则 vendor 到 `extern/third_party/`（§2.D3） |
| **R11** | 彻底改名（D4）破坏 `node_3d.tscn` 与用户已有工程 | 场景加载失败 | 一次性迁移 `demo/node_3d.tscn`；在 §7 给出旧→新完整映射表与迁移命令；`CHANGELOG.md` 记为 breaking change |

---

## 5. Spike S1：内容装配路线裁决（阻塞 Phase 4）

> 目标：用**可比的定量数据**决定 §2.D2 走 A / B / C，而不是凭感觉。预计 ~300 行一次性代码，产出后即可删除。

### S1.1 输入 fixture（3 类，覆盖典型面）

| # | fixture | 覆盖点 |
|---|---|---|
| F1 | 摄影测量 b3dm（取 `demo` 数据集一个中层级瓦片） | POSITION / NORMAL / TEXCOORD_0，u16 或 u32 index，无材质扩展 |
| F2 | 含 `KHR_draco_mesh_compression` 与/或 `KHR_texture_basisu`（KTX2）的瓦片 | 压缩几何 / GPU 压缩贴图 |
| F3 | 纯 `.glb`（非 b3dm 包裹，tileset 1.1 常见） | 非包裹路径 |

F2 若现有数据集不含，从 3D Tiles 官方样例取一个最小瓦片入 `tests/data/`。

### S1.2 对比分支

| 分支 | 实现 | 前置 |
|---|---|---|
| **基线 B** | 现有 cesium-native 构建的 `GodotPrepareRendererResources` 路径 | 依赖 **Phase 0.2** 的旧版构建成功 |
| **候选 A** | 新 `ContentFactory`，走 `GLTFDocument` | 独立小扩展或直接在 `dev` 上临时接线 |

> 若 Phase 0.2 失效（cesium-native 构建受阻），则基线 B 改为**离线导出**：用 CesiumJS / `gltf-transform` 读出 fixture 的规范指标（顶点数、index 数、AABB、材质参数、贴图格式）作为参照值。这样即使拿不到旧二进制，S1 仍可执行。

### S1.3 导出指标（两分支输出**同格式 CSV**）

`surface_count` · `vertex_count` · `index_count` · `aabb_min[3]` · `aabb_max[3]` · `material_params`（albedo/metallic/roughness/unlit）· `texture_formats`（`Image::Format` 列表）· `node_tree`（节点路径）· `load_thread_ms` · `main_thread_ms` · `peak_bytes`

### S1.4 通过门槛（**全部满足才选 A**）

| # | 门槛 | 理由 |
|---|---|---|
| 1 | `aabb_min/max` 与基线一致（容差 1e-3 m） | **否决任何坐标系/上轴错误**，是最硬的一条 |
| 2 | `vertex_count` 与基线差异 ≤ 5% | 允许索引化/顶点复制策略不同 |
| 3 | F1/F2/F3 三类 fixture 均正确出图，Draco 与 KTX2 正常 | 覆盖率底线 |
| 4 | `main_thread_ms` ≤ **8 ms/瓦片** | 1000 瓦片累计 ≈ 8 s，可被每帧预算摊平；超出则必须依赖 R3 预算队列 |
| 5 | 无 `Image` 格式被强制解码为 `RGBA8` | 否则显存占用翻约 4 倍 |

### S1.5 裁决规则

| 失败门槛 | 结论 |
|---|---|
| 1 或 3 | **改选 B**（自建 `ArrayMesh`），接受约 1300 行装配代码的维护成本 |
| 2（仅此一条） | 选 A，但需人工核对顶点差异来源（索引化策略），确认无视觉差 |
| 4（其余通过） | **选 A + 强制 R3 预算队列**，并把 `maximum_simultaneous_tile_loads` 下调 |
| 5（其余通过） | 选 A + 在 worker 线程自行完成 KTX2→BC7/ASTC 转码后再交 `GLTFDocument`（需先核实其能否吞已转码数据）；不可行则选 B |
| 保守优先 | 若 A 与 B 指标接近且团队更看重确定性，选 **C 混合**（摄影测量快路径 + 扩展回退），但须接受双份维护 |

---

## 6. 测试与验证策略

### 6.1 单元测试（core 层，doctest）

移植参考实现的 6 个 spec 文件，作为**行为等价性契约**：

| 原 spec | 覆盖 | 移植要点 |
|---|---|---|
| `math.spec.ts` | SSE / 表面距离 / fog / region OBB 数学 | 直译，double 断言 |
| `parsers.spec.ts` | b3dm 头 / subtree 位流 / tileset 解析 | 需内联二进制 fixture（`tests/data/`） |
| `scheduling.spec.ts` | REPLACE 回退、cullWhileMoving、cullWithChildrenBounds、无条件细化 | **需要可注入的 fake asset accessor**（见 6.2） |
| `scheduler-fixes.spec.ts` | LRU 淘汰复位、preferLeaves、离屏子级、出视锥取消 | 同上 |
| `srs-bounding.spec.ts` | region 转换不变式、sphere 线框 | 直译 |
| `up-axis.spec.ts` | **真实数据** glb accessor AABB 与 box 对齐断言、RTC 不受旋转影响 | 需 vendored 小样本 fixture（原测试用 Photogrammetry c0 瓦片，可裁剪至数 KB） |

### 6.2 关键测试基建：可注入的资产访问器

参考实现靠 `fetch` 打桩 + vitest 假定时器实现确定性。C++ 侧等价物：

```cpp
class FakeAssetAccessor : public core::IAssetAccessor {
  // 预置 url → bytes 映射
  // 支持"手动完成"模式，让测试显式推进 LOADING → PROCESSING → READY 状态机
  // 支持注入失败/超时，验证重试与永久失败标记
};
```

**这是让调度测试可写的前提**，必须在 Phase 3 之前完成。

### 6.3 关键验证工具：调度 trace-diff

新增 headless 工具 `tools/sched_trace`：

- **输入**：`tileset.json` 路径 + 一串相机位姿 + 视口高度
- **输出**：CSV，每帧每瓦片一行：
  `frame, tileId, depth, sse, geometricError, worldMatrix[16], decision(render|load|refine|culled|deferred|evicted)`
- **用法**：① 在 TS 侧 `Tileset3DRenderer` 加一个导出钩子输出同格式 CSV；② **逐帧 diff 两份 CSV**

> 这把"我的移植行为是否一致"从"靠肉眼看画面"变成**可 diff 的确定性产物**，是本计划性价比最高的一项投入。建议在 **Phase 3 结束前**建成，之后每个 Phase 都跑。

### 6.4 架构门禁（CI 检查）

```bash
# 1. core 层不得依赖 Godot / Cesium
rg -n '#include\s*[<"](godot_cpp|Cesium)' src/core/ && exit 1
# 2. 全仓库不得残留 Cesium 头文件与 Cesium 命名类
rg -n '#include\s*[<"]Cesium' src/ && exit 1
rg -n 'class\s+Cesium|CesiumForGodot' src/ && exit 1
# 3. cesium-native 不得出现在构建脚本
rg -n 'cesium-native' --glob '!docs/**' . && exit 1
```

### 6.5 端到端验收（对齐参考实现）

用 `demo/node_3d.tscn` 指向同一数据集（现有：`E:/GISData/3DTiles/3DTiles-Photo-grammetry/tileset.json`），与 Web 版 demo **并排比对**：

| 验收项 | 判据 |
|---|---|
| LOD 一致性 | 同一相机位姿下渲染瓦片级别一致（允许 ±1 级） |
| 无空洞/裂缝 | 快速推拉相机过程中不出现持续空洞（REPLACE 父级回退生效） |
| 朝向正确 | 无 90° 翻转、无镜像；包围体线框与几何同框 |
| 贴图正确 | 无错位、无颠倒、无 sRGB 错误；wrap 模式生效 |
| 统计面板 | `TilesetStats` 各字段与参考实现量级一致（`loadedTiles` 为活值、`totalTiles` 为累计值） |
| 稳定性 | 长时间绕行相机：内存不无界增长；LRU 淘汰有日志；瓦片不反复重下 |
| 加载完成 | 加载进度收敛到 100% |
| 主线程 | 加载期无 >100 ms 的长帧（`FrameBudgetQueue` 生效的证据） |

---

## 7. 命名迁移清单（D4 落地）

### 7.1 类型

| 现名 | 新名 | 类型 / 说明 |
|---|---|---|
| namespace `CesiumForGodot` | `tiles3d` | 命名空间 |
| `Cesium` | `Godot3DTiles` | `Object`（版本信息，改 `ClassDB` 注册名） |
| `CesiumGeoreference` | `Georeference3D` | `Node3D` |
| `Cesium3DTileset` | `Tileset3D` | `Node3D` |
| `LongitudeLatitudeHeight` | **保留** | `Resource`（术语本身与 Cesium 无关） |
| `EarthCenteredEarthFixed` | **保留** | `Resource`（同上，标准大地测量术语） |
| `CesiumGltfNode` | `TileContentNodes` | struct |
| `CesiumPrimitiveInfo` | `PrimitiveInfo` | struct |
| `GodotAssetAccessor` | `AssetAccessor` | 类 |
| `GodotPrepareRendererResources` | **删除** → `ContentFactory` | 职责转移 |
| `GodotTilesetExternals` | **删除** | — |
| `GodotTaskProcessor` | **删除** → `WorkerThreadPool` | — |
| `ThreadUtils.hpp` | **删除** → `WorkerThreadPool` | — |
| `CesiumTileExcluder` / `CesiumTilesetExcluder` | **删除** | 原为空 TODO 实现 |
| `CesiumEllipsoid` | **删除** → `core/math/GeoMath` | — |
| `CameraManager` / `FileHelper` | 保留 | 已 Cesium-free |

> `Godot*` 前缀一并去掉：整个项目就是 Godot 插件，前缀是冗余；重名风险由 `tiles3d::` 命名空间隔离。

### 7.2 属性与信号（Godot 侧可见）

顺带把带空格的属性名统一为 Godot 惯用的 snake_case：

| 现属性名 | 新属性名 |
|---|---|
| `url` | `url` |
| `maximum screenspace error` | `maximum_screen_space_error` |
| `preload ancestors` | `preload_ancestors` |
| `preload siblings` | `preload_siblings` |
| `forbid holes` | `forbid_holes` |
| `maximum simultaneous tile loads` | `maximum_simultaneous_tile_loads` |
| `maximum cached MB` | `maximum_cached_mbytes` |
| `loading descendant limit` | `loading_descendant_limit` |
| `enable frustum culling` | `enable_frustum_culling` |
| `enable fog culling` | `enable_fog_culling` |
| `enforce culled screen space error` | `enforce_culled_screen_space_error` |
| `culled screen space error` | `culled_screen_space_error` |
| `suspend update` | `suspend_update` |
| `create physics meshes` | `create_physics_meshes` |
| `generate smooth normals` | `generate_smooth_normals` |
| `log selection stats` | `log_selection_stats` |
| `originAuthority` | `origin_authority` |
| `scale` | `scale` |

信号：`lngLatH_changed` → `geodetic_changed`；`ecef_changed`、`on_tileset_loaded` 保持不变。

### 7.3 `demo/node_3d.tscn` 迁移

```diff
- [node name="CesiumGeoreference" type="CesiumGeoreference" parent="."]
- originAuthority = SubResource("EarthCenteredEarthFixed_7u6rm")
+ [node name="Georeference3D" type="Georeference3D" parent="."]
+ origin_authority = SubResource("EarthCenteredEarthFixed_7u6rm")

- [node name="Cesium3DTileset" type="Cesium3DTileset" parent="CesiumGeoreference"]
+ [node name="Tileset3D" type="Tileset3D" parent="Georeference3D"]
```

`CHANGELOG.md` 必须记录为 **breaking change**。

---

## 8. 实施阶段

> 每阶段结束必须：编译通过（`/W4 /WX`）+ 相关单测通过 + 跑一遍 trace-diff（Phase 3 之后）+ 记一次 workspace memory。

### Phase 0 — 恢复可构建基线 & 移除依赖

1. `git submodule update --init extern/godot-cpp`（**仅 godot-cpp**）
2. **基线快照（已确认尝试）**：`git submodule update --init extern/cesium-native` 并构建一次，产出**可运行的旧版二进制**，同时作为 §5.S1 的基线分支 B。
   - 若构建受阻（依赖多、耗时长）→ **放弃 0.2**，改以 Web 版 demo（+ §5.S1.2 的离线导出方案）作为对照真值，并在本文档记录该决定。
3. 移除 cesium-native：`.gitmodules` 条目、`extern/CMakeLists.txt` 中 `add_subdirectory(cesium-native)` 与 `cesium-native-wrapper`、删除 `extern/cesium-native` 目录
4. 新增依赖：`glm` / `nlohmann_json` / `doctest`，集中放在 `extern/third_party/CMakeLists.txt`
5. 清理仓库根脏目录 `CMakeFiles/`，补 `.gitignore`
6. `src/CMakeLists.txt` 拆分为 `core`（静态库，`EXCLUDE_FROM_ALL` 之外独立可测）+ `godot-3dtiles`（共享库，链 `core`）
7. **执行 §7 命名迁移**（此时做，后续新代码直接用最终名，避免二次清扫）

**交付物**：能构建出一个结构正确但调度为空的扩展；`core` 静态库建立。

### Phase 0.5 — Spike S1（阻塞 Phase 4）

按 §5 执行内容装配路线裁决，输出指标 CSV 与结论，更新 §2.D2 状态为已定。

**交付物**：S1 报告（指标表 + 门槛判定 + 最终路线）。**spike 代码一次性，结论落定后删除。**

### Phase 1 — core 数学与包围体

- 实现 §3 中 `core/math/*` 全部内容
- 单测：移植 `math.spec.ts` + `srs-bounding.spec.ts`

**交付物**：数学层单测全绿。**验证**：region→OBB 变换不变式（宽 ≤π 与 >π 两条分支）。

### Phase 2 — 瓦片树与解析器

- `core/tiles/Tile`、`TilesetJson`、`core/implicit/*`、`core/content/B3dmParser`
- 单测：移植 `parsers.spec.ts`

**交付物**：能解析真实 `tileset.json` 并打印树结构；能解析 `.subtree` 并展开隐式瓦片。

### Phase 3 — 调度器核心

- `core/tiles/Tileset` + `TilesetCache` + `core/net/RequestScheduler`
- `FakeAssetAccessor` 测试基建
- **建成 `tools/sched_trace` + TS 侧同格式导出**

**交付物**：单测移植 `scheduling.spec.ts` + `scheduler-fixes.spec.ts` 全绿；trace-diff 在 3 组相机轨迹上**逐行一致**。

### Phase 4 — Godot 集成骨架

- `godot/AssetAccessor`（重写：每帧 `poll` + gzip 解压 + `FileAccess`，修 §0.4 #8）
- `godot/CameraManager`（去 ECEF，输出 `core::ViewState`）
- `godot/Tileset3D`（桥接 `core::Tileset`，逐帧驱动）
- `godot/ContentFactory`（按 S1 结论落地）
- `godot/SceneSync` + `FrameBudgetQueue`（含 R4 单一释放路径）

**交付物**：demo 场景能加载并渲染真实数据集，`debug_show_bounding_volume` 线框与几何同框。

### Phase 5 — 调试可视、统计与打磨

- `DebugTilesHelpers`（线框 / 标签 / 染色 / 冻结帧）
- `TilesetStats` 面板 + `onTileLoad`/`onProgress`/`onError` 信号
- 上轴校正、`RTC_CENTER`、物理碰撞开关（`create_physics_meshes` 默认 `false`）
- §6.5 端到端全部 8 项验收

### Phase 6 — 清理与文档

- 删除 `GodotPrepareRendererResources.*`、`GodotTilesetExternals.*`、`GodotTaskProcessor.*`、`ThreadUtils.hpp`、`CesiumTileExcluder.h`、`CesiumTilesetExcluder.cpp`、`CesiumEllipsoid.*`
- 跑 `clang-format`（CI 用 v15）；`/W4 /WX` 全绿
- 更新 `README.md`（重写 Status 一节）、`CHANGELOG.md`（记录 breaking change）

---

## 9. 完成定义（Definition of Done）

1. `extern/cesium-native` 目录、`.gitmodules` 条目、所有 `#include <Cesium*>` **全部消失**，且 `src/` 内无 `class Cesium*` / `CesiumForGodot`（§6.4 三个门禁全通过）
2. `src/core/` 可独立编译为静态库，**不含任何 Godot / Cesium 头文件**，单测全绿（6 个 spec 全量移植）
3. 原生依赖仅剩：`godot-cpp` + `glm` + `nlohmann/json`（+ 仅测试用的 `doctest`）；`ThreadUtils.hpp` 自研线程池已被 `WorkerThreadPool` 取代
4. `tools/sched_trace` 与 TS 参考实现 trace-diff **逐行一致**（3 组相机轨迹）
5. `demo/node_3d.tscn` 已按 §7.3 迁移，加载并渲染真实数据集正常
6. §6.5 端到端验收 **8 项全部通过**
7. §0.4 的 3 个 P0 缺陷（#1 判空、#7 双重释放、#8 resolve 后未 return）**全部消除**并有测试覆盖
8. `README.md` / `CHANGELOG.md` 更新；`CHANGELOG.md` 标注 breaking change

---

## 10. 决策记录

| # | 决策点 | 结论 | 日期 |
|---|---|---|---|
| D-1 | 坐标系方案 | **方案 A**：保留地理参考节点，`modelMatrix = localToEcef⁻¹ · inverse(root.transform)` | 2026-09-16 |
| D-2 | glTF 内容装配路线 | **已定为 Godot `GLTFDocument`**（§16.2）。Spike S1 由「裁决」降级为「验证」 | 2026-09-17 |
| D-3 | 第三方依赖（glm / nlohmann_json / doctest / WorkerThreadPool） | **采纳** | 2026-09-16 |
| D-4 | 类名与 API | **彻底去 Cesium 命名**，同步迁移 `node_3d.tscn`（§7） | 2026-09-16 |
| D-5 | 功能范围 | **按 §2.D5**，不做 pnts/i3dm/cmpt/样式引擎/全局优先级调度 | 2026-09-16 |
| D-6 | Phase 0.2 视觉基线 | **先试**，超时即放弃并改用 Web demo + 离线导出作真值 | 2026-09-16 |
| D-7 | 目标引擎版本 | **Godot 4.7.2**（本机 `D:\Applications\Godot_v4.7.2-stable_win64`），不再兼容 4.3 | 2026-09-16 |
| D-8 | godot-cpp 版本 | **`10.0.0-stable`**（commit `507ed9d`）。v10 起独立版本号，分支只到 `4.5`，4.6/4.7 靠 `GODOTCPP_API_VERSION` 选 API | 2026-09-16 |
| D-9 | 构建预设 | **单配置 Ninja**（放弃 `Ninja Multi-Config`），并新增 `GODOTCPP_TARGET=editor` 预设 | 2026-09-16 |
| D-10 | Hot reload | 默认 **OFF**（`reloadable = false`）；旧变量 `GODOT_ENABLE_HOT_RELOAD` 在 v10 中已失效，属修复而非回退 | 2026-09-16 |
| D-11 | `src/core/` 是否使用 C++ 异常 | **完全不用**。前置条件违反用 `assert` + 定义明确的兜底返回 | 2026-09-17 |
| D-12 | 单测框架用法 | 只用 doctest 支持的断言；**`doctest::Approx` 没有 `margin()`**（那是 Catch2 的），近零比较一律写显式绝对差 | 2026-09-17 |
| D-13 | glTF 内容装配路线（**反转 D-2**） | **改选自建装配**（原 §2.D2 选项 B）：`GltfReader`（core）+ 手工组装 `MeshInstance3D`/`ArrayMesh`/`StandardMaterial3D`，`GLTFDocument` 整条路径删除（§17.4） | 2026-09-17 |

**D-11 的理由**：Godot 自身以禁用 C++ 异常的方式构建；godot-cpp 的默认 `GODOTCPP_DISABLE_EXCEPTIONS=ON` 会给消费者加 `_HAS_EXCEPTIONS=0`。在这条链接链上的库靠 `throw` 表达前置条件是隐患——异常穿过不启用异常编译的代码是 UB 级风险。而这些前置条件违反（非 box 做细分、level 为负）本质是**调用方编程错误**，`assert` 才是对的工具。附带收益：doctest 的 `CHECK_THROWS_*` 不再需要，`/EHsc` 在不在都无所谓，测试从此不依赖 MSVC 异常开关。

**D-12 的理由**：首次编译时 `test_geomath.cpp` 报 `C2039: "margin": 不是 "doctest::Approx" 的成员`。`margin()` 是 Catch2 的 API，doctest 只有相对 `epsilon()`——它只能处理"相对某个非零参考值"的比较，对"约等于 0"用不了，必须写成 `std::abs(x) < tol`。

---

## 11. 目标引擎与工具链（Godot 4.7.2 / godot-cpp v10）

> 本节记录 Phase 0 实测得到的环境事实与版本决策。**所有内容均为本机实测**，非推断。

### 11.1 版本对应关系（已用 `git ls-remote` 核实）

| 项 | 事实 |
|---|---|
| 目标引擎 | **Godot 4.7.2**，`D:\Applications\Godot_v4.7.2-stable_win64\Godot_v4.7.2-stable_win64.exe`（标准版，非 .NET） |
| 目标绑定 | **godot-cpp `10.0.0-stable`**，commit `507ed9d840c01a3c5b2a39af8bb4000bfac30bf5`，2026-09-15 发布 |
| 仓库当前 gitlink | `fbbf9ec4…` = **`godot-4.3-stable`（2024-08）** → 与引擎相差 4 个大版本，**必须更新** |
| godot-cpp 4.x 分支 | 只到 `4.5`（`4.0`–`4.5`）。**不存在 `4.6` / `4.7` 分支** |
| 4.6/4.7 支持方式 | v10 起 godot-cpp 独立版本号，通过 `GODOTCPP_API_VERSION`（或 `GODOTCPP_CUSTOM_API_FILE`）选择 `gdextension/extension_api-<dashed>.json` |

**含义**：本仓库 `.gitmodules` 中记录的 `branch` 与 gitlink 必须改到 `10.0.0-stable`。仅靠 `git submodule update`（不带 `--remote`）**不会**升级，因为子模块检出的是索引里记录的 gitlink。

### 11.2 godot-cpp v10 的 CMake 选项**全部改名**（已读源码核实）

原 `extern/CMakeLists.txt` 使用的名字在 v10 中**已不存在**，属于静默失效：

| 项目原本使用的名字 | v10 的正确名字 | 默认值 | 说明 |
|---|---|---|---|
| `GODOT_ENABLE_HOT_RELOAD` | **`GODOTCPP_USE_HOT_RELOAD`** | `""`（关） | 原变量无效，旧构建实际是靠手写 `HOT_RELOAD_ENABLED` 宏打开的 |
| `GODOT_CPP_SYSTEM_HEADERS` | **`GODOTCPP_SYSTEM_HEADERS`** | `OFF` | 原变量无效，即 godot-cpp 头文件**从未**被当作 SYSTEM 处理 → 其告警会撞上 `/WX` |
| —— | `GODOTCPP_API_VERSION` | `""` | **必须非空**，否则 `FATAL_ERROR: 'GODOTCPP_API_VERSION' must be provided` |
| —— | `GODOTCPP_TARGET` | `template_debug` | 合法值：`template_debug` / `template_release` / `editor` |
| —— | `GODOTCPP_CUSTOM_API_FILE` | `""` | 优先级高于前两者 |
| —— | `GODOTCPP_ENABLE_TESTING` | `OFF` | godot-cpp 自带集成测试 |
| —— | `GODOTCPP_WARNING_AS_ERROR` | `OFF` | |
| —— | `GODOTCPP_SYMBOL_VISIBILITY` | `hidden` | 与主库的 `CXX_VISIBILITY_PRESET hidden` 一致，故删除了原来手工 `set_target_properties` |
| —— | `GODOTCPP_PRECISION` | `single` | 与官方引擎一致；**调度器内核仍用 double，不受此项影响** |

另注：v10 内 `add_library(godot-cpp STATIC)` 无条件创建，并额外提供 `godot::cpp` 别名。

### 11.3 为什么预设改为**单配置 Ninja**（D-9）

两条硬约束共同排除了 `Ninja Multi-Config`：

1. `GODOTCPP_TARGET` 是**单值**开关——一次 configure 只能产出一种绑定（debug / release / editor）。多配置构建目录无法同时满足 Debug 与 Release。
2. `templates/CMakeLists.txt` 第 14 行硬性要求 `CMAKE_BUILD_TYPE ∈ {Debug, Release}`，否则 `FATAL_ERROR`。而 `Ninja Multi-Config` 下 `CMAKE_BUILD_TYPE` 为空 → 原 `windows` 预设**必然配置失败**（这是一个既存缺陷）。

因此预设改为 `windows-editor` / `windows-release`（每平台两个），`binaryDir` 分离为 `build/<preset>`。

**为什么必须保留 `editor` 目标**：`CameraManager` 需要读取编辑器视口相机，用的是 `EditorInterface::get_editor_viewport_3d()`；该 API 只在 `TOOLS_ENABLED` 下存在，即 **`GODOTCPP_TARGET=editor`**。用 `template_debug` 构建时编辑器相机路径会拿不到（这既是既有代码的真实依赖，也是 Phase 4 必须保留的能力）。

### 11.4 本机工具链现状（实测）

**2026-09-16 初次实测**：没有任何 C++ 编译器，构建链完全不可用。

**2026-09-17 更新**：工具链已装齐。

| 类别 | 结果 |
|---|---|
| CMake | ✅ `3.31.12`（`D:\Applications\cmake-3.31.12-windows-x86_64\bin`） |
| Git | ✅ `2.54.0.windows.1` |
| Python | ✅ `3.13.15`（`D:\Program Files\Python`，godot-cpp 绑定生成器需要） |
| Godot | ✅ `4.7.2 stable` |
| Ninja | ✅ `1.13.2`（`D:\ninja-win`，已在 PATH） |
| Visual Studio | ✅ **Visual Studio Community 2026**（v18.10.1），装在 `D:\Program Files\Microsoft Visual Studio\18\Community` |
| MSVC 工具集 | ✅ `14.51.36231`，`cl.exe` 版本 `19.51.36257`（`VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64`） |
| Windows SDK | ✅ `10.0.26100.0`，装在**非默认路径** `D:\Windows Kits\10`；`rc.exe` / `mt.exe` 均在 `bin\10.0.26100.0\x64\` |
| `vswhere` | ✅ 存在，且 `-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64` 能正确返回安装路径（即 `scripts\msvc_env.bat` 可用） |

**已验证可行的静态检查**：`cmake --list-presets` 通过（预设 JSON 结构有效）；`cmake --list-presets=build` 通过。

**两个已排除的首次 configure 风险**：
- `cmake/ClangFormat.cmake` 由 `if (CLANG_FORMAT_PROGRAM)` 守护，无 clang-format 时静默跳过，不会 `FATAL_ERROR`
- `cmake/GitVersionInfo.cmake` 走 `git describe --tags --always`，本仓库**没有 tag** 时会回退到 commit 短哈希，不会失败（唯一的 `FATAL_ERROR` 在源码中是注释掉的）

### 11.5 工具链方案（含对「Ninja 能否替代 MSVC」的澄清）

**先澄清概念**：Ninja **不是编译器**，它是构建执行器（build executor）。两者在不同层：

```
CMake (生成)  →  Ninja 或 MSBuild (调度/执行)  →  cl.exe / g++ / clang++ (真正编译)
```

- `Ninja + cl.exe`：常规组合，且 **Ninja 驱动 cl.exe 通常比 MSBuild 快**（MSBuild 有大量 .NET 层与项目求值开销）。本仓库 `CMakePresets.json` 原本就是 `Ninja` + `cl.exe`，这个方向是对的。
- 但 **Ninja 换不掉 MSVC**：真正编译代码的仍是 `cl.exe`。当前问题不是"构建系统慢"，而是**一个编译器都没有**。
- 换编译器（如改用 `g++`）才谈得上"编译效率差异"：`cl.exe` 与 `clang-cl` 编译速度接近（clang 前端通常更快），`g++` 在重模板场景偏慢且调试信息体验较差。

| 方案 | 需安装 | 体积 | 与 Godot 4.7.2 官方二进制的 ABI | 评价 |
|---|---|---|---|---|
| **A. VS Build Tools 2022（推荐）** | 仅 "使用 C++ 的桌面开发" 工作负载 + Windows SDK（**不必装 IDE**） | ≈ 2.5–4 GB | **完全一致**（Godot 官方 win64 版即 MSVC 构建） | 零 ABI 风险；`CompilerWarnings.cmake`、`CMakePresets.json` 已按 MSVC 配好；免费 |
| B. LLVM/Clang（`clang-cl`） | LLVM + **仍需** Windows SDK 与 MSVC 头文件 | ≈ 2 GB + SDK | MSVC ABI，兼容 | 无法摆脱 SDK；收益有限 |
| C. MinGW-w64（winlibs 免安装 zip） | 一个 zip，解压即用，无需 Windows SDK | ≈ 150–500 MB | Godot 官方文档**把 MinGW-w64 列为 Windows 可选项**；GDExtension 边界是 C ABI，故可行 | 零管理权限、体积最小；但非官方二进制所用工具链，**hot reload 不可用**，需 `-static-libgcc -static-libstdc++` |

**推荐 A**。装到 **D 盘**（C 盘仅剩 16.6 GB，D 盘 696.9 GB）；Build Tools 安装器允许自定义路径。

### 11.6 安装与使用步骤（**已完成**，2026-09-17）

实际落地为 **Visual Studio Community 2026**（而非最初建议的 Build Tools 2022）+ `Ninja 1.13.2`，均装在 D 盘，符合"避开 C 盘"的意图。安装位置：

```
VS:    D:\Program Files\Microsoft Visual Studio\18\Community
Ninja: D:\ninja-win\ninja.exe                       （已在 PATH）
SDK:   D:\Windows Kits\10\bin\10.0.26100.0\x64\     （非默认路径）
```

使用方式（**Ninja + cl.exe 必须在 MSVC 环境里运行**，这是 Ninja 相对 VS 生成器的关键差异）：

```bat
call "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset windows-editor
cmake --build --preset windows-editor --parallel
ctest --test-dir build\windows-editor --output-on-failure
cmake --install build\windows-editor
```

仓库已提供 `scripts\msvc_env.bat`（用 `vswhere` 自动定位并 `call vcvars64.bat`，已核实其 `-requires` 参数在本机能正确返回安装路径），四个既有脚本已改为预设驱动并自动先调用它。

### 11.7 与引擎**逐字节**对齐的可选步骤

`GODOTCPP_API_VERSION=4.7` 用的是 godot-cpp 自带的 API JSON。若要绑定到本机这个确切的 4.7.2 构建：

```bat
Godot_v4.7.2-stable_win64_console.exe --headless --dump-extension-api
cmake --preset windows-editor -DGODOTCPP_CUSTOM_API_FILE=D:\Develop\codespace\godot-3dtiles\extension_api.json
```

`GODOTCPP_CUSTOM_API_FILE` 优先级最高。建议在 Phase 4 首次加载扩展时用这种方式排除版本歧义。

### 11.8 本机 `git submodule` 不可用（实测）

```
git submodule status
→ git-submodule: line 7: basename: command not found
→ git-submodule: line 7: sed: command not found
→ git-submodule: line 22: .: git-sh-setup: file not found
```

Git 的 `submodule` 命令是 shell 脚本，依赖 `basename`/`sed`/`git-sh-setup`，在本机 PATH 下都取不到（**该故障与本次重构无关，是机器级 PATH 问题**）。影响与替代做法：

| 需求 | 替代命令 |
|---|---|
| 拉取 godot-cpp | `git clone --branch 10.0.0-stable --depth 1 https://github.com/godotengine/godot-cpp.git extern/godot-cpp` |
| 让已存在的模块 gitdir 升级到 v10 | `git --git-dir=.git/modules/extern/godot-cpp fetch --tags origin`<br>`git --git-dir=.git/modules/extern/godot-cpp --work-tree=extern/godot-cpp checkout -f 10.0.0-stable` |
| 记录新版本 | `git add extern/godot-cpp`（子模块指针更新） |

修好 PATH（让 Git 自带的 `usr/bin` 可见，或改用「Git Bash」环境）后 `git submodule` 即可恢复。

### 11.9 Phase 0 完成情况与**未验证项**

**已完成（结构性改动）**

1. 移除 `cesium-native`：`.gitmodules` 条目、gitlink、`extern/cesium-native` 目录、`extern/CMakeLists.txt` 中的 `add_subdirectory` 与 `cesium-native-wrapper`
2. 删除 19 个失去头文件即无法编译的 Cesium 耦合源文件（**5786 行**，`git diff --cached --stat` 实测）；保留 4 个 Cesium-free 文件
3. 清理**被误提交进 git** 的构建产物 `CMakeFiles/`（4 个文件）
4. 新增 `extern/third_party/`（glm 1.0.1 / nlohmann_json 3.11.3 / doctest 2.4.11，FetchContent + `FETCHCONTENT_SOURCE_DIR_*` 离线逃生通道）
5. `src/core/` 静态库建立，首个真实模块 `core/math/Mat4.{h,cpp}` + `tests/`（doctest）
6. 根 `CMakeLists.txt`：`extern` 提到 `src` 之前（否则 core 链接不到 `tiles3d_third_party`）；`ccache` 提到目标创建之前（原位置在 `add_subdirectory(src)` 之后，`CMAKE_CXX_COMPILER_LAUNCHER` 从未作用到主目标）；`CMAKE_INSTALL_PREFIX` 改为可被预设覆盖
7. 按 §11.2 修正 godot-cpp 选项名；按 §11.3 重写预设；`.gdextension` 提升到 `compatibility_minimum = "4.7"`
8. 修复既存缺陷：`templates/template.debug.gdextension.in` 的 macOS 路径写成 `Darwin-Universal`（大写 U），与实际生成的 `lib/Darwin-universal` 不符；`src/CMakeLists.txt` 的 `target_include_directories(... "src")` 实际解析为 `src/src` 且从不生效

**未验证（**必须**等工具链就绪后第一件做的事）**

> 因本机无编译器，`cmake --preset ...` 会在编译器自检阶段失败，故以下全部**未经运行验证**：
> - 全部 CMakeLists / CMakePresets 语法与链接关系
> - `GODOTCPP_API_VERSION=4.7` 是否命中 godot-cpp 自带的 API JSON
> - `GODOTCPP_TARGET=editor` + `GODOTCPP_USE_HOT_RELOAD=ON` 组合是否正常
> - FetchContent 三个依赖能否拉取并暴露 `glm::glm` / `nlohmann_json::nlohmann_json` / `doctest::doctest`
> - `core/math/Mat4.cpp` 与 `tests/test_mat4.cpp` 能否编译且断言通过
> - `.gdextension` 在 Godot 4.7.2 中能否解析并加载

### 11.10 首次构建交接（受限执行环境）

本机安全策略把 **`reg.exe` 列入程序黑名单**，且明确禁止绕过。这直接卡住了构建链：

```
vcvars64.bat / VsDevCmd.bat  →  靠 reg.exe 读注册表 KitsRoot10 定位 Windows SDK
                            →  reg.exe 被拦  →  SDK 路径未注入 PATH
                            →  rc.exe / mt.exe 找不到
                            →  CMake 编译器自检在链接阶段失败:
                               --mt=CMAKE_MT-NOTFOUND
                               RC Pass 1: command "rc /fo ..." failed ... no such file or directory
```

**这是环境假故障，不是配置错误**：`rc.exe` / `mt.exe` 实际存在于 `D:\Windows Kits\10\bin\10.0.26100.0\x64\`，且 `vcvars64` 本身返回 `VCVARS_EXIT=0`、`cl` 与 `link` 都能找到（`cl` 版本 `19.51.36257`）。只有 SDK 那一段缺失。

**因此首次构建必须在普通终端执行**（不受该黑名单约束）：

```bat
:: 推荐：Developer Command Prompt for VS 2026，或普通终端里先 call vcvars64
cd /d D:\Develop\codespace\godot-3dtiles
cmake --preset windows-editor
cmake --build --preset windows-editor --parallel
ctest --test-dir build\windows-editor --output-on-failure
cmake --install build\windows-editor
```

或直接跑 `scripts\debug_build_install.bat`（内部用 `vswhere` 定位并 `call vcvars64.bat`）。

首次构建的流程与耗时来源：`FetchContent` 拉 glm / nlohmann_json / doctest → 跑 godot-cpp 绑定生成器（Python 3.13.15）→ 编译 godot-cpp 静态库 → 编译 `tiles3d_core` + 扩展 + 测试。**首次较慢属正常。**

**首个需要留意的真实风险**：MSVC `19.51`（VS 2026）配 `/W4 /WX`。新编译器可能引入新告警导致 `/WX` 直接失败；`cmake/CompilerWarnings.cmake` 里一长串 `/w14xxx` 编号在 19.51 下可能已不存在（`/w14619` 已被显式关闭，正是为此准备的）。若出现 `C4619`/新告警，先临时 `-Dgodot-3dtiles_WARNING_AS_ERROR=OFF` 定位，再逐条处理而不是整体关掉。

同时清掉了两个受污染的构建目录：`build\windows-editor`（失败的缓存里 `CMAKE_RC_COMPILER=rc` 会被后续 configure 复用）与 `build\Windows-AMD64`（4.3 时代旧预设的残留）。

## 12. Phase 0 验证结果与 Phase 1 交付

### 12.1 Phase 0 验证结果（用户实测，2026-09-17）

§11.9 的"未验证"清单已被下列实测结论取代：

| 项 | 结果 |
|---|---|
| `cmake --preset windows-editor` | ✅ 通过（除下述网络问题外） |
| CMake 编译器自检 | ✅ 通过，`MSVC 19.51.36257.0` |
| `GODOTCPP_API_VERSION=4.7` 命中 API JSON | ✅ `GODOTCPP_GDEXTENSION_API_FILE = 'gdextension/extension_api-4-7.json'` |
| `GODOTCPP_TARGET=editor` / `SYSTEM_HEADERS=ON` / `USE_HOT_RELOAD=ON` | ✅ 全部进入 preset 并生效 |
| godot-cpp 绑定生成 | ✅ `There are 2135 Files to generate`，`Using 23 cores` |
| `cmake --install` | ✅ 成功，产物落到 `demo/addons` |
| `.gdextension` 在 Godot 4.7.2 中解析 | ✅ 被加载（否则不会报 class 未找到） |
| `core/math/Mat4.cpp` + `tests/test_mat4.cpp` | ⏳ 未验证（构建止于 FetchContent，未编到该目标） |

**demo 运行时的两条报错属预期**：

```
ERROR: Cannot get class 'CesiumGeoreference'.
ERROR: Cannot get class 'Cesium3DTileset'.
```

Phase 0 删除了这两个类，而 `demo/node_3d.tscn` 仍引用它们。它们会在 Phase 4 以 `Georeference3D` / `Tileset3D` 回归，届时按 §7.3 一起迁移场景。**不要为此改场景**。

### 12.2 FetchContent 网络问题的根因与对策

`extern/third_party/CMakeLists.txt` 的 FetchContent 在用户终端连不上 github.com：

```
fatal: unable to access 'https://github.com/g-truc/glm.git/': Failed to connect to github.com port 443 after 21123 ms
```

**根因**：本机（沙箱）环境变量 `HTTP_PROXY=HTTPS_PROXY=http://127.0.0.1:50639` 存在，所以我这边 `git ls-remote` / `git fetch` 到 github 正常；用户自己的终端没有该代理，直连 github 被墙。**这个不对称是本方案的设计失误**——给一个本可零网络依赖的工程在 configure 期加了联网下载。

三条对策（按推荐度）：

1. **预先填充 FetchContent 源目录**（最省事，不改 CMake）：
   ```bat
   cmake --preset windows-editor -DFETCHCONTENT_SOURCE_DIR_GLM=D:\deps\glm ^
                                 -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=D:\deps\json ^
                                 -DFETCHCONTENT_SOURCE_DIR_DOCTEST=D:\deps\doctest
   ```
   三个库都是 header-only，用可达镜像手工 clone 即可（已实测 `gitee.com/mirrors/glm.git` 与 `ghfast.top` 前缀均可达）。
2. **临时走代理**：与沙箱同法，给终端设 `HTTPS_PROXY`。
3. **彻底去掉联网依赖**：把三个库 vendor 进 `extern/third_party/vendor/`。glm / nlohmann / doctest 合计源码约 3 MB，代价可接受，收益是**离线可构建**。**Phase 2 开始前建议执行此项**，否则每次换机器/清 build 都会重踩。

### 12.3 Phase 1 交付内容

`tiles3d_core` 的数学层已完成（约 620 行实现 + 约 610 行测试）：

| 文件 | 内容 |
|---|---|
| `math/Types.h` | `Vec3` / `Vec4` / `Mat3` / `Mat4` 的 double 精度别名，集中说明为何必须是 double |
| `math/Mat4.{h,cpp}` | 参考实现的 `mat4Mul` / `mat4Inverse` / `identity` / `getMatrixMaxScale`，外加 `transformPoint` / `transformDirection` / `linearPart` / `transformLinear`（对应 three.js `applyMatrix4` / `applyMatrix3` / `Matrix3.setFromMatrix4`） |
| `math/GeoMath.{h,cpp}` | `Region` / `OrientedBoundingBox` 类型、`levelOffset`、`wgs84ToCartesian`、`wgs84SurfaceNormal`、`normalizeSafe`、`regionCorners`、**`regionToEcefObb`** |
| `math/BoundingVolume.{h,cpp}` | box / sphere / region 三态（固定 12 槽 payload，保持 trivial 可拷贝）、`boundingVolumeCenter`、`boundingVolumeRadius`、`subdivideBox` |
| `math/ScreenSpaceError.{h,cpp}` | `computeScreenSpaceError`、`computeSurfaceDistance`、`orientedBoxDistanceToPoint`、`computeBvSurfaceDistance`、`fog` |
| `tests/test_mat4.cpp` | 9 个用例，含 D1 坐标系不变式与 double 精度必要性的量化断言 |
| `tests/test_geomath.cpp` | `levelOffset` 期望值、WGS84 已知点、`regionToEcefObb` 的正交性与**角点包含性**不变式、跨赤道与 >π 两条分支 |
| `tests/test_bounding_volume.cpp` | 直接移植 spec 的 center / radius / subdivideBox（含四叉树叶节点 z 保持、八叉树 child 5 位序） |
| `tests/test_screen_space_error.cpp` | 直接移植 spec 的 SSE / 表面距离 / fog，外加 OBB 距离与 world matrix 换帧 |

**实现取舍**（与参考实现的差异，均已注释说明）：
- `levelOffset` 用 64 位整数迭代求值而非 `pow()`，结果精确（可表示到四叉树 31 层 / 八叉树 21 层）
- 向量运算直接用 glm（`glm::dot` / `cross` / `length`），仅 `normalizeSafe` 单独实现——因为参考实现的 `normalize3` 在零长度时返回原向量而非 NaN，语义与 `glm::normalize` 不同
- `BoundingVolume` 用固定 12 槽 `std::array<double,12>` 而非变长容器，避免每瓦片一次堆分配
- 参考实现的 `BoundingVolume | undefined` 语义在 C++ 侧映射为 `const BoundingVolume *`（可空指针），已用于 `computeBvSurfaceDistance`
- `convertRegionBoundingVolumes` **未在 Phase 1 交付**，因为它遍历瓦片树，依赖 Phase 2 的 `Tile` 类型；随 Phase 2 一起落地

### 12.4 Phase 1 待验证（下一步唯一动作）

```bat
cmake --preset windows-editor          :: 需先解决 12.2 的依赖来源
cmake --build --preset windows-editor --parallel
ctest --test-dir build\windows-editor --output-on-failure
```

期望：`tiles3d_core` 与 `tiles3d_tests` 编译通过，4 个测试文件全部用例通过（约 33 个断言块）。

**已知需留意的点**：`BoundingVolume.cpp` 的 `volume.data = { ... }` 依赖 `std::array` 从花括号列表赋值；`regionToEcefObb` 的 `>π` 分支尚无参考 spec 覆盖，我用正交性 + 有限性断言守住，若后续拿到真实大区域数据需补强。

### 12.5 首次编译迭代（2026-09-17）

**结论：`tiles3d_core.lib` 成功链接** —— Phase 1 的四个实现文件（`Mat4` / `GeoMath` / `BoundingVolume` / `ScreenSpaceError`）**全部编译通过**。失败只发生在测试目标。

#### 失败 1：doctest 判定"异常被禁用"

```
error C2338: static assertion failed: 'Exceptions are disabled!
Use DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS if you want to compile with exceptions disabled.'
```

**根因链（已逐层取证）**：

1. `doctest.h` 第 289-293 行：
   ```cpp
   #if !defined(__cpp_exceptions) && !defined(__EXCEPTIONS) && !defined(_CPPUNWIND)
   #define DOCTEST_CONFIG_NO_EXCEPTIONS
   ```
   MSVC 下 `_CPPUNWIND` 只在传了 `/EHsc` 一类开关时才定义。
2. `build/windows-editor/CMakeCache.txt` 里 **`CMAKE_CXX_FLAGS` 与 `CMAKE_CXX_FLAGS_DEBUG` 都是空的**，于是编译命令里既没有 `/EHsc` 也没有 `/Zi /Od /RTC1`。
3. 而 CMake 的 `Modules/Platform/Windows-MSVC.cmake`（第 196/218/254 行）默认**是带 `/EHsc`** 的，try-compile 阶段也确实出现过 `/DWIN32 /D_WINDOWS /EHsc`。

**判定：该 build 目录的 cache 已损坏**，不是配置问题。证据是用户第一次 configure 失败于 FetchContent、第二次直接报 `You have changed variables that require your cache to be deleted` 与 `CMake can not determine linker language`——CMake 自己两次指出 cache 不一致。**处理：删掉 `build/windows-editor` 重新 configure。**

同时按 **D-11** 把异常从 core 移除，使测试不再依赖 `/EHsc`（见下）。

#### 失败 2：`doctest::Approx` 没有 `margin()`

`test_geomath.cpp` 有 4 处 `doctest::Approx(0.0).margin(1e-6)`。`doctest::Approx` 只有相对 `epsilon()`，**`margin()` 是 Catch2 的 API**。改用显式绝对差 `std::abs(x) < tol`。

> 讽刺的是 `test_mat4.cpp` 里我早就在注释里写过"doctest 没有 margin"并用的是绝对差，却在 `test_geomath.cpp` 忘了——所以同一批测试里一个编过一个编不过。

#### 本轮改动

| 改动 | 说明 |
|---|---|
| core 去掉全部异常 | `levelOffset` / `asRegion` / `subdivideBox` 改为 `assert` + 定义明确的兜底返回；移除 `<stdexcept>`，加 `<cassert>` |
| 测试去掉 `CHECK_THROWS_*` | 由 6 处减为 0；改为验证**有效路径**与**参考闭式公式一致性** |
| `margin()` → 绝对差 | 4 处 |
| 补充说明 | 前置条件违反不再由测试覆盖——被违反的 `assert` 在 Debug 下会**中止测试进程**，所以刻意不测。这是 D-11 的已知代价，已在该测试文件顶部注明 |

#### 未解决 / 待观察

- **`CMAKE_CXX_FLAGS*` 确认为空，但影响比预期小，已定案**（见 §12.6）。真实编译命令里既没有 `/Zi` 也没有 `/EHsc`，但 **PDB 照样被生成**（`/Fd` + CMake 3.25 起的 `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` 机制），所以"Debug 构建没有调试信息"这一担心不成立。`/EHsc` 缺失只影响 doctest 对异常的判断，而 core 已改为无异常（D-11），不再有影响。**结论：不需要为它加 CMake 补丁。**
- `regionToEcefObb` 的 `>π` 分支仍无参考 spec 覆盖，靠正交性 + 有限性断言守住。

#### 下一步

```bat
rmdir /s /q build\windows-editor
cmake --preset windows-editor
cmake --build --preset windows-editor --parallel
ctest --test-dir build\windows-editor --output-on-failure
```

### 12.6 第二轮：测试二进制链接成功但运行时访问冲突

`tiles3d_core` 与 `tiles3d_tests` 都编译链接通过，安装也正常（`godot-3dtiles-d.dll` 3244 KB + PDB）。但测试可执行文件**直接崩溃**：

```
exit code = -1073741819  (0xC0000005 = STATUS_ACCESS_VIOLATION)
```

**症状定位**（这一步很关键，它直接排除了测试代码的嫌疑）：

| 调用 | 结果 |
|---|---|
| `tiles3d_tests.exe --help` | ✅ exit 0，正常打印选项表 |
| `tiles3d_tests.exe --list-test-cases` | ❌ 0xC0000005，**且无任何输出** |
| `tiles3d_tests.exe`（裸跑） | ❌ 0xC0000005，**且无任何输出** |

`--list-test-cases` 不执行任何测试体，却也崩溃且**连框架 intro 都没输出** → **崩溃发生在静态初始化/测试注册阶段，在 main 打印任何东西之前**，与我的测试逻辑无关。

**根因**：MSVC **14.51（VS 2026）** 与 doctest 的前向声明冲突。

- doctest 在未定义 `DOCTEST_CONFIG_USE_STD_HEADERS` 时，会自己 `namespace std` 里前向声明 `std::tuple` / `std::allocator` / `std::basic_string`（`doctest.h` 约 538-546 行），目的是省下 `<iostream>/<string>/<tuple>` 的 include 开销。
- MSVC 14.51 的 STL 拒绝这种重声明 → `warning C5285`（**之前被我误判为无害，已纠正**）。
- 类型身份在 doctest 的 TU 与 STL 之间不一致构成 ODR 违反 → 静态注册期内存布局错乱 → 访问冲突。

**旁证**：NVIDIA Slang 项目在 2026-06 踩到同一个问题——其 CI 记录原文：

> *"GitHub's hosted Windows runner image bumped MSVC to 14.51.x, which newly emits warning C5285 on doctest's vendored std::tuple forward declaration"*

他们因为编译带 `/WX`（警告即错误，C2220）而卡在编译期，没走到运行期；我们没对测试目标开 `/WX`，所以二进制生成了出来，才暴露成崩溃。

**修复**（doctest 官方 FAQ 针对"STL 实现定义不同"情形指定的开关）：

```cmake
target_compile_definitions( tiles3d_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS )
```

它会强制 doctest 包含真实标准库头，前向声明整段消失（`C5285` 随之消失）。必须对**所有**包含 `doctest.h` 的 TU 可见，故放在 target 级而非某个源文件里。

**若此修复无效的后备方案**：换掉 doctest，改用约 80 行的自建极简 assert/表驱动 harness，彻底消除第三方工具链漂移风险。本项目已连续两次被第三方摩擦拖慢（FetchContent 断网、doctest 与 MSVC 14.51 冲突），自建 harness 的维护成本可能低于持续排障成本——待验证结果决定。

**附注**：本轮用户**未清除** `build/windows-editor`（证据：增量构建 `[0/2] Re-checking globbed directories...`，且 `CMakeCache.txt` 中 `CMAKE_CXX_FLAGS*` 仍为空）。因此"删目录重配"仍是未验证的动作；但由于 PDB 不受影响（见上），**它现在只是卫生问题，不是阻塞项**。

### 12.7 第三轮：测试首次真正执行 —— 28/29 通过

`DOCTEST_CONFIG_USE_STD_HEADERS` 生效后（`C5285` 消失，编译与链接正常），**测试二进制终于跑起来了**：

```
[doctest] test cases:  29 |  28 passed | 1 failed | 0 skipped
[doctest] assertions: 225 | 223 passed | 2 failed |
```

**这是 Phase 1 的全部断言第一次真正执行。** 唯一的失败**不是代码 bug，而是我测试里的期望值算错了**：

```
test_geomath.cpp(170): CHECK( glm::length( box.halfAxes[0] ) < 7000.0 )
  values: CHECK( 318779 < 7000 )
test_geomath.cpp(171): CHECK( glm::length( box.halfAxes[1] ) < 7000.0 )
  values: CHECK( 316648 < 7000 )
```

**我的错误**：注释里写"0.05 rad 经度在赤道约 5.6 km 半宽"——把**弧度当成了角度**。实际 0.05 rad × 6378137 ≈ **318.9 km**，差 57 倍。

**手算复核代码是对的**：北向半轴 = `(1-e²)·N·sin(0.05) + 100·sin(0.05)`，其中 `N(lat=0.05) ≈ 6378190`、`(1-e²)·N ≈ 6335495` → `6335495 × 0.049979 + 5.0 ≈ 316,642`，实测 **316,648**，吻合到 0.002%。`regionToEcefObb` 在跨赤道分支上的实现是正确的。

**已修正该用例**：把凭感觉猜的边界换成**解析期望值** `0.05 · kWgs84SemiMajorAxis`（2% 容差），并增加一条有意义的断言——z 半轴必须远小于水平半轴（637 km 跨度上椭球面下垂约 16 km，远大于 200 m 的高度带，这个量级关系本身值得钉住）。测试反而比以前更硬。

**遗留**：`__msvc_ostream.hpp(613): warning C4530: 使用了 C++ 异常处理程序，但未启用展开语义。请指定 /EHsc`。这仍是那个空 `CMAKE_CXX_FLAGS` 造成的——`DOCTEST_CONFIG_USE_STD_HEADERS` 把真实 STL 头拉进来后，缺 `/EHsc` 就显形了。测试目标没开 `/WX` 所以只是警告，但**扩展 DLL 也在同一个缺 `/EHsc` 的环境下构建**，对 godot-cpp 大量使用 STL 的代码是潜在隐患。**建议清理 `build/windows-editor` 重建**（一次性命令，同时清掉 `_deps`）。

### 12.8 当前状态与下一步

| 项 | 状态 |
|---|---|
| Phase 0（移除 cesium-native、构建重构、4.7/godot-cpp v10 对齐） | ✅ 完成并实测 |
| Phase 1（core 数学层，约 620 行实现 + 约 610 行测试） | ✅ 实现完成，**28/29 用例通过**，1 个失败用例已修正待复跑 |
| doctest × MSVC 14.51 兼容问题 | ✅ 已修（`DOCTEST_CONFIG_USE_STD_HEADERS`） |
| 空 `CMAKE_CXX_FLAGS*`（→ 缺 `/EHsc`，C4530） | ⚠️ 待清理 build 目录解决 |

下一步二选一：**Phase 0.5（Spike S1 裁决 glTF 装配路线）** 或直接 **Phase 2（瓦片树 + 解析器）**。S1 只阻塞 Phase 4，不影响 Phase 2/3。

---

## 13. Phase 2 交付

### 13.1 为什么拆成 2a / 2b / 2c

Phase 2 原计划一次性交付 `Tile` + 隐式瓦片 + `B3dmParser`，实测规模约 1500 行 C++ + 约 500 行 TS 参考需要通读。拆成三段，每段都能独立编译验证：

| 段 | 内容 | 状态 |
|---|---|---|
| **2a** | `Tile` + `TilesetJson` 解析 + `convertRegionBoundingVolumes` | ✅ 已交付，待编译 |
| 2b | 隐式瓦片：`BitStream` / `SubtreeReader` / `ImplicitTileManager` | 待做 |
| 2c | `B3dmParser` + 内容类型分派（`ContentType`） | 待做 |

### 13.2 2a 交付内容

| 文件 | 行数 | 内容 |
|---|---|---|
| `tiles/Tile.h` / `.cpp` | 167 / 101 | `RefineMode`、`ContentState`、`TileContent`、`ImplicitTiling`、`ImplicitCoordinates`、`Tile` 类、`convertRegionBoundingVolumes` |
| `tiles/TilesetJson.h` / `.cpp` | 62 / 285 | `parseBoundingVolume`、`TilesetParseResult`、`parseTilesetJson`（递归构建瓦片树） |
| `tests/test_tile.cpp` | 184 | 12 个用例：region 转换公式、链式累积、内容 BV、box/sphere 不变、半径缓存失效、空 BV 容错 |
| `tests/test_tileset_json.cpp` | 322 | 11 个用例：1.0/1.1 隐式解析、显式树、**refine 继承**、**pre-order id**、四类解析失败 |

### 13.3 关键设计决策

| 决策 | 理由 |
|---|---|
| **子节点用 `std::vector<std::unique_ptr<Tile>>`** | 地址稳定性是硬需求：Phase 3 的加载队列、LRU 缓存、每帧可见集合全都持有裸 `Tile*`。值语义的 `vector<Tile>` 在扩容时会搬移元素，直接失效 |
| **`ContentState` 用 6 态枚举**，替代参考实现的 4 个布尔（`loaded`/`loading`/`contentReady`/`loadFailed`） | 参考实现的布尔无法表达"字节已到、正在主线程装配"这一中间态，而 Godot 侧的 glTF→节点装配恰恰需要一个不阻塞遍历的中间态。枚举同时对齐 Cesium `Cesium3DTileContentState`，且 `contentStateName()` 直接给 trace-diff 输出用 |
| **`Tile::id` = 解析期 pre-order 序号** | `tools/sched_trace` 要逐行 diff C++ 与 TS 的调度决策，必须有一个跨实现确定一致的瓦片标识 |
| **解析失败经 `TilesetParseResult::error` 返回，不抛异常** | 延续 D-11。`TilesetParseResult` 同时带出 `assetVersion`（内容加载需区分 tileset 文档与 glTF 文档）与 `geometricError` |
| **`parseBoundingVolume` 校验数组长度** | 参考实现不校验，`box` 少几个元素会在很久之后变成 NaN 几何；在这里失败成本低得多 |
| **`refine` 继承语义完整保留**，但对未知取值采取"继承"而非原样采纳 | 参考实现里 `refine` 的继承是修过的真实 bug（倾斜摄影只给根标签 REPLACE，子级不写 → 默认 ADD 会让每层都渲染、粗父级盖住细子级）。未知值继承是比参考实现更严格的失败安全 |
| **内容模板判定保持 `只查 '{'`**（严格版会同时要求 `'}'`） | 畸形输入下的行为一致性比"更安全"更重要，解析层刻意与参考实现逐字对齐 |
| **`Tile::boundingVolumeRadius()` 记忆化 + 显式 `invalidateBoundingVolumeRadius()`** | 参考实现只做惰性缓存，且**没有**失效接口——它靠"转换一定发生在任何半径查询之前"这一时序侥幸成立。显式失效把这条隐含依赖变成可测的契约（`test_tile.cpp` 里有专门的回归守卫） |
| **`convertRegionBoundingVolumes` 把 world 线性部分提到递归外** | 参考实现每个包围盒都重算一次 `modelMatrix` 的线性部分；它整棵树恒定，属于无谓开销 |

### 13.4 与参考实现的行为等价点（刻意保留）

- `region` 转换公式 `box_tileLocal = chain⁻¹ · modelMatrix · OBB(ecef)`，且同时作用于**瓦片自身 BV 与内容 BV**
- 非 region 包围盒原样返回（box/sphere 不被触碰）
- 隐式瓦片的内容模板从**瓦片层级**搬到 `ImplicitTiling::contentUriTemplate`（1.0 扩展式与 1.1 核心属性两条路径都处理）
- `1.1 核心属性 implicitTiling` 优先于 `1.0 extensions['3DTILES_implicit_tiling']`
- 根瓦片 `implicitCoordinates = {0,0,0,0}`

### 13.5 验证状态

**未编译**。改动全在 `src/core/`（由 GLOB 自动纳入 `tiles3d_core`）与 `tests/`（同样自动纳入），无需改 CMake。下一步：

```bat
.\scripts\debug_build_install.bat
```

然后由我直接运行 `tiles3d_tests.exe` 取结果（预期 29 + 23 = 52 个用例）。**首次编译大概率需要修一两个小问题**——新代码用了 `if` 带初始化语句（C++17）、`std::optional`、`glm` 的 `dmat3`/矩阵-向量乘等，都没在本工程编译过。

### 13.6 2a 已埋好但尚未使用的伏笔

- `Tile::contentState` / `contentBytes` / `loadErrorCount` — Phase 3 的加载状态机与内存账务用
- `Tile::isExternalTileset` — 外部 tileset 容器瓦片用（Phase 3 `loadTileContent`）
- `Tile::worldMatrix` — Phase 3 遍历写入
- `ImplicitTiling::contentUriTemplate` / `subtreeUriTemplate` — 2b 的 `ImplicitTileManager` 用
- 参考实现里 `Tile` 的遍历 scratch 字段（`_sse` / `_distanceToCamera` / `_radius` / `_foveatedFactor` / `_priorityDeferred` / `cacheNode` / `_touchedFrame`）**故意未加**：它们只在 Phase 3 有用，提前加入就是无法验证的死字段，届时随使用它的代码一起落地

## 14. 目录布局决策：不拆分 `include/` 与 `src/`

**结论：头文件与实现同目录（现状保持），不引入 `include/` 树。** 理由不是"项目小所以随意"，而是 `include/` 在本项目**语义上不成立**。

### 14.1 判断依据：`include/` 是对外承诺，我们没有对外承诺

`include/`（或 `public/`）的唯一职责是**把"我承诺长期支持的接口"与"实现细节"分开**——它服务于**外部消费者**。

| 项目 | 有外部消费者吗 | `include/` 是否成立 |
|---|---|---|
| `godot-cpp` | ✅ 有，全世界的扩展都 `#include <godot_cpp/...>` 并链接它 | ✅ 成立，`include/` 就是它的公开 API |
| `geotwin` | ✅ 有，它是应用且带 OSG 插件 API（`include/osgPlugin.h`）、`CommandLine.h` 入口 | ✅ 成立 |
| **本项目** | ❌ **没有**。产物是单个 `godot-3dtiles.dll`，对外接口只有 `.gdextension` 文件里的 `entry_symbol` | ❌ **不成立** |

GDExtension 是一个**运行时加载的单一动态库**：没有任何外部工程会 `#include` 我们的头文件，也不会链接我们。给它套一层 `include/` 等于声明一个**根本不存在的公开 API**，还要额外承担"两个目录保持镜像"的维护成本。

### 14.2 geotwin 的拆分不可直接照搬

实测其结构：`include/` 68 个头文件、`src/` 54 个实现，**8 个模块镜像目录**（`converter` / `geo` / `render` / `resource` / `scene` / `storage` / `ui` / `utils`，两边同名），引用方式为 `#include "converter/GltfBuilder.h"`（即 include 根是 `include/`）。总量约 120 个源文件，依赖用 `vcpkg.json` + `extern/` 双轨。

它拆分的动机来自**它的处境**：文件多到需要两棵树来降低单目录噪声、有 OSG 插件这种真正的对外接口、include 根需要与 vendored `extern/` 划清。**这些条件本项目一个都不具备**（当前 C++ 文件合计约 30 个）。

### 14.3 混合布局反而是本生态的惯例

- 官方 `godot-cpp-template`：`src/` 混合
- 本仓库的来源 `GDExtensionTemplate`（asmaloney）：`src/` 混合
- 本仓库现状：`src/` 混合

贡献者看到的是他们期待的形态。**在插件工程里，熟悉的形状比自创的形状更有价值。**

### 14.4 真正该照搬 geotwin 的三点（我们已具备）

| geotwin 的做法 | 本项目 |
|---|---|
| 按模块划分子目录 | ✅ `core/{math,tiles,implicit,content,net}` |
| 每个目标单一 include 根 | ✅ Godot 层根为 `src/`；`tiles3d_core` 根为 `src/core/` |
| 根目录放 `.clang-format` | ✅ 已有 |
| 三方依赖集中放 `extern/` | ✅ `extern/third_party/` |

**不要照搬的一点**：geotwin 同时用 `vcpkg.json` 和 `extern/glm`，是两套依赖机制并存（`CMakeLists.txt` 里还硬编码了 `GLM_ROOT`）。本项目只有一套（FetchContent + `FETCHCONTENT_SOURCE_DIR_*` 离线逃生），**不要再叠加 vcpkg**。

### 14.5 比 include/src 更值得管的事：include 拼写的规范性

`tiles3d_core` 是通过 **PUBLIC** include 目录暴露的，所以 Godot 层同时能看到 `src/` 与 `src/core/` 两个根，导致**同一个头文件有两种可解析拼写**：

```cpp
// Godot 层：两种都能编过，但目前统一用前者
#include "core/math/GeoMath.h"   // ← 实际使用
#include "math/GeoMath.h"        // ← 也能解析（因为 core 的 PUBLIC include 目录）
```

这是一个**真实的可读性隐患**：同一文件两个名字，日后很容易写成两种风格并存。修法不是重构目录，而是**加一条门禁**：Godot 层引用 core 头文件必须带 `core/` 前缀。写进 §6.4 的 CI 检查即可：

```bash
# Godot 层引用 core 头文件必须带 core/ 前缀（排除 core 自身与 tests）
rg -n '#include\s+"(math|tiles|implicit|content|net)/' src/*.h src/*.cpp && exit 1
```

> 注意：`src/core/` 内部与 `tests/` 用短拼写（`"math/Mat4.h"`）是**正确**的——它们的 include 根就是 `src/core`。

### 14.6 什么时候回头拆

不是"以后再看"，而是有明确触发条件——**任一条成立就重新评估**：

1. C++ 文件总数 **超过约 60 个**（当前约 30）；
2. 出现**任何外部消费者**（例如 `core` 被拆成独立仓库/独立发布、或有人把本插件当库链接）；
3. `core` 需要**独立版本号与发布节奏**；
4. 出现第二个 GDExtension（或多目标共享 `core`），使"公开 API"概念真实存在。

在那之前，拆分只有成本没有收益。

## 15. Godot 节点暴露（可在编辑器中测试）

### 15.1 本阶段交付

| 文件 | 内容 |
|---|---|
| `src/core/math/GeoMath.{h,cpp}` | 新增 `eastNorthUpToFixedFrame`（ENU→ECEF 帧），`tests/test_geomath.cpp` 加 3 个用例（赤道课本帧、锚点处正交性与往返求逆、局部轴语义） |
| `src/Georeference3D.{h,cpp}` | `Node3D`；`origin_authority`（LLH 或 ECEF 资源）→ `local_to_ecef()` / `ecef_to_local()`，懒计算 + 缓存；监听资源信号自动刷新 |
| `src/Tileset3D.{h,cpp}` | `Node3D`；`url` 属性 → 读文件 → core 解析 → `convertRegionBoundingVolumes` → **把瓦片包围盒画成线框**；统计属性与 `dump_tree()` |
| `src/RegisterExtension.cpp` | 注册 `Georeference3D` / `Tileset3D` |
| `demo/node_3d.tscn` | 迁移为 `Georeference3D` + `Tileset3D`（指向 1.0 数据集） |

### 15.2 为什么先画线框而不是先渲染内容

内容渲染（b3dm → glTF → Godot 节点）需要 Phase 3 的遍历调度器与 Phase 4 的内容管线。而**线框不依赖任何内容管线**，却能一次性验证整条帧链：

1. 文件读取与 JSON 解析是否成立
2. 瓦片树与 `refine` 继承是否正确（1.0 数据集全文件只有根写了 REPLACE）
3. `regionConvert` 是否执行（本例无 region，属未覆盖路径）
4. `modelMatrix` 与 `worldMatrix` 累积是否让数据集落在原点附近
5. 局部坐标系（Z-up ENU）经 georeference 节点的 Z-up→Y-up 旋转后，在 Godot 里朝向是否正确

这是**成本最低的可观测性投入**。按深度着色（`Color::from_hsv(0.11 × depth, …)`）让 LOD 层级一眼可辨。

### 15.3 编辑器里怎么用

1. 打开 `demo/` 工程，场景 `node_3d.tscn`
2. 选中 `Tileset3D` 节点 → 按 `F` 聚焦（线框在原点附近，约 566 m × 531 m × 41 m）
3. 输出面板会打印一行加载摘要：
   `[Tileset3D] loaded '…': version=1.0 tiles=374 maxDepth=N rootGE=777.242 georeferenced=yes`
4. 想看树结构：在脚本或编辑器控制台调用 `$Tileset3D.dump_tree(3)`
5. 改 `url` 会**自动重载**（`set_url` 在 `is_inside_tree()` 时触发 `reload`）；也可直接调 `reload()`

**换数据集前注意**：`url` 指向 1.1 数据集只会解析出根瓦片（隐式瓦片需要 Phase 2b 的 subtree 读取器）。同时**应把 georeference 的 ECEF 原点改成该数据集 root transform 的平移值**，否则数据集会偏离原点、坐标变大。

### 15.4 节点 API

| 类型 | 成员 |
|---|---|
| `Georeference3D` | 属性 `origin_authority`（`LongitudeLatitudeHeight` / `EarthCenteredEarthFixed`）、`scale`；方法 `refresh()`；信号 `georeference_changed` |
| `Tileset3D` | 属性 `url`、`maximum_screen_space_error`（**本阶段仅存储，Phase 3 才消费**）、`debug_show_bounding_volume`、`debug_bounding_volume_scale`；方法 `load()` / `reload()` / `unload()` / `dump_tree(max_depth)`；只读 `get_tile_count()` / `get_maximum_depth()` / `get_asset_version()` / `get_root_geometric_error()` / `get_last_error()` / `is_placed_by_georeference()`；信号 `tileset_loaded` / `load_failed(reason)` |

**无 georeference 父节点时的回落**：`modelMatrix = inverse(root.transform)`（参考实现的做法），数据集居中在原点但**不在地球上的正确位置**。加载摘要里的 `georeferenced=no` 就是这条路径。

### 15.5 godot-cpp 10.0.0 的 API 陷阱（本次实测踩到，勿再猜）

写节点代码时**不要凭记忆写 godot-cpp 的符号**，先 grep 头文件。本轮踩到的四处：

| 误写 | 正确 | 说明 |
|---|---|---|
| `#include "godot_cpp/classes/base_material_3d.hpp"` | **`base_material3d.hpp`** | 类名 `BaseMaterial3D` 对应的文件名**数字前无下划线**；`standard_material3d.hpp` 同理 |
| `material->set_vertex_color_use_as_albedo(true)` | `material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true)` | Godot 把这些 property 归到 `set_flag(Flags, bool)` 下，没有逐属性的 setter |
| 以为 `memnew` 在 `namespace godot` 里 | 它是**宏**（`memory.hpp:133`），展开为 `::godot::_post_initialize(new (DefaultAllocator{}) …)` | 无需限定命名空间 |
| 以为 `utility_functions.hpp` 是手写头 | 它在**生成目录** `gen/include/godot_cpp/variant/utility_functions.hpp` | v10 起手写头在 `extern/godot-cpp/include/`，生成头在 `build/<preset>/extern/godot-cpp/gen/include/`；`vformat` 声明在 `variant/variant.hpp:353` |

**通用做法**：`Godot3DTiles`/`Georeference3D` 这类节点用到的枚举与签名，一律 grep `build/windows-editor/extern/godot-cpp/gen/include/godot_cpp/classes/<snake_case>.hpp` 确认后再写。这比编译一轮快得多。

---

## 16. 内容管线（Phase 2c + Phase 4 前半）

线框验证帧链之后，用户问「怎么看不到瓦片」——因为当时**没有任何读 `.b3dm` 的代码路径**。本节交付内容管线的前两块，第三块（遍历调度）见 §16.4。

### 16.1 Phase 2c：`B3dmParser`

`src/core/content/B3dmParser.{h,cpp}` + `tests/test_b3dm_parser.cpp`（6 个用例）。

读参考实现（`index.ts:748-816`）后照实实现，并纠正了我此前的一个**错误猜想**：我原本以为参考实现只累加 JSON 表长度、会漏掉二进制表长度——**实际它是规范的**（`offset` 里同时加了 `featureTableBinaryByteLength` 与 `batchTableBinaryByteLength`，再做 8 字节对齐）。照此实现。

与参考实现的两处**有意差异**：

| 项 | 参考实现 | 本项目 | 理由 |
|---|---|---|---|
| GLB 返回方式 | `buffer.slice()` 复制一份 | 返回 `glbOffset` / `glbLength` | 大瓦片不做无谓复制；调用方在需要 `PackedByteArray` 时再切 |
| 越界校验 | 信任头部 `byteLength` | 校验 `byteLength <= size` 及各表边界 | 参考实现对截断的缓冲会切出越界区间；这里把静默 OOB 变成明确报错 |

保留的参考语义：magic 必须 `b3dm`、`version` 必须为 1、`batchTableJson` 在无 batch table 时为空、`RTC_CENTER` 与 `BATCH_LENGTH` 从 feature table JSON 读取。

### 16.2 D-2 定案：内容装配走 Godot `GLTFDocument`

**不再需要 Spike S1 来「裁决」，直接定为方案 A。** 理由：

1. **它已经把 1306 行手工装配要解决的问题全部解决了。** 原 `GodotPrepareRendererResources.cpp` 的绝大部分工作量是在重实现 glTF 导入器已有的能力（材质映射、sampler、UV、mipmap、sRGB），且在其中引入了 §0.4 的 9 项缺陷。
2. **零新增原生依赖。** 方案 B 要引入 `cgltf`/`tinygltf` + `draco` + `basisu`，而 Godot 已自带 Draco 与 Basis/KTX2。
3. **它产出的是标准 `Node3D`/`MeshInstance3D`**，直接接入 Godot 渲染管线（阴影、GI、标准材质），而自建 `ArrayMesh` 需要自己对齐这一切。
4. 当初列为门槛的「主线程耗时」问题，靠 §16.3 的每帧预算处理，不需要靠"在 worker 线程建 mesh"来回避。

**Spike S1 的定位随之改变**：从「决定 A 还是 B」降级为**验证 A 的覆盖率**——具体是第 3 条门槛（fixture 能否正确出图，Draco / KTX2 是否正常）与第 4 条（主线程耗时是否可接受）。若验证失败再回退到 B/混合，届时 §5 的其余门槛仍适用。

### 16.3 交付内容

| 文件 | 内容 |
|---|---|
| `src/core/content/B3dmParser.{h,cpp}` | b3dm 头 + feature/batch table + 内嵌 GLB 定位；无异常，错误经 `error` 返回 |
| `tests/test_b3dm_parser.cpp` | 6 用例：正常解析、无 batch table、8 字节对齐、外来 magic、非法版本、截断缓冲 |
| `src/GodotMathConvert.h` | **double→float 的唯一转换点**：`toGodotVector` / `toGodotTransform`。`Tileset3D.cpp` 里的局部副本已删除，两处消费方共用 |
| `src/ContentFactory.{h,cpp}` | 容器识别（b3dm / 二进制 glTF）→ `GLTFDocument` → `Node3D`；上轴校正与 `RTC_CENTER` 的放置 |

**变换约定（关键，容易搞错）**：

```
wrapper.transform  = worldMatrix                        // 瓦片局部 → 渲染帧 R
gltfRoot.transform = rotationX(+90°) + origin=RTC_CENTER
```

- `rotationX(+90°)` 是 glTF 的 Y-up → 瓦片 Z-up 校正，**只作用于内容，绝不作用于包围盒**
- `RTC_CENTER` 是**位置分量**，因此不随上轴校正旋转——它定义在瓦片坐标系里，所以是"在旋转之外合成"而不是"被旋转带动"。参考实现（`premultiply` 后再设 `position`）正是这个顺序
- 用一层 wrapper 承载 `worldMatrix`，glTF 根承载校正+RTC。这样可见性切换与释放都只针对 wrapper，也避免了把一个含 RTC 的变换提前合成进 `worldMatrix`

`GLTFDocument` 的 API（已 grep 生成头确认）：
```cpp
Error append_from_buffer( const PackedByteArray &bytes, const String &basePath, const Ref<GLTFState> &state, uint32_t flags = 0 );
Node *generate_scene( const Ref<GLTFState> &state, float bakeFps = 30, bool trimming = false, bool removeImmutableTracks = true );
```

### 16.4 遍历调度（已交付）

`Tileset3D::traverse_tile` **逐条移植**自参考实现 `index.ts:1699-1940`。动手前先读了源码——下面这些语义**全部不能凭直觉发明**，其中两条直接推翻了我事前的设想：

| 规则 | 含义 |
|---|---|
| `refines = hasRenderableContent` | REPLACE 下，父级**只有自身内容已就绪**时才可能继续渲染 |
| 子级"有内容但未就绪" → `refines = false` | **父级回退渲染，覆盖加载期间的空洞**（这就是我此前不确定的那条边界语义） |
| 无可见子级 → `refines = false` | 防洞；本项目暂无视锥裁剪，子级存在即视为可见 |
| `forceRefine = !ready && !hasContentUri && GE > 0` | 无内容的瓦片必须继续细化，否则是死路 |
| `unconditionallyRefine = !hasContentUri \|\| GE >= nearestConditionalGE` | NASA `canUnconditionallyRefine`：数据 GE **非单调反弹**时，父级 SSE 达标不代表子级精度达标，必须细化到 GE 收敛层 |
| `childConditionalGE = (hasContentUri && !unconditionallyRefine) ? GE : nearestConditionalGE` | 子级继承最近"条件父级"的 GE |
| `nearestConditionalGE` 初值 = **+∞** | 任何有限值都会让根要么永远无条件细化、要么永不细化 |
| `requestContent` 只在"停止细化"或"细化时对子级"被调用 | **推论：相机靠近时，根瓦片的 b3dm 根本不会被加载**——请求直接下钻到满足 SSE 的深层瓦片。我原先"先加载根、再逐级细化"的设想**是错的** |

**本阶段有意未实现**（属 Phase 3 完整范围，逐条在代码里标了 TODO）：

- **视锥裁剪**（`isChildVisible` / `cullWithChildrenBounds`）。缺它不会出错：细化本身被 SSE 限制，相机背后的瓦片距离远、SSE 小、自然停止细化；损失的只是"完全不去访问"的那部分节省，以及 `cullWithChildrenBounds`（REPLACE 瓦片子级可能超出父级包围体，需用并集做保守裁剪）
- foveation（`_foveatedFactor` / `priorityDeferred`）
- `dynamicScreenSpaceError`（雾因子衰减）
- `skipLevelOfDetail`（跳过中间层级直载深层）
- `cullRequestsWhileMoving`（出视锥请求取消）
- 优先级队列排序（当前按遍历顺序加载）
- LRU 淘汰与内存预算（当前只加载不淘汰；该数据集 373 个 b3dm，规模可控）

### 16.5 加载与场景同步

- **同步加载**：`FileAccess` 读盘 + `GLTFDocument` 装配都在主线程。用 `maximum_simultaneous_loads`（默认 8）限制每帧启动数，避免冷启动卡死单帧
- **失败重试**：单瓦片失败重试 3 次后标记 `ContentState::Failed`，避免永久空白，也避免每帧重试刷屏
- **可见性同步**：遍历前清空 `render_list`；同步时先隐藏 `loaded_tiles` 全部节点，再显示 `render_list` 中已就绪的。**按已加载列表而非瓦片树遍历**，开销与实挂载数成正比
- **worldMatrix 每帧重贴**：内容挂载后仍每帧 `set_transform`，这样 georeference 或本节点移动后内容不会错位

---

## 17. 阻断项：`KHR_draco_mesh_compression`（Spike S1 门槛 3 失败）

### 17.1 事实

实测数据集（`D:\GISData\3D Tiles\1.0\Photogrammetry`，373 个 b3dm / 压缩后 38.3 MB）：

```json
"extensionsUsed":     ["KHR_materials_unlit","KHR_draco_mesh_compression"]
"extensionsRequired": ["KHR_materials_unlit","KHR_draco_mesh_compression"]
```

两个都是**必需**扩展，所以严格加载器整份拒绝。运行时报错：

```
ERROR: glTF: Can't import file '', required extension 'KHR_draco_mesh_compression'
       is not supported. Are you missing a GLTFDocumentExtension plugin?
```

**Godot 的核心 glTF 导入器不实现该扩展**（[godot#73738](https://github.com/godotengine/godot/issues/73738)）——不是构建开关、不是编辑器/运行时差异，就是没实现。引擎在 4.3+ 提供了 `GLTFDocumentExtension` 扩展点（报错信息正是在邀请实现一个），但**官方无实现**。社区有第三方 GDExtension（`GDDraco`，MIT，包 Google Draco SDK 1.5.7），但它是第三方、且声明只测过 Godot 4.5，不适合作为产品依赖。

**其余全通。** 日志证明整条链路正确：`loaded ... tiles=374 maxDepth=8 rootGE=777.242 georeferenced=yes` → 遍历 → 内容路径解析到 `0/0.b3dm` → b3dm 解析成功 → 调用 `GLTFDocument` 才失败。**唯一断点是 Draco。**

### 17.2 更正：D-2 的论证有两条要改

**（a）「零新增原生依赖」这条不成立。** §16.2 把它列为选 `GLTFDocument` 的理由之一——但 Draco 是**几何压缩**，无论走 `GLTFDocument` 还是自建 `ArrayMesh`，**都必须自己解码**。方案 B 一样要引入 Draco SDK。所以这条理由无效。

**（b）「门槛 3 失败 → 改选 B」这个裁决规则是错的。** §5.S1.5 写着 "1 或 3 失败 → 改选 B（自建 `ArrayMesh`）"。但 Draco 与装配路线**正交**：换 B 并不解决解码问题，只是把同一件事换到另一条路上去做，还额外丢掉 `GLTFDocument` 的材质/sampler/sRGB/上轴处理。

**D-2 结论本身不变**（仍应选 `GLTFDocument`），但理由收窄为三条：① 它已覆盖原 1306 行手工装配要解决的问题；② 产出标准 `Node3D` 直接接入渲染管线；③ 主线程耗时可用每帧预算处理。**「零依赖」这条划掉。**

### 17.3 三条可选路线

| 路线 | 做法 | 代价 | 结果 |
|---|---|---|---|
| **A. 自己实现 Draco 解码扩展** | 链接 Google Draco SDK，实现 `GLTFDocumentExtension` 处理 `KHR_draco_mesh_compression`，用 `GLTFDocument::register_gltf_document_extension()` 注册 | 大：新增原生依赖 + 属性映射/图元重建逻辑；Godot 自带实现可作参考但要移植 | **长期正确**，插件从此能吃真实数据（Cesium ion 产出普遍带 Draco） |
| **B. 离线把数据集转成非 Draco** | 用带 Draco 解码器的工具（`gltf-transform` / `3d-tiles-tools` / `draco3d`）逐瓦片解码重写 | 中：需写一个 b3dm 拆包→转码→回包的工具；38 MB 膨胀若干倍；**会改动用户数据集，必须写到新目录** | 今天就能看到画面，但**插件仍不能处理真实 Draco 数据**（能力缺口仍在） |
| **C. 先拿小型非 Draco 样例集验证管线** | 下载 Cesium `3d-tiles-samples` 里不含 Draco 的小样例，指向它 | 小 | 把"管线本身是否正确"与"Draco 是否支持"**解耦**，最小代价确认前者的正确性 |

**建议顺序：C → A。** C 的代价极小，且能立刻回答"我们的解析/遍历/装配到底对不对"这个当前无法区分的问题；A 是绕不过去的，早晚要做。B 只适合作为临时手段，且要单独评估是否值得改用户数据集。

### 17.4 需要连带修正的计划条目

- §2.D2 的「倾向 `GLTFDocument`」理由中删除"零新增原生依赖"，改为"需另行解决 Draco"
- §5.S1.5 裁决表第 "1 或 3" 行的结论从「改选 B」改为「Draco 作为独立工作流处理，与装配路线无关」
- Phase 0.5（Spike S1）新增一项验证目标：**Draco 解码能力的引入方式**（自己实现 vs 依赖第三方）
- §9 完成定义第 3 条「原生依赖仅剩 godot-cpp + glm + nlohmann/json」需追加说明 Draco SDK 为例外

### 17.5 已实施：转码路线（第 17.3 节的 A 与 B 的合体）

用户决策：**用现成的三方库做 Draco 解码**（Google Draco SDK），**仍装配成 `MeshInstance3D`**。落地方式如下。

**关键判断**：Godot 的 `GLTFDocumentExtension` 只暴露 `_get_supported_extensions` / `_parse_node_extensions` / `_generate_scene_node` / `_import_node` / `_import_post`，**没有网格或图元级钩子**——所以 Draco 扩展必须在 `_generate_scene_node()` 里自己建 `Mesh`+`MeshInstance3D`，且会绕过 Godot 的材质/sampler/色彩空间处理。

**因此改为"容器重写"**：在把字节交给 `GLTFDocument` 之前，把 Draco 压缩的图元**解码并写回成普通 accessor**，再交给 `GLTFDocument`。这与 three.js 的 `DRACOLoader` 是同一种形态（它也是在构建几何之前把图元属性替换成解码后的数据），并且：

- **装配路径仍然只有一条**（`GLTFDocument`），产出仍是标准 `MeshInstance3D` ✓ 满足用户"用 MeshInstance 装配"的要求
- 材质、贴图、sampler、sRGB 全部免费保留（`KHR_materials_unlit` Godot 本来就支持）
- 与引擎无关，放在 `src/core/content/`，可单元测试

**交付**：

| 文件 | 内容 |
|---|---|
| `src/core/content/DracoTranscoder.{h,cpp}` | `requiresDracoDecoding()` 与 `transcodeDracoToPlainGltf()`：解析 GLB 容器 → 逐图元 Draco 解码 → 把解码结果追加进 BIN chunk 并重指 accessor → 从 `extensionsUsed`/`extensionsRequired` 移除该扩展 → 重新序列化 GLB |
| `extern/third_party/CMakeLists.txt` | `add_subdirectory(draco)` + 9 个必须显式关闭的选项（见 17.6），并把 `draco::draco` 挂到 `tiles3d_third_party` INTERFACE |
| `src/ContentFactory.cpp` | 提取 GLB 后**无条件**调用转码（非 Draco 的 glTF 原样透传，故可无条件跑） |
| `extern/third_party/draco/` | vendored Google Draco 1.5.7，裁剪后 **3.05 MB / 516 文件**（Apache-2.0） |

**实现细节（值得记下的三条）**：

1. **属性用 `PointAttribute::GetValue<float>()` 而不是 `GetValue<char>()` 之类**——它的模板参数是**输出**类型，内部会做 int→float 转换，所以量化过的法线/UV 也能正确读出 float，不需要自己处理 `data_type()` 分支。
2. **顶点按"每 point 一条"展开**：`attribute->mapped_index(PointIndex(p))` 取属性值下标，面索引直接用 `mesh->face(f)[k].value()`（point 下标）。因为顶点数组是每 point 一条，所以 point 下标即顶点下标 ✓。
3. **索引强制 `UNSIGNED_INT`(5125)**，避免 Draco 的顶点数跨过 uint16 边界。

**未做（可后续优化）**：解码后的原压缩 bufferView 保留为死数据（删除会重排后续所有 view 下标）；未做 accessor 级去重与 `min`/`max` 重算（沿用原值，`POSITION` 的 min/max 因此仍是 Draco 前的值——数值上等价，因为解码是精确的）。

### 17.6 Draco 的 CMake 选项（默认值会让 configure 直接失败）

| 选项 | 值 | 不改的后果 |
|---|---|---|
| `DRACO_TRANSCODER_SUPPORTED` | OFF | 需要 Eigen / filesystem / tinygltf 三个子模块，裁剪后不存在 → `draco_die_missing_submodule` 直接 `FATAL_ERROR` |
| `DRACO_TESTS` | OFF | 需要 googletest 子模块，同上 |
| `DRACO_INSTALL` | OFF | 会把安装规则注册进顶层 install（与当初 doctest/glm/nlohmann 的泄漏同类） |
| `DRACO_GLTF` / `DRACO_GLTF_BITSTREAM` | OFF | 无关目标 |
| `DRACO_MAYA_PLUGIN` / `DRACO_UNITY_PLUGIN` / `DRACO_JS_GLUE` | OFF | 无关目标 |
| `DRACO_VERBOSE` | OFF | 噪音 |

`add_subdirectory` **必须给出独立的 binary 目录**——Draco 拒绝在源码树内配置（`draco_root == draco_build` 时 `FATAL_ERROR`）。

原生库目标名是 **`draco_static`**（或 `draco_shared`），并用别名 **`draco::draco`** 统一引用（`CMakeLists.txt:1050/1053`）。

---

### 17.7 D-2 反转（D-13）：GLTFDocument → 自建装配（2026-09-17）

**触发**：真实数据集（0/0.b3dm）在编辑器进程里加载后 `meshInstances=0`，屏幕无几何。逐层排查（读 4.7.2 引擎源码核实）：

1. **根因**：Godot 的 glTF 导入器把网格节点生成为 `ImporterMeshInstance3D`（一个**不渲染的 Node3D 占位**），转换成 `MeshInstance3D` 由 `GLTFDocumentExtensionConvertImporterMesh::import_post` 完成。而 `modules/gltf/register_types.cpp` 用 `is_editor_hint()` 门控注册该扩展——**编辑器进程内永远不转换**。`generate_scene` 末尾只对"根节点本身是 ImporterMeshInstance3D"的情况做兜底转换，内部节点不做。编辑器里跑 Tileset3D（@tool 预览）拿到的就是占位节点，`countMeshInstances` 因此为 0。
2. **次要摩擦**：内嵌图片在编辑器里走 `ResourceImporterTexture` 重导入管线（`_parse_image_save_image`），basePath 在项目外时产生 `Can't find file ... during reimport` 错误 + "uncompressed" 警告噪音（非致命）。
3. **结构性成本**：为让 GLTFDocument 吃 Draco，我们维护了整个"GLB 重序列化"层（追加 BIN、重指 accessor、重写容器 ~200 行），而 GltfReader 本来就要解析这些 JSON；主线程还要承担 `append_from_buffer`/`generate_scene` 的完整导入管线开销。

**决策（D-13）**：走 §2.D2 的选项 B。`GLTFDocument`/`DracoTranscoder` 整条路径删除，新增：

| 文件 | 内容 |
|---|---|
| `src/core/content/GltfReader.{h,cpp}` | 引擎无关：GLB 容器解析、typed accessor 读取（componentType 5120–5126 / normalized / byteStride）、**Draco 直接解码为 SoA 顶点数据**（不再重序列化 GLB）、材质/纹理/采样器/场景图（matrix 与 TRS）、TRIANGLE_STRIP/FAN 展开、索引边界校验；不支持的能力（sparse、外部 buffer/图片 URI、meshopt/basisu）**报名失败**而非静默错绘 |
| `src/ContentFactory.cpp`（重写） | 遍历 GltfModel 手工组装：每图元一个 `ArrayMesh` surface（SoA 直拷进 Packed 数组）、每 glTF 材质一个共享 `StandardMaterial3D`（unlit→UNSHADED、alphaMode→透明度模式、doubleSided→CULL_DISABLED、sampler filter/repeat 映射，4.7 的 wrap 是 `FLAG_USE_TEXTURE_REPEAT` bool flag）、内嵌图片 `load_jpg/png_from_buffer`→`ImageTexture`（mipmap 按采样器生成）；点亮材质缺 NORMAL 时按老 `computeFlatNormals` 路径展开平直法线 |

**换来的**：编辑器/运行时行为完全一致（无 is_editor_hint 分叉）、无图片重导入噪音、删掉 GLB 重序列化层、Draco 解码直连装配（少一次全量拷贝）、逐图元行为全部可测。

**代价（接受）**：材质只覆盖摄影测量子集（baseColorTexture/Factor、KHR_materials_unlit、alphaMode、doubleSided、sampler filter/wrap）；不支持 meshopt/basisu/sparse（报错即止）。这些正是本数据集与 Cesium ion 摄影测量产物的实际集合；扩大覆盖时在 `GltfReader` 单点加。

---

*本计划基于对两个仓库的完整源码阅读：`godot-3dtiles/src` 全量逐函数核对 + 参考实现 `index.ts` 结构映射 + 6 个 spec 文件 + 2 份设计文档；并在本机实测了工具链、版本、网络可达性与编译测试（§11–§17）。*
