# G4GO

G4GO 是一个以 Geant4 为物理基线、支持 CPU 参考传输和 CUDA/OptiX 光学传输的 MVP 工程。当前 optical backend 使用统一的 photon/event bridge，保持原有 `cellHit` 和 `pulse` ROOT ntuple 接口。

## 构建

Geant4 11.4.2、CMake 3.24 以上和 C++20 是基础依赖。

```bash
cmake -S . -B build \
  -DG4GO_BUILD_UIVIS=OFF \
  -DG4GO_ENABLE_OPTIX=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

配置阶段会自动检查 CUDA compiler、CUDA Toolkit、`bin2c` 和 vendored OptiX headers。依赖完整时，`build/g4go` 包含 OptiX 和 CPU 两套 optical transport；依赖缺失时，CMake 保留 CPU backend 并继续完成构建。

配置或构建后，`scripts/` 中的所有 `.mac` 宏会平铺复制到 `build/` 根目录，不会生成 `build/scripts/` 子目录。

可以显式关闭 OptiX：

```bash
cmake -S . -B build \
  -DG4GO_BUILD_UIVIS=OFF \
  -DG4GO_ENABLE_OPTIX=OFF
```

`G4GO_CUDA_ARCHITECTURE` 使用 CUDA 的 `compute_XX` 数值。部署到其他 GPU 时按目标 GPU 的 compute capability 设置。

## 目录结构

公开接口和实现按模块镜像组织：

```text
include/g4go/
├── simulation/
├── detector/
└── optical/

src/
├── main.cpp
├── simulation/
├── detector/
└── optical/

tests/optical/
└── CoreTest.cpp
```

`simulation` 保存 Geant4 action，`detector` 保存几何、材料和 sensitive detector，`optical` 保存场景、CPU/OptiX 传输以及 Geant4 bridge。材料和光学表面定义集中在 `DetectorConstruction.cpp` 中；模块内部辅助实现放在对应目录中，例如 Geant4 场景导出器 `SceneExporter`。

CMake 将模块编译为内部静态库：`g4go_optical_core`、`g4go_optical_cpu`、可选的 `g4go_optical_optix`、`g4go_optical_geant4`、`g4go_detector` 和 `g4go_simulation`。最终 `g4go` 目标只编译 `src/main.cpp`，依赖关系由模块库表达。

## 探测器模型

当前 `DetectorConstruction` 构造一个单晶体光学探测单元：

- 晶体尺寸为 `3 cm × 3 cm × 8 cm`，材料使用 `G4_CESIUM_IODIDE`。
- 晶体光学参数包括折射率、吸收长度、闪烁发光谱、闪烁产额和时间常数。
- 晶体前端连接硅脂耦合层和环氧窗口，耦合层与窗口沿晶体轴向放置。
- SiPM 尺寸为 `3 mm × 3 mm × 0.1 mm`，使用硅材料和表面探测效率曲线。
- 晶体表面设置 `ReflectorSurface` 皮肤表面，世界体与晶体之间设置 `CoatingSurface` 边界表面；SiPM 使用阴极皮肤表面记录光子探测效率。

晶体闪烁参数和 SiPM 探测效率当前直接定义在 `DetectorConstruction.cpp` 中。

## 运行

```bash
./build/g4go build/run_smoke.mac
./build/g4go --backend geant4 --threads 1 build/run_smoke.mac
./build/g4go --backend capture build/run_smoke.mac
./build/g4go --backend cpu build/run_smoke.mac
./build/g4go --backend optix build/run_smoke.mac
```

命令行选项：

- `--backend auto|geant4|capture|cpu|optix`：选择传输后端，默认 `auto`。
- `auto` 优先尝试 OptiX；OptiX 构建依赖或运行时驱动不可用时切换到 CPU。
- 显式指定 `--backend optix` 时，OptiX 初始化失败会报告错误并终止，便于定位 GPU 环境问题。
- `--seed UINT64`：设置 Geant4 和 optical reference 的种子，默认 `42`。
- `--threads UINT32`：Geant4 backend 的 worker 数量；auto、capture、CPU 和 OptiX 使用串行 Geant4 事件驱动。
- `--max-photons UINT32`：单事件 photon capture 上限，默认 `5'000'000`。
- `--max-bounces UINT32`：单 photon 最大边界交互次数，默认 `4096`。

## Backend 结构

| Backend | Geant4 光子跟踪 | capture | CPU/OptiX 传输 | 结果写入 |
| --- | ---: | ---: | ---: | ---: |
| `auto` | 创建后立即 kill | 开启 | OptiX，失败时 CPU | `SensorHC` / `pulse` |
| `geant4` | 开启 | 关闭 | 关闭 | Geant4 SD |
| `capture` | 创建后立即 kill | 开启 | 关闭 | 仅统计捕获量 |
| `cpu` | 创建后立即 kill | 开启 | CPU reference | `SensorHC` / `pulse` |
| `optix` | 创建后立即 kill | 开启 | OptiX 9.1 | `SensorHC` / `pulse` |

CPU 和 OptiX scene exporter 当前支持 `G4Box`、full-phi `G4Tubs`、材料 `RINDEX/GROUPVEL/ABSLENGTH`，以及 polished unified/glisur optical surface。边界 surface 查找遵循 directed border surface、daughter skin surface、current skin surface 的优先级。

OptiX backend 将 device program 编译为 OptiX IR，使用 custom primitive AABB GAS。每个 photon 对应一个 raygen launch slot，hit 结果通过稳定的 photon slot 回读，随机数使用 host/device 一致的 Philox4x32-10。

## 验证

```bash
cmake --build build --target g4go-format-check
ctest --test-dir build --output-on-failure
rootls -t build/smoke.root
```

`build/run_optical_smoke.mac` 使用一个从 core 内部发射的 deterministic optical photon，适合验证初始化、capture、transport、SensorHC 和 ROOT 输出。`build/run_smoke.mac` 保留一个 gamma event 的源链路 smoke，gamma 是否发生能量沉积由 Geant4 随机过程决定；`build/run_regression.mac` 保留原有 1000 event 基线。
`build/run_optical_benchmark.mac` 使用单个 run 中的 1000 个直接光子，用于比较 capture、CPU reference 和 OptiX 的传输统计。

OptiX 的编译验证覆盖 device IR、IR embedding、host pipeline 链接和 core tests。`auto` backend 需要驱动提供完整的 `libnvoptix.so.1` API；部分 WSL 环境只提供 loader shim，缺少 `optixQueryFunctionTable`，此时运行阶段会自动切换到 CPU。显式使用 `--backend optix` 时会报告驱动能力不足。
