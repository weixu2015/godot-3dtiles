# Changelog

格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号与
`CMakeLists.txt` 的 `project( VERSION )` 保持一致（当前 **0.0.1**）。

> 本文件此前是上游模板 [GDExtensionTemplate](https://github.com/asmaloney/GDExtensionTemplate)
> 的历史（4.x / 2023 年的 PR 列表），与本项目无关，已改写为本项目自己的记录。

## [Unreleased]

### 新增

- **数字地球层并入主线**：`Globe3D` 椭球地表（surface / graticule / atmosphere）、
  行星级影像四叉树（ECT 瓦片方案）、轨道相机、**floating origin**（重定世界原点，
  行星尺度下亚毫米可分辨）、运行时 HUD（F3）。区域级瓦片走地表 ENU 帧，行星级走椭球面，
  两条路线共用同一个调度内核与内容管线。
- 构建预设 **`windows-editor-release`**：`GODOTCPP_TARGET=editor` + `CMAKE_BUILD_TYPE=Release`。
  godot-cpp 的"目标"与"构建类型"两条轴正交，此前只提供 `(editor, Debug)` 与
  `(template_release, Release)`，造成"一编 release 编辑器视口就不飞数据集"的困惑。
- 运行时调试与度量开关（均为环境变量，见 `docs/ARCHITECTURE.md` §5.5）：
  `TILES3D_TIMING`、`GLOBE_DATASET`、`GLOBE_PERF_SWEEP`、`ST_STATS`、`ST_MOTION`、
  `ST_LOADS`、`ST_UPLOADS`、`ST_HIDE`、`ST_SHOT`。
- `demo/single_tileset.tscn` 运行期视图：数据集下拉框 + 右上角参数面板
  （fps / 帧时间分位数 / draws / prims / 相机 / 瓦片计数）；右键绕**模型自身中心**刚性旋转
  （相机朝向与枢轴解耦，旋转不跳、不会把模型甩出视口）。
- 逐数据集 `height_offset`：摄影测量数据常以海平面为基准，与裸椭球底图之间存在大地水准面
  差距（旧金山约 −31.5 m），用逐数据集偏移补偿。

### 修复

- **模型下沉**：`height_offset` 的"上方向"原先取自 `Globe3D::ecef_to_local(Vector3.ZERO)`，
  实测该值模长 1.46e7 m（地表锚点到地心仅 6.37e6 m），归一化后是**水平方向** ——
  抬升被横向平移掉，模型纹丝不动。改用 `geodetic_to_local` 的差分求锚点处法向。
- **锚点分离量长期显示过期值**：`anchor_separation` 原为加载时快照，切数据集移动锚点后
  不复算，面板一直显示旧的"几千公里 / 不可见"警告；改为实时计算，并让数据集中心缓存与
  锚点无关的量（`dataset_center_raw_`），按当前 frame 重新表达。
- **origin shift 阈值**改为随高度缩放（`max(阈值, 高度 × 0.5)`）：固定 1000 m 时，
  从轨道降到城市要跨 1.4e7 m ⇒ 上千次重定原点，每次遍历全部已加载瓦片。
- **加载并发**默认 20 → 6：A/B 实测流式期间最差帧 18.2 ms → 13.3 ms，瓦片/秒未下降
  （每个在途请求都是与主线程抢同一批 CPU 的 worker）。
- **每帧可见性**改为差分：原先对**所有**已加载瓦片调 `set_visible(false)` 再点亮选中的。
- **`rebase`** 只给屏上内容写 `set_transform`（缓存矩阵仍全量更新）。
- `rendering_server.hpp` 的 include 移出 `TILES3D_EDITOR_TARGET` 守卫 ——
  `RenderingServer` 已用于无条件的 `disconnect_pre_draw()`，守卫导致 release 构建编不过。
- `configure_release.bat` / `configure_editor_release.bat` 复用 `build/windows-editor/_deps`
  的第三方源码：FetchContent 每个构建目录各存一份，无网络时第二次配置会卡在重 clone GLM。

## [0.0.1]

首个可运行版本：3D Tiles 1.0 / 1.1（含 `.subtree` 隐式瓦片）、Draco 网格解压、KTX2 纹理、
屏幕空间误差遍历调度、子线程取数解码 + 主线程装配的内容管线、区域级 ENU 地理参考帧、
运行时 HUD。
