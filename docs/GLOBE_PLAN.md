# globe 节点实现计划

> 分支：`feat/globe`
> 目标：在 Godot 里实现一个 `Globe3D` 节点（虚拟地球），3D Tiles 可以按真实经纬度落在椭球面上，
> 效果对齐前端 cesiumjs 版 `web-spatial-examples/apps/main/src/views/globe`。

---

## 0. 结论摘要

| 项 | 结论 |
|---|---|
| 新节点 | `Globe3D`（椭球外观 + 四叉树地表 LOD + 大气）。**不承担坐标职责** |
| 节点关系 | `Globe3D` 与 `Tileset3D` **同级并列**，二者都向上解析同一个 `Georeference3D`（见 §3） |
| `Tileset3D` 改动 | **零改动**。不需要知道 `Globe3D` 存在，也不需要新增解析分支 |
| 精度方案 | **Origin Shift 单模式**，`rebase_threshold` 一个参数控制刷新频率（见 §2） |
| 关键新增 | `core/ellipsoid/GlobeQuadtree`（ECT 瓦片方案）、`core/math/EllipsoidalOccluder`（地平线剔除） |
| 坐标分工 | `Tileset3D` 走 ENU 切平面（城市级）；`Globe3D` 地表直接 ECEF 直算（全球）——见 §3.3 |
| 分期 | P0 静态椭球 → P3 最小相机 → P1 四叉树地表 → P2 3D Tiles 落位 → P4 大气 → P5 GlobeSubScene |


---

## 1. 参考实现盘点（前端 cesiumjs 版）

参考目录：`D:\Develop\jsspace\web-spatial-examples\apps\main\src\views\globe`

| 文件 | 行数 | 职责 | 对 Godot 的可复用度 |
|---|---|---|---|
| `renderers/globe3d/tileScheme.ts` | 191 | ECT 瓦片方案、quadkey、**地平线剔除** | **高**，纯数学，可 1:1 移植 |
| `renderers/globe3d/QuadtreeTile.ts` | 190 | 瓦片数据模型、包围球、LRU 队列 | **高**，数据模型与引擎无关 |
| `renderers/globe3d/atmosphere.ts` | 520 | 大气单次散射 GLSL、太阳精灵、昼夜注入 | **中**，GLSL 需改写为 Godot Shader |
| `renderers/QuadtreeGlobe.ts` | 1244 | 选择遍历 / 加载队列 / 相机交互 / HUD | **中**，逻辑可移植，Three.js API 需替换 |
| `renderers/FullGlobe.ts` | 221 | 在 Quadtree 上加大气/太阳/天空盒 | **中**，作为 P4 的设计参考 |
| `threeDTiles/index.ts` | 3471 | 3D Tiles 调度器 | **低**，你已在 C++ 里自研了等价内核 |

### 1.1 参考实现的核心算法（必须移植）

**ECT 瓦片方案**（`tileScheme.ts`）—— 不是 Web Mercator，是等距圆柱：

```
level L 有 2^L × 2^L 个瓦片
tileXYToRectangle(x, y, level):
  size  = 2π / 2^L
  west  = -π + x * size
  east  = west + size
  north = latitudeFromMercatorY(y / n)      // Mercator 纬度映射
  south = latitudeFromMercatorY((y+1) / n)
```

**几何纬度扩展**（极冠处理）：y=0 行向北扩到 +90°，y=n-1 行向南扩到 -90°，因为 Mercator 只在 ±85.0511° 内有瓦片。

**纬度 → ECEF**（`cartographicToXYZ`，**注意 Y 为极轴**）：

```
x = (A + h) * cos(lat) * cos(lon)
y = (C + h) * sin(lat)
z = -(B + h) * cos(lat) * sin(lon)      // 负号：右手系绕 +Y 极轴，否则东西镜像
```

**SSE**（`screenSpaceError`）：

```
maxGeometricError = GE_LEVEL0 / 2^level
distance          = max(|camera - sphere.center| - sphere.radius, 1.0)
sse               = maxGeometricError * height / (distance * 2*tan(fov/2))
```

其中 `GE_LEVEL0 = ((max(A,C) * 2π) / 4) / 65`。

**地平线剔除**（`computeHorizonCullingPointFromRectangle` + `isScaledSpacePointVisible`）—— `EllipsoidalOccluder` 的忠实移植，把点除以椭球半径缩放到单位球空间后判断遮挡。这是"背面瓦片不加载"的关键，**必须移植**，否则地球背面一半的瓦片会全部进渲染列表。

**纹理回退**（`findAppearanceSource` + `updateTileAppearance`）：子瓦片纹理未就绪时沿祖先链找最近 `DONE` 的纹理，对基础 UV 做仿射变换映射到子矩形。这是"不出现空洞"的关键。

---

## 2. 精度方案：Origin Shift 单模式

### 2.1 决策

**单一机制，一个参数控制刷新频率**。反对"两套并存"，因为二者精度本质完全相同。

```
R = localToEcef⁻¹                       // double，原点可动
node.transform = toGodotTransform( R · worldMatrix_double )   // float32，唯一收窄点
```

- `rebase_threshold`（米）设为 `1000`（默认，对应 0.12 mm 量化，人眼无感）→ Cesium 的 Origin Shift 行为
- `rebase_threshold = 0` → 每帧 re-base → kimi 建议的"相机相对渲染"行为

同一套代码，一个属性。可实测对比后再定默认值。

### 2.2 为什么不需要"顶点烘焙"作为前提

`final = wrapper(float32) × vertex(float32)` 的组合误差：

| 场景 | float32 相对精度 | 绝对误差 |
|---|---|---|
| 原生路径，wrapper 在 R 系（原点靠近相机） | 1.2e-7 | 可取 |
| 未修正路径，wrapper 在 ECEF 量级 | 1.2e-7 | **约 0.76 m（抖散）** |

即：**只要 wrapper 落在 R 系（原点被 re-base 到相机附近），顶点自身的 float32 就是可接受的**，无需把顶点烘焙到瓦片中心（那是另一个独立优化，不是前提）。第 0 步已有一条 opengl 零成本验证：Godot 的 opengl 渲染后端支持 `RenderingServer::MESH_VERTEX` 提交 `real_t`（single 构建下即 float），不需要改引擎。

### 2.3 Godot 侧 re-base 的具体落点

现有代码里三处需要动：

| 位置 | 现状 | 改动 |
|---|---|---|
| `Georeference3D::refresh()` | 清缓存 + 发信号 | 复用，re-base 时改写 origin 后调用 |
| `Tileset3D::sync_content_visibility()` | 每帧 `set_transform( toGodotTransform( *tile->worldMatrix ) )` | 顺带减掉新原点平移（矩阵同型，只动 `[3]` 列） |
| `Tileset3D::current_view()` | `to_local( camera->get_global_transform().origin )` | 相机位置同步减去旧原点平移 |

**关键约束**：re-base 必须在同一帧内"先算完 traversal、再在 sync 阶段统一平移"，否则 `render_list` 用旧坐标系、内容节点用新坐标系，会闪一帧。

---

## 3. 节点设计

### 3.1 核心决策：`Globe3D` 不承担坐标职责

**相对位置由 georeference 唯一决定，与"哪个节点提供它"无关。**

```cpp
// Tileset3D::compute_model_matrix() —— 只做一件事：ECEF -> R
if ( const Georeference3D *reference = find_georeference(); reference != nullptr )
    return reference->ecef_to_local();
```

`find_georeference()` 返回的只是"ECEF→R 这个变换矩阵"的提供者。只要两个 `Tileset3D` 拿到**同一个**
`ecef_to_local()`，它们的瓦片必然落在同一坐标系，相对位置必然正确。`Globe3D` 在这条链路上
**不参与任何几何计算**。

因此节点关系是**同级并列**，不是父子包含：

```
Node3D
├── Georeference3D               # 【唯一坐标系原点】可选
│   ├── Globe3D                  #   椭球地表（纯外观，向上解析帧）
│   ├── Tileset3D                #   3D Tiles（Photogrammetry）
│   └── Tileset3D                #   3D Tiles（taiwan）
├── Tileset3D                    # 【单独用】隐式自带 georeference（现状完全不变）
└── Camera3D + GlobeCameraController

或极简（不需要 globe 时）：
Node3D
├── Tileset3D                    # 现状用法，不引入任何 globe 相关节点
└── Camera3D
```

| 用法 | 节点结构 | 说明 |
|---|---|---|
| ① 只要 3D Tiles | `Tileset3D` 单独 | 现状不变 |
| ② 多 tileset 共享帧 | `Georeference3D` → 多个 `Tileset3D` | 现状不变 |
| ③ 虚拟地球 + 落位 | `Georeference3D` → { `Globe3D`, `Tileset3D`... } | 三者同级，共享同一帧 |

**为什么不让 `Globe3D` 内部持有 ENU 帧**：那会制造一个静默错误 —— 用户只拖了一个 `Globe3D`
却把 `Tileset3D` 放在它外面时，两者会各自算一个帧，导致地表与 3D Tiles 静默错位。用
`Georeference3D` 做**唯一**的帧提供者，从结构上排除该错误。

### 3.2 `Globe3D` 自身结构

```
Globe3D (Node3D)
├── 椭球（P0）
│   ├── 程序化生成球面网格（经纬细分）
│   └── 材质：纯色 / 等距柱状纹理 / 网格线
├── 地表四叉树（P1）
│   └── GlobeQuadtree 调度 + 瓦片 MeshInstance3D
└── 大气与太阳（P4）
    ├── Environment / 体积雾（Godot 原生方案，不做独立大气壳 mesh）
    └── DirectionalLight3D
```

**`Globe3D` 的解析规则**（与 `Tileset3D` 完全同构，照搬现有语义）：
1. 向上找 `Georeference3D` → 用它；
2. 找不到 → 自建一个等价帧（允许单独拖一个 `Globe3D` 也能看到地球，纯便利，非必需）。

**相机控制器（P3）挂在 `Camera3D` 上，不挂在 `Globe3D` 上。** 与 Cesium for Unity 的
`CesiumOriginShift` 一致 —— shift 的驱动者是**相机**，不是地球。

### 3.3 坐标分工边界（关键，决定哪一层用什么坐标）

`Georeference3D` 的 ENU 帧是**原点处的切平面**（`eastNorthUpToFixedFrame`），对球面是近似。
偏差随离原点距离平方增长：

| 离原点距离 | 切平面 vs 球面偏差 |
|---|---|
| 10 km | 7.8 m |
| 100 km | 785 m |
| 1000 km | 78 km |
| 5000 km | 完全不可用 |

因此**分工如下**：

| | 坐标表达 | 适用尺度 |
|---|---|---|
| `Tileset3D`（3D Tiles 数据集） | ENU 切平面（`Georeference3D`） | 城市级（≤ 几十 km） |
| `Globe3D` 地表 | **ECEF 直算**（四叉树瓦片，不经 ENU 切平面） | 全球 |

**这不构成冲突**：`Georeference3D` 只是把 3D Tiles 数据集作为刚体摆在 ECEF 的某处，相机拉远时
该刚体本身不失真；失真的只是"用 ENU 帧去生成地表几何"这一件事 —— 而那正是四叉树瓦片体系
（ECT 瓦片方案）存在的理由，它的每个瓦片顶点直接在 ECEF 里算。

### 3.4 坐标系统一（关键，易错）

Godot 是 Y-up，参考实现是 **Y 为极轴** 的 ECEF。两者**恰好一致**，但 90° 翻转的账要算清楚：

| 层 | 约定 | 说明 |
|---|---|---|
| `core::math` 内核 | Z-up ECEF | 现有约定，**不动** |
| `Georeference3D` | 节点 transform 承载 Z-up→Y-up（`Transform3D(1,0,0, 0,0,1, 0,-1,0)` = **-90° 绕 X**） | 现有约定，**不动** |
| `Globe3D` 自身 | Y-up，+Y = 北极 | 与参考实现一致 |

`Globe3D` 内部构造 ENU 帧时，走 `core::math::eastNorthUpToFixedFrame`（Z-up），再套 `Georeference3D` 的翻转 —— 与现有 `Tileset3D::compute_model_matrix()` 走完全相同的路径，保证两套节点渲染出的东西朝向一致。

---

## 4. 分期计划

### P0 — 静态椭球（可验收的最小闭环） ✅ 已完成 2026-10-01

| 任务 | 文件 | 验收 | 状态 |
|---|---|---|---|
| P0.1 WGS84 椭球常量与经纬→ECEF | `src/core/math/GeoMath.{h,cpp}`（补 `kWgs84SemiMinorAxis` / `kWgs84MeanRadius` / `kWebMercatorMaxLatitude` / `geodeticToYUp`） | 单测：赤道点 x=A，极点 y=C | ✅ |
| P0.2 程序化球面网格 | `src/Globe3D.{h,cpp}`（新，SurfaceTool 三角面） | 编辑器里看到椭球 | ✅ |
| P0.3 `Globe3D` 节点骨架 | `src/Globe3D.{h,cpp}`（新） | 节点可拖进场景、可调细分 | ✅ |
| P0.4 等距柱状纹理材质 | `src/Globe3D.cpp`（`StandardMaterial3D` + `albedo_texture` 属性） | 贴一张世界地图纹理，方向正确 | ✅（纹理接口就绪，默认用纯色 + 经纬网线） |

> 说明：P0 实际把网格与材质都放在 `src/Globe3D.{h,cpp}` 内（小型 GDExtension 插件沿用"头与实现同目录"惯例），未拆出独立的 `src/Globe/` 子目录与 `GlobeMesh`/`GlobeMaterial`。P1 的四叉树调度器落地时若体量变大，再考虑拆目录。

**P0 验收标准**：编辑器里能看到一个方向正确的椭球（赤道线水平、0° 经线在正确位置），纹理不镜像。

**P0 实测结果**：
- 单测新增 `tests/test_tile_scheme.cpp`、`tests/test_ellipsoidal_occluder.cpp` 共 15 例全过。
- Godot 4.7.2 无头加载扩展零报错。
- 几何审计（`demo/capture_globe.tscn`）10/10：椭球居中原点、南北极 ±C（Y-up）、赤道 +A、`lon+90 → -Z`（不镜像）。
- RTX 2060 实机截图确认：球体 + 经纬网线方向正确。

### P1 — 地表四叉树 LOD ✅ 已完成（2026-10-01）

| 任务 | 文件 | 验收 |
|---|---|---|
| P1.1 ECT 瓦片方案 | `src/core/math/TileScheme.{h,cpp}`（新） | 单测：`tileXYToRectangle` 与 TS 逐字段一致 |
| P1.2 地平线剔除 | `src/core/math/EllipsoidalOccluder.{h,cpp}`（新） | 单测：`isScaledSpacePointVisible` 与 TS 一致 |
| P1.3 瓦片数据模型 | `src/GlobeTile.{h,cpp}`（新） | 包围球、occludeePoint 懒计算 |
| P1.4 节点与遍历 | `src/GlobeTileLayer.{h,cpp}`（新，`Node3D`） | 编辑器 + 运行时都渲染出地表 |
| P1.5 瓦片网格与纹理 | 同上 | 瓦片拼接无裂缝、极冠不破 |
| P1.6 纹理回退 | 同上 | 快速推拉无空洞 |

**P1 验收标准**：
1. 相机在 1000 km 高空，渲染层级落在地表细节合适的一级
2. 相机绕到地球背面时，`stats.culled` 计数显著增长（证明地平线剔除生效）
3. 相机从太空推到地面，瓦片逐级细化且不出现持续空洞
4. 与 TS 参考实现同相机位姿 trace-diff（可自动化）

**实测结果**（`demo/globe.tscn`，RTX 2060 实机）：
- 影像源对齐参考页的本地 Bing 缓存：`http://127.0.0.1:9090/tiles/bing/{q}.jpeg?n=z&g=11404`
  （参考页走 Vite 代理 `/api → http://localhost:9090`，这里直连同源静态缓存，省掉代理一层）。
  `maximum_level_` 取 16（Bing 服务上限 19，但本机缓存 17 级以上为空，取 16 免 404 风暴）。
- 运行窗口 `demo/globe_capture.tscn`：`rendered=82 loading=9 cached=536 max_level=6`；
  编辑器窗口（615 px 视口）：`rendered=16`，球体、海陆、大气均正常。
- 编辑器可见性（踩坑记录，**不是** Godot 的相机限制，是三件事叠加）：

  | 症状 | 根因 | 修法 |
  |---|---|---|
  | 编辑器里完全看不到地球 | `Tileset3D::frame_camera()` 按**数据集尺度**（台湾数据集半径约 1.2 km）把编辑器相机摆到 `(0, 499, 1248)`，这在北京时间 `Georeference3D` 原点上方 1.3 km，地球中心在 6.4e6 m 之外，整颗星球在地平线以下 | `Tileset3D::frame_camera()` 检测同场景有无 `Globe3D`，有则直接返回（globe 场景由 globe 自己取景） |
  | 摆到正确机位仍一片黑 | 编辑器 3D 视口相机的近/远裁剪面是默认的 0.1 / **4000**，球心在 1.6e7 m 外 | `GlobeTileLayer` 新增 `manage_editor_clip`（默认开），按相机到椭球中心的距离**每帧**重设 near/far |
  | 刚设好的 near/far 又被打回 0.1/4000 | 编辑器会逐帧重写 3D 视口相机的裁剪面，一次赋值撑不过一帧 | 同上，改成每帧维护；取景位姿反倒只需保持几帧（`needs_framing_` + `kFramingFrames`） |
  | 编辑器相机崩溃时 `Condition "det == 0"` | 视口相机在编辑器建相机的一瞬间基是零矩阵，`affine_inverse()` 出 NaN 视锥把整帧瓦片全剔掉 | `compute_frustum()` 先查行列式，退化时放过一个视锥 |

  > 结论：编辑器里跑 `_process` 完全正常（Tileset3D 在编辑器里打了 3500+ 帧 LOD），
  > `EditorInterface::get_editor_viewport_3d()->get_camera_3d()` 拿到的相机可写、
  > `Viewport` 的 camera-override API 虽然引擎源码里有（`viewport.cpp:4319`），
  > 但 godot-cpp 没绑定，改不了，只能用 `set_global_transform` + `set_near/far`。
- 调试开关：`GlobeTileLayer` 的 `print_telemetry = true` 会每 3 秒打一行
  `editor / needs_framing / campos / camcenter / near / far / rendered`，
  排查"场景看着是空的"时先开它，demo 场景默认关闭。

**编辑器相机的"小世界"限制（实测，不是猜测）**

| 现象 | 实测数据 | 根因 | 处理 |
|---|---|---|---|
| Viewport Settings 里 View Z-Far 最多填 1000000 | 地球半径 6375000 m，比 UI 上限还大 | 那是**编辑器 UI 的上限**，不是引擎上限：扩展写 far=6.4e7 时能稳定生效（90k 帧遥测不掉） | `manage_editor_clip`（默认开）每帧重写，那个对话框对 globe 场景已无意义 |
| 滚轮缩放"地球大小不变" | 从 16000 km 机位发 5 个 wheel 事件，相机只移动 **1 m**（0.2 m/格） | 编辑器自由相机的**导航状态是自己的私有量**（focus/distance），滚轮步长由它算，跟我们写进去的 `set_global_transform` 无关；它学到的是"小世界"尺度 | 不用滚轮：改用 `editor_view_longitude / _latitude / _distance` 三个属性（或 `reframe_editor_view()`）确定性取景，改一下就是一次"缩放" |
| 滚轮"缩放过程中地球隐藏了" | 导航期间编辑器把相机 near/far 写成 **0.1 / 4000**，持续整个惯性拖尾（≈20+ 帧），far=4000 直接切掉 6.4e6 m 外的星球 | 编辑器在导航帧把裁剪面写回自己的预设，且写在我们的 `_process` 之后 | 改到 `RenderingServer.frame_pre_draw` 里再断言一次（该钩子在所有 Node 的 `_process` 之后、绘制之前），修复后滚轮全程维持 near=1600 / far=6.4e7 |
| 想用 camera override 彻底接管编辑器视口 | `ClassDB.class_has_method("Viewport","enable_camera_3d_override")` = **false** | 该 API 没进 `_bind_methods`，脚本层（含 `Object::call`）完全不可达 | 放弃这条路；编辑器的定位就是"检查摆放"，要交互导航就跑场景用 `GlobeCameraController` |
| far 公式 | 相机在椭球内时 `4d` 会切掉对侧地表 | 需要 `far ≥ d + R` 而不是 `k·d` | `far = max(4d, d + 1.5R)`，对任何球外机位 4d 项占优、深度比不变 |

> 结论：**编辑器里能"看"，不能"飞"**。摆放检查用属性取景，交互浏览用运行窗口。
> 顺便记一笔：编辑器恢复的是上次的活动标签页（`.godot/editor/editor_layout.cfg` 的
> `current_scene`），只传场景路径不改那个文件的话，截图可能拍到别的场景。

### P2 — 3D Tiles 落位（核心价值）

| 任务 | 文件 | 验收 |
|---|---|---|
| P2.1 动态原点（`rebase_threshold` 属性） | `Georeference3D` + `Tileset3D` | 相机移动 10 km 后坐标仍在小量级 |
| P2.2 `Globe3D` 向上解析帧 | `Globe3D::find_georeference()`（与 `Tileset3D` 同构） | 与同级 `Tileset3D` 用同一帧 |
| P2.3 经纬度定位 API | `Globe3D::local_to_ecef()` / `ecef_to_local()`（转发给帧提供者） | GDScript 可按经纬度放节点 |
| P2.4 落位验证 | demo 场景 | 数据集出现在正确经纬度、朝向正确 |

**P2 验收标准**：
1. 加载 taiwan / Photogrammetry 数据集，位置与 Cesium 参考页一致（截图对比）
2. 相机在地表附近漫游 50 km，模型不抖动（对比：把 `rebase_threshold` 设为极大值时应复盘抖动）
3. 多个 tileset 同时落位，相对位置正确
4. `Globe3D` 与 `Tileset3D` 同级摆放时，地表与数据集对齐（证明共享帧成立）

### P3 — 相机与交互

> **状态：✅ 已完成 2026-10-01**（`camera_p3.tscn` 审计 10/10 ALL PASS）

| 任务 | 文件 | 验收 |
|---|---|---|
| P3.1 `GlobeCameraController` | `src/GlobeCameraController.{h,cpp}`（新，**挂在 `Camera3D` 上**） | 左键轨道旋转、右键 tilt、滚轮缩放惯性 |
| P3.2 相机不穿地 | 同上 | `enforce_camera_above_ellipsoid`（贴地 1 m 钳制） |
| P3.3 相机位姿 API | 同上 | `set_camera_pose` / `get_camera_direction` / `get_camera_up` / `orbit_to` |
| P3.4 与 Godot 原生相机共存 | 同上 | 继承 `Camera3D`，`_unhandled_input` 不与 UI 抢事件 |

**P3 验收标准**：交互手感与 Cesium 参考页一致（对比录屏）；相机贴地 1 m 时不穿模。

**实现记录（2026-10-01）**
- 实际路径为 `src/GlobeCameraController.{h,cpp}`（与 `Globe3D` 同目录，符合本项目"头实现同目录"约定），非计划中的 `src/Globe/`。
- 常量对齐参考 `QuadtreeGlobe.ts` L87–117：`kMinCameraHeight=1.0`、`kElLimit=π/2-0.01`、
  `kWheelImpulse=0.5`、`kWheelMaxLogVelocity=2.5`、`kWheelDamping=2.5`、`kInertiaSpinCoef=0.9`。
- 滚轮缩放在 **log 距离空间**积分（`wheel_log_distance_ += wheel_log_velocity_ * dt`），
  这样跨 5 个数量级的缩放手感线性，与参考一致。
- **关键 bug（已修）**：`resolve_ellipsoid_center()` 返回的是**父空间**坐标，但 `orbit_to()` 里
  用了 `to_global(pivot)` —— Godot 的 `to_global()` 把参数当**节点本地**坐标并叠加自身位置，
  导致 look_at 目标错位（`dot=0.0`）且 `set_distance` 半径漏算。正确写法是经父节点转换：
  `Object::cast_to<Node3D>(get_parent())->to_global(pivot)`。同一个坑在测试脚本里也犯了一次。
- 相机不穿地走**缩放空间**（X/Z 除 A、Y 除 C 得单位球）判定，与 `EllipsoidalOccluder` 同套数学。


### P4 — 大气与昼夜（Godot 原生方案） ✅ 已完成 2026-10-01

| 任务 | 文件 | 验收 |
|---|---|---|
| P4.1 大气 | `Globe3D` + Godot `Environment`（雾 / 体积雾） | 地平线附近有大气辉光 |
| P4.2 太阳方向与昼夜 | `Globe3D` + `DirectionalLight3D` | 晨昏线位置正确 |
| P4.3 体积云 | Godot 体积雾 / 自定义 fog shader | 太空视角可见云层 |

**决策**：**不做独立大气壳 mesh + raymarch**（参考实现的方案），也**不走 `Environment` 体积雾**。
体积雾在 Compatibility 渲染器下难以得到清晰可控的 limb glow，调试中发现它还会把整颗星球
笼罩成灰色。最终采用一个 Godot 原生 `ShaderMaterial` 实现的 **back-face glow shell**：在椭球
外面再套一层稍大的球壳，`cull_front` 只画背面，`blend_add` 做边缘辉光。这样完全在 Godot
标准管线内，无 G5 风险，且检查器可直接调颜色、强度、衰减。

**P4 验收标准**：太空视角截图与参考 `globe.html` 对比（大气厚度、晨昏线位置）。

**实现记录（2026-10-01）**
- 最终没有走 `Environment` 体积雾，而是用一个 **back-face glow shell**：`Globe3D` 在椭球面外
  再生成一层稍大的球壳，材质 `render_mode blend_add, cull_front, unshaded, depth_draw_never`。
  壳片用 `cull_front` 只保留星球背面那半层，且深度测试仍开启，因此星球会把壳的远半面遮住，
  剩下的就是从星球轮廓溢出的一圈光晕。
- 大气落进 `ALBEDO` 而不是 `EMISSION`：`unshaded` 模式下 `EMISSION` 会被忽略，这是调试中
  最隐蔽的坑（渲染完全正确，但像素全黑）。辉光公式为 `pow(1 - |N·V|, falloff)`，保证边缘最亮。
- 颜色/强度/衰减都暴露为检查器属性：`atmosphere_scale`（默认 1.06）、`atmosphere_color`
  （默认 `(0.30, 0.55, 1.0)`）、`atmosphere_intensity`（默认 2.4）、`atmosphere_falloff`
  （默认 8.0）。
- `demo/globe.tscn` 作为入口场景：黑色太空背景 + ProceduralSkyMaterial（近黑）+ 自动将太阳
  对准可见半球 + 初始距离 16 000 km，打开即能看到完整地球与蓝色大气环。

**本轮未做**：体积云（P4.3）留待后续。

### P5 — `GlobeSubScene`（预留，本轮不实现）

**问题**：re-base 时原点跳变会影响局部场景（物理、粒子、动画）。kimi 提到的
"平移瞬间物理/粒子出问题"正是此问题。

**Cesium 的解法**（值得借鉴）：不做"每帧 rebase"，而是用**局部坐标系隔离**：

```
CesiumGeoreference
├── DynamicCamera          (挂 CesiumOriginShift)
├── Global Tilesets        (全球 tileset)
└── Factory SubScene       <- 激活范围内时原点固定，Origin Shift 暂停
    ├── CesiumSubScene
    ├── Building           <- 局部对象不需要 GlobeAnchor
    └── PhysicsObjects
```

**Godot 侧设计**：`GlobeSubScene` 节点带 `activation_radius`。当相机进入其范围时，
原点固定到该子场景原点、re-base 暂停，子场景内的普通节点（含物理/粒子）不受影响；
离开范围则恢复 re-base。

**本轮不实现**，仅在 `Globe3D` 的 re-base 逻辑中预留钩子。

---

## 5. 风险与对策

| 编号 | 风险 | 影响 | 对策 |
|---|---|---|---|
| G1 | 参考实现的 **Y 为极轴** 与内核 **Z-up** 混淆 | 全场景转 90° / 镜像 | P0 先用一张标了经纬线的纹理自检；单测断言赤道/极点位置 |
| G2 | 地平线剔除遗漏 | 背面瓦片全加载，性能崩 | P1.2 单测对齐 TS；P1 验收第 2 条 |
| G3 | 地表用 ENU 切平面近似导致全球尺度失真 | 拉远后地表严重变形 | §3.3 已定：地表走 **ECEF 直算**，不经 ENU 帧 |
| G4 | Godot 的 `Camera3D::set_near()` 钳制 `MAX(near, 0.001)` | 无法用 big_space 式的 `near/scale` 手法 | 本方案不依赖 near 极小值，规避（这也是选 Origin Shift 的原因之一） |
| G5 | 大气方案的 Three.js `onBeforeCompile` 注入点在 Godot 无对应 | 昼夜光照无法注入瓦片材质 | **已定 P4 改用 Godot 原生 `ShaderMaterial` 后壳辉光**，不做独立大气壳 mesh，也不走体积雾 |
| G6 | 动态 rebase 与 `frame_camera()`（编辑器自动取景）交互 | 取景后坐标系错乱 | `frame_camera()` 后强制 re-base 一次 |
| G7 | 大地形瓦片顶点数爆炸（P1 程序化生成的瓦片网格） | 内存/带宽 | 瓦片网格按层级降细分（参考实现：`segs = clamp(64 >> min(level,4), 6, 32)`） |
| G8 | re-base 原点跳变影响局部物理/粒子 | 局部场景抖动 | P5 `GlobeSubScene` 隔离；本轮在 re-base 逻辑预留钩子 |


---

## 6. 与现有代码的复用清单

| 现有文件 | 在 globe 里的角色 | 需改动 |
|---|---|---|
| `core/math/GeoMath.{h,cpp}` | 椭球常量、经纬→ECEF、ENU 帧、region→OBB | 补极轴变体（Y-up）；`cartographicToXYZ` 的 `-sin(lon)` |
| `core/math/Mat4.{h,cpp}` | 全部矩阵运算 | 不变 |
| `core/math/BoundingVolume.{h,cpp}` | 包围体 | 不变 |
| `core/math/ScreenSpaceError.{h,cpp}` | SSE 计算 | 不变（Globe 复用同一 SSE） |
| `OriginAuthority.{h,cpp}` | 原点资源载体 | 不变（re-base 只需改写 ECEF 值） |
| `Georeference3D.{h,cpp}` | **唯一的 ENU 帧提供者** | 不变（`refresh()` 已支持原点变更） |
| `Tileset3D.{h,cpp}` | 3D Tiles 渲染 | 仅补"rebase 时平移内容节点"（§2.3）；**不新增任何 globe 相关解析** |
| `GodotMathConvert.h` | double→float 唯一收窄点 | 不变 |
| `ContentFactory.{h,cpp}` | 内容装配 | 不变 |
| `core/tiles/*` | 3D Tiles 树与调度 | 不变 |

**结论：约 90% 的内核可直接复用，新增代码集中在"椭球外观 + 四叉树地表 + 相机"三块。**
`Tileset3D` 除了 §2.3 的 re-base 平移外**不需要任何改动**。

---

## 7. 开发顺序

```
P0（静态椭球）
  └─→ P3（最小相机，P1 需要相机才能验 LOD）
        └─→ P1（四叉树地表，独立可验收）
              └─→ P2（3D Tiles 落位，核心价值）
                    └─→ P4（大气昼夜，Godot 原生）
                          └─→ P5（GlobeSubScene，预留）
```

**注意 P1 与 P3 的依赖倒置**：参考实现把相机写在 `QuadtreeGlobe` 里，但为了独立验收，
P3 的相机控制器应该先做（P1 的验收需要"以指定位姿观察地球"）。

---

## 8. 已确认项（用户决策）

| # | 项 | 决策 |
|---|---|---|
| 1 | 地表纹理 | **单图 + 瓦片服务都要**，默认单图 |
| 2 | 3D Tiles 与地表瓦片 | **不做遮挡剔除**，靠高度差自然穿插（与 cesiumjs 一致） |
| 3 | 多 globe（月球/火星） | **不需要**，椭球参数可硬编码 WGS84 |
| 4 | 大气 | **Godot 原生方案**（`Environment` + 体积雾），不做独立大气壳 mesh |
| 5 | 节点架构 | **方案 A**：`Globe3D` 与 `Tileset3D` 同级，都向上解析 `Georeference3D` |
| 6 | `GlobeSubScene` | **保留在计划中**（P5），本轮不实现 |

### 地理参考自检 + Home 按钮 + 一个待修的坐标系 bug（2026-10-02 第二轮）

**数据集"已加载但看不见"的定论**（`taiwan` 数据集）：
`root.transform` 是一个 ENU 帧，原点在 **lon 120.00000 / lat 30.00000**（整数，像转换器写死的占位值），
包围盒中心经它变换后落在 **lon 120.40568 / lat 31.07878 / h 1712.1 m**（江苏一带），
距场景锚点（台湾 120.97 / 23.5）**841 km**。所以不是渲染问题，是 geotwin 导出的 georeference 不对。

- 新增 `Tileset3D::report_georeference()`：加载后打印数据集自报经纬度与锚点距离，
  超过 `max(3 * radius, 10 km)` 就告警 `Nothing will be visible at the anchor`。
  这类故障此前与渲染 bug 完全不可区分：每个瓦片都报 loaded，屏幕一片空。
- 新增 `GeoMath::cartesianToWgs84`（Bowring，含极点短路），单测往返 6 组采样。
  用 Cesium 官方 `1.0/Photogrammetry` 数据集交叉验证：它自报 lon -75.59671 / lat 40.03880（费城）。
- 新增 5 个 getter：`get_dataset_longitude/latitude/height/radius/anchor_separation`。

**Home 按钮**（`globe.tscn` 的 HUD）：`globe.gd::home()` 按**数据集自己的声明位置**做 flyTo，
并实时显示 dataset 经纬度/半径/距锚点距离。配套 `GlobeCameraController::fly_to(lon, lat, dist, sec)`：

- 方向用 Rodrigues 球面插值，**不能**用 `Vector3.slerp`——它以 `cross(from,to)` 定轴，
  近平行或对跖时退化。台湾→费城相距 160°，实测会乱跳。
- 距离走 log 空间缓动；`is_flying()` 可查；真实输入取消飞行。
- **取消条件必须忽略零位移的 hover motion**：窗口获得焦点会发一个 delta = 0 的 motion，
  按"任意输入即取消"会让长距离飞行中途停住。中国→费城的大圆航线恰好经过西伯利亚，
  于是表现为"飞到西伯利亚去了"（实为飞了 30%~50% 就停）。已修并实测。
- 距离下限取**目标点本地地表半径 + 1 m**，不是 `min_distance()`（极半径 x 1.01 = 全球固定
  42 km 高度，会让 389 m 的数据集只剩一个点）。

**已修：相机在带 Georeference3D 的场景里落到错误经纬度**（2026-10-02 第三轮）

三个独立的 bug 叠在一起，症状都是"Home 按钮飞不到数据集"：

1. **`frame()` 解析到的帧不是 pivot/转换用的那一帧**（主因，`GlobeFrame.cpp` / `GlobeCameraController.cpp`）
   - `GlobeCameraController` 是 `Georeference3D` 的**同级兄弟**，而 `GlobeFrame::resolve()` 只走
     祖先链 → 找不到 → 回退成"局部空间 = Y-up ECEF（只带翻转，无平移）"。
     但 pivot 与 `frame_point_to_parent()` 走的是**锚点 ENU 帧**。两帧之间差一个旋转**和一个
     6370 km 平移**，所以"距离对、方向错"：实测请求费城（−75.5967, 40.0388）落到
     （144.99, 62.18）即西伯利亚，误差 8083 km。
   - 为什么距离类检查全都通过：旋转保长度，且 `resolve_ellipsoid_center()` 当时仍从
     Georeference3D 取 pivot，只有**方向**用了回退帧。
   - 修法：`GlobeFrame::from_carrier(carrier)` 从"真正承载帧的节点"构建帧（Georeference3D → 其
     ENU 矩阵；Globe3D → 它自己已解析的帧；null → 回退帧），`frame()` 改用它，
     与 `resolve_frame_node()` 同源。同时新增 `frame_point_from_parent()` 作为
     `frame_point_to_parent()` 的逆，供 `camera_height_above_ellipsoid()` /
     `enforce_camera_above_ellipsoid()` 把父空间坐标喂回帧矩阵（原先直接把父空间当帧局部，
     在 Georeference3D 带翻转 transform 时方向会差 90°）。`ellipsoid_offset()` 收敛了这两处重复计算。
   - 顺带删掉未声明就调用的 `frame_point_from_ecef()`（WIP 里编不过）。

2. **`Georeference3D` 的 transform 没写在 `globe.tscn` 里** → 场景世界是 Z-up ENU，
   与 `node_3d.tscn`（带 `Transform3D(1,0,0, 0,0,1, 0,-1,0)`）不一致，
   `look_at` 的 up、轨道仰角钳制都按 Y-up 假设。已补上同一 transform。

3. **`Globe3D::local_to_geodetic()` 返回的是地心纬度**（`atan2(z,p)` + `|v| − A` 当高度），
   不是文档承诺的 geodetic，也不是 `geodetic_to_local()` 的逆：纬度 40° 差 0.19°（约 21 km），
   高度差数百米。改为走内核的 `cartesianToWgs84()`（Bowring）。任何"把位置和数据集自报经纬度
   对比"的检查都会被它误导成落位 bug。

4. **Home 取景距离偏 8 km**（`globe.gd`）：`EARTH_A + height + 4r` 用的是赤道半径，
   而纬度 40° 处椭球面离地心比赤道**近 8 km**，于是相机停在数据集上方 ~10 km，
   数据集只剩十几个像素。改为经 Globe3D 量"地心 → 目标点"的真实距离。

**验收**：新增 `demo/globe_fly_audit.tscn`（无头可跑，真机渲染时另存截图）：

```
godot --headless --path demo globe_fly_audit.tscn
godot --path demo --rendering-driver opengl3 globe_fly_audit.tscn   # 另存 globe_fly_audit.png
```

7 项断言：相机经纬度 == 数据集自报经纬度、相机高度 == 请求取景高度、
瓦片内容落位在数据集经纬度上、内容在相机前方且落在画面中心。
**负向对照**：把 `frame()` 改回 `GlobeFrame::resolve(this)` 重编，审计立刻报
`camera longitude -63.97 / latitude 59.60`（2 项 FAIL），证明该审计确实能捕获此 bug；
修复版 ALL PASS，内核单测 84 例中 78 过 / 6 失败（与基线一致，均为既有 gltf_reader 用例）。

**注**：`globe.tscn` 的锚点与数据集已对齐（Philadelphia −75.596707 / 40.038796）；
换回 taiwan 数据集时锚点要同时改回 120.97 / 23.5，否则数据集会落在离锚点 12000 km 处，
float32 量化到 ~1 m。
