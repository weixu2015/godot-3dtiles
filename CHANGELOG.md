# Changelog

本项目遵循 [语义化版本](https://semver.org/lang/zh-CN/)，
变更记录格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)。

## [0.1.0] - 2026-10-03

首个里程碑版本：**一个可用的、不依赖 Cesium 的 Godot 3D Tiles 加载器。**

面向单数据集、小范围（城市 / 园区 / 工程级）的数字孪生场景。

### 新增

- **`Tileset3D` 节点** —— 加载 `tileset.json`，按屏幕空间误差（SSE）细化遍历，
  驱动内容加载与渲染，并提供深度着色的包围盒调试线框。
- **`Georeference3D` 节点** —— 显式地理参考帧，供多个 `Tileset3D` 共享，
  使多个数据集按真实经纬度相对摆放。
- **`OriginAuthority` 体系** —— `LongitudeLatitudeHeight` / `EarthCenteredEarthFixed`
  两种原点来源。
- **引擎无关内核 `tiles3d_core`** —— 纯 C++20 静态库，不依赖 Godot，
  可脱离引擎单测（`math/`、`tiles/`、`content/`）。
- **3D Tiles 1.1 隐式瓦片** —— `.subtree` 二进制解码、Morton 索引寻址、
  按需展开隐式瓦片。
- **自建 glTF 内容管线** —— 替代 `GLTFDocument`，手工组装
  `ArrayMesh` + `StandardMaterial3D` + `ImageTexture`，
  编辑器与运行时行为完全一致。
- **`KHR_draco_mesh_compression` 支持** —— vendored Google Draco 1.5.7，
  在 `GltfReader` 内直接解码为 SoA 顶点数据。
- **`KHR_texture_basisu`（KTX2）支持** —— 自持 basisu transcoder + zstd，
  进入 Godot 前转为 RGBA8。
- **内容上轴校正** —— 读取 `asset.gltfUpAxis`，正确处理 `X` / `Y` / `Z` 三种声明。
- **调试工具** —— `demo/dataset_audit.tscn` 全数据集鲁棒性审计；
  `demo/implicit_audit.tscn` 单数据集渲染验证；
  `demo/addons/viewport_hud/` 编辑器 3D 视口实时参数面板（FPS / 相机位姿 / 投影参数）。

### 变更

- 从零实现 3D Tiles 规范，**移除 `cesium-native` 依赖**，
  原 Cesium 耦合的 19 个源文件（约 5800 行）被替换为自研内核。
- glTF 内容装配路线改为自建（原方案依赖 `GLTFDocument`），
  原因是 Godot 的 `GLTFDocumentExtensionConvertImporterMesh` 在编辑器进程内不注册，
  导致 `@tool` 预览拿到的全是不渲染的占位节点。
- 隐式地理参考的判定改为**声明式**：仅当根节点有 `region` 包围体、
  或声明了 `transform` 时才构建 ENU 帧，否则使用 `inverse(rootTransform)`。
  （此前的数值启发式会把部分数据集误判，导致内容错位约 6370 km。）

### 修复

- `Condition "!v.is_finite()" is true` —— `eastNorthUpToFixedFrame` 在 ECEF 原点
  退化（`normalizeSafe` 返回零向量），使矩阵奇异、`invert()` 产出 NaN，
  导致每个实例的 `instance_set_transform` 失败。现已按声明判定，并为该锐边补充单测。
- 内容上轴曾被写死为 Y-up，顶点远离原点的数据集（如 taiwan）会被旋转甩到
  约 169k 单位外、超出相机远裁剪面，表现为「整场景全黑但报告已加载」。
- 外部 tileset（`content.uri` 指向另一个 `tileset.json`）展开。

### 已知问题

- `tests/test_gltf_reader.cpp` 有 6 个用例 / 7 个断言长期失败（历史遗留，
  与当前功能无关）。判断改动是否引入回归时需与该基线对照。
- 数据集 `E:/GISData/3D Tiles/Aerometrex-SanFrancisco-2cm` 本身损坏
  （根 content URI 指向目录、深层引用的外部 tileset 整棵树缺失），无法渲染。

### 不在范围内

`pnts` / `i3dm` / `cmpt` 容器、3D Tiles 样式引擎、全局优先级请求调度、
瓦片元数据（`EXT_structural_metadata` 等）的完整解析。

`Globe3D`（椭球地表 + 影像四叉树 LOD 数字地球）不在主分支，
基础实现见 `feat/globe` 分支。
