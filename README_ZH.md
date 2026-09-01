# G4GO

> **Geant4** **G**PU-accelerated **O**ptical physics simulation

<p align="center">
  <a href="README.md">English</a> · <a href="README_ZH.md">简体中文</a>
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue.svg" alt="License: Apache-2.0"></a>
  <a href="https://isocpp.org/"><img src="https://img.shields.io/badge/C%2B%2B-20-00599C.svg" alt="C++20"></a>
  <a href="https://cmake.org/"><img src="https://img.shields.io/badge/CMake-3.24%2B-064F8C.svg" alt="CMake 3.24 or newer"></a>
  <a href="https://geant4.web.cern.ch/"><img src="https://img.shields.io/badge/Geant4-11.4.2-1F77B4.svg" alt="Geant4 11.4.2"></a>
  <a href="https://developer.nvidia.com/cuda-toolkit"><img src="https://img.shields.io/badge/CUDA-12.0%2B-76B900.svg" alt="CUDA 12.0 or newer"></a>
  <a href="https://developer.nvidia.com/optix"><img src="https://img.shields.io/badge/OptiX-9.1-76B900.svg" alt="NVIDIA OptiX 9.1"></a>
</p>

G4GO 是一个以 Geant4 为物理与几何底层、使用 CUDA/OptiX 加速光学光子输运模拟的项目。

项目面向单晶体光学探测单元，提供统一的 CPU/GPU optical backend、Geant4 event adapter 和 ROOT ntuple 输出。GPU backend 负责光学光子的批量输运，Geant4 继续负责粒子相互作用、几何定义、材料属性、事件管理和结果写入。

## 主要能力

- 使用 Geant4 完成探测器建模、初级粒子产生、能量沉积和 CPU 光学输运。
- 使用 CUDA 与 NVIDIA OptiX 9.1 执行 GPU 光学光子输运。
- 支持 `auto`、`cpu` 和 `gpu` 三种 backend 选择策略。
- 将 `CrystalHit` 和 `SensorHit` 写入 ROOT ntuple。
- 提供光学边界单元测试、端到端 smoke test 和 CPU/GPU `nOptPho` 回归测试。

## Backend

| Backend | 光学光子处理 | 适用场景 | 失败行为 |
| --- | --- | --- | --- |
| `auto` | 优先使用 OptiX，初始化失败时切换到 Geant4 | 通用运行与 smoke test | 保留告警并继续使用 CPU |
| `cpu` | Geant4 原生 optical tracking | 基准结果、无 NVIDIA GPU 环境 | Geant4 错误直接上报 |
| `gpu` | 捕获 Geant4 生成的光学光子并交给 OptiX | GPU 验证、性能测试、回归测试 | OptiX 不可用时直接失败 |

GPU backend 使用事件聚合的异步流水线。Geant4 worker 将事件提交到共享队列，独立 GPU executor 聚合多个事件后执行 OptiX launch；结果按 event ID 返回原 worker，并由 Geant4 analysis manager 写入 ROOT 文件。

## 依赖

### 基础构建

| 依赖 | 要求 | 用途 |
| --- | --- | --- |
| CMake | 3.24 或更高 | 配置与构建 |
| C++ compiler | 支持 C++20 | 编译 host 代码 |
| Geant4 | 11.4.2 或兼容版本 | 物理、几何、事件与分析 |
| Git 和网络访问 | 首次配置时可用 | 通过 `FetchContent` 获取 CLI11；启用 OptiX 时获取 headers |

### GPU backend

- NVIDIA GPU 与版本号高于 R590 的 NVIDIA 驱动；OptiX runtime 由 NVIDIA 驱动提供。
- CUDA Toolkit 12.0 或更高版本，包含 `nvcc` 和 `bin2c`。
- OptiX 9.1 development headers 由 CMake 从 `NVIDIA/optix-dev` 获取；完整 OptiX SDK 对 G4GO 属于可选依赖。
- `G4GO_CUDA_ARCHITECTURE` 与目标 GPU 的 compute capability 一致。

### 测试与结果检查

- Bash 用于 CPU/GPU 回归测试编排。
- ROOT 命令行程序用于回归比较，`rootls` 可用于检查输出文件。
- `clang-format` 可选；检测到后会生成格式化相关目标。

## 构建

### 启用 OptiX

```bash
cmake -S . -B build \
  -DG4GO_BUILD_UIVIS=OFF \
  -DG4GO_ENABLE_OPTIX=ON
cmake --build build -j
```

配置阶段会检查 CUDA compiler、CUDA Toolkit、`bin2c` 和 OptiX headers。完整 OptiX SDK 不属于必需依赖，因为 CMake 会自动获取 development headers。依赖完整时会构建 OptiX backend；依赖不完整时仍会生成支持 Geant4 backend 的 `g4go`。

`G4GO_CUDA_ARCHITECTURE` 使用 CUDA 的 `compute_XX` 数值，默认值为 `120`。例如目标架构为 `compute_89` 时使用：

```bash
cmake -S . -B build \
  -DG4GO_BUILD_UIVIS=OFF \
  -DG4GO_ENABLE_OPTIX=ON \
  -DG4GO_CUDA_ARCHITECTURE=89
cmake --build build -j
```

### 仅使用 Geant4 backend

```bash
cmake -S . -B build \
  -DG4GO_BUILD_UIVIS=OFF \
  -DG4GO_ENABLE_OPTIX=OFF
cmake --build build -j
```

### 常用 CMake 选项

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `G4GO_BUILD_UIVIS` | `ON` | 构建 Geant4 UI 与 visualization 支持 |
| `G4GO_ENABLE_OPTIX` | `ON` | 尝试构建 CUDA/OptiX backend |
| `G4GO_CUDA_ARCHITECTURE` | `120` | OptiX device program 的 CUDA compute architecture |
| `G4GO_ENABLE_FORMAT_TARGETS` | `ON` | 检测并创建 clang-format 目标 |
| `G4GO_ENABLE_CLANG_TIDY` | `OFF` | 编译时启用 clang-tidy |
| `BUILD_TESTING` | `ON` | 注册 CTest 测试 |

配置时，`scripts/` 会复制到 `build/scripts/`，测试运行文件会复制到 `build/test/`。修改相关文件后重新执行 CMake 配置即可同步构建目录。

安装可执行文件：

```bash
cmake --install build --prefix /path/to/prefix
```

## 快速开始

在构建目录运行单光子 smoke macro：

```bash
cd build
./g4go --backend auto scripts/run_optical_test.mac
```

显式运行 CPU 或 GPU backend：

```bash
cd build
./g4go --backend cpu --threads 1 scripts/run_optical_test.mac
./g4go --backend gpu scripts/run_optical_test.mac
```

成功启用 GPU backend 时，运行日志包含：

```text
[g4go] optical backend: gpu, ...
```

启用 `G4GO_BUILD_UIVIS=ON` 后，可从包含 `vis.mac` 的目录启动交互式 visualization：

```bash
cd build/scripts
../g4go
```

## 命令行接口

```text
g4go [OPTIONS] [macro]
```

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `-h, --help` | — | 显示帮助信息 |
| `--backend auto\|cpu\|gpu` | `auto` | 选择光学光子输运 backend |
| `--seed UINT64` | `42` | 设置 Geant4 和 OptiX 随机种子 |
| `--threads UINT32` | `0` | Geant4 worker 数量；`0` 表示使用检测到的 CPU core 数量 |
| `--batch-photons UINT32` | `1000000` | GPU batch 的目标光子数 |
| `--max-photons UINT32` | `5000000` | 单事件允许捕获的最大光子数 |
| `--max-bounces UINT32` | `4096` | 单光子的最大边界交互次数 |
| `--mesh-rotation-steps UINT32` | `360` | Geant4 polyhedron 旋转采样数，有效范围为 8 到 4096 |
| `macro` | — | Geant4 macro 文件；无 UI 构建时必须提供 |

内置 macro：

| 文件 | 用途 | Analysis 文件名 |
| --- | --- | --- |
| `scripts/run_optical_test.mac` | 从晶体内部发射一个确定性的 optical photon | `run_optical_test.root` |
| `scripts/run_beam_eminus.mac` | 从晶体前方发射 5 MeV 电子束，共 2000 个 event | `run_beam_eminus.root` |
| `scripts/vis.mac` | 交互式几何与轨迹可视化 | `vis.root` |

ROOT 文件写入当前工作目录。仓库自带 macro 的文件名由各自的 `/analysis/setFileName` 命令确定；event 没有产生可写 ntuple 记录时，Geant4 analysis manager 可能不会保留空文件。可在束流运行后检查结果：

```bash
cd build
./g4go --backend cpu scripts/run_beam_eminus.mac
rootls -t run_beam_eminus.root
```

## 探测器模型

当前 `DetectorConstruction` 构造一个单晶体光学探测单元：

- `3 cm × 3 cm × 8 cm` 的 `G4_CESIUM_IODIDE` 晶体。
- 晶体光学参数包含折射率、吸收长度、闪烁发光谱、闪烁产额和时间常数。
- 晶体前端依次连接硅脂耦合层、环氧窗口和 `3 mm × 3 mm × 0.1 mm` SiPM。
- 晶体与包覆层、世界体及 SiPM 之间配置 optical surface 和探测效率曲线。

材料属性和光学表面当前集中定义在 `src/detector/DetectorConstruction.cpp`。

## ROOT 输出

ROOT 文件包含两个 tree。tree 名使用 PascalCase，branch 名使用 lowerCamelCase；`Edep` 和 `nOptPho` 保留项目约定名称。

| Tree | Branch | 含义 |
| --- | --- | --- |
| `CrystalHit` | `eventID` | Geant4 event ID |
| `CrystalHit` | `moduleID` | 晶体模块 ID |
| `CrystalHit` | `Edep` | 晶体能量沉积 |
| `CrystalHit` | `nOptPho` | 该 event 的 SiPM 探测光子数 |
| `SensorHit` | `eventID` | Geant4 event ID |
| `SensorHit` | `sensorID` | SiPM copy number |
| `SensorHit` | `timeOfFlight` | 光子到达时间 |

## 架构

```text
G4GO/
├── cmake/                  # CMake helper scripts
├── include/g4go/
│   ├── detector/           # 探测器公开接口
│   ├── optical/            # backend 无关的光学数据结构与接口
│   └── simulation/         # Geant4 action 公开接口
├── scripts/                # Geant4 macro
├── src/
│   ├── detector/           # 几何、材料与 sensitive detector
│   ├── optical/
│   │   ├── geant4/         # event adapter、scene exporter 与 batch service
│   │   └── optix/          # OptiX host/device transport
│   └── simulation/         # run、event 与 analysis action
└── test/                   # 单元、smoke 与回归测试
```

CMake 将实现编译为 `g4go_optical_core`、可选的 `g4go_optical_optix`、`g4go_optical_geant4`、`g4go_detector` 和 `g4go_simulation` 静态库，最终由 `g4go` 可执行文件组合。

Geant4 是 geometry truth source。scene exporter 通过 `CreatePolyhedron()` 将 solid 转为 triangle mesh，并保留 placement、material、surface、copy number 和 parent topology。相同 `G4VSolid` 的 mesh 可复用，每个 physical volume 仍对应独立的 OptiX instance。

## GPU 光学范围与限制

当前 GPU backend 支持以下边界能力：

- `glisur` 和 `unified` surface model 下的 `polished` 与 `ground` finish。
- `dielectric_metal` 和 `dielectric_dielectric` surface type。
- 包含 S/P 偏振分量的 Fresnel 反射、折射和全反射。
- `REFLECTIVITY`、`TRANSMITTANCE` 与 absorption 路径中的 `EFFICIENCY`。
- GLISUR polish、UNIFIED sigma-alpha、specular lobe/spike/backscatter 参数。

scene export 会显式拒绝当前未实现的 Rayleigh、Mie、WLS、LUT、DAVIS、dichroic、coated surface type/model 及 painted finish。达到 `--max-bounces` 的 photon 计入 `truncated`；无效状态计入 `invalid`，两者都会出现在 GPU backend 运行统计中。

`--mesh-rotation-steps 360` 采用 correctness-first 默认值。复杂几何 profiling 应同时评估 mesh 误差、triangle 数量、GAS build time、显存与 transport time。

## 测试

构建完成后运行快速测试：

```bash
ctest --test-dir build --output-on-failure -L 'unit|smoke'
```

| CTest | 标签 | 内容 |
| --- | --- | --- |
| `g4go_optical_boundary` | `unit;cpu` | Fresnel、Brewster 角、全反射、表面概率和 Lambertian 方向 |
| `g4go_smoke_auto` | `smoke;integration;auto` | 单光子端到端初始化与输运 |
| `g4go_noptpho_regression` | `regression;cpu;gpu;slow` | 单核 CPU、全核 CPU 与 GPU 的 `CrystalHit.nOptPho` 比较 |

运行全部测试：

```bash
ctest --test-dir build --output-on-failure
```

### CPU 单核/全核与 GPU `nOptPho` 回归

```bash
(
  cd build
  ctest --no-label-summary --output-on-failure \
    -R '^g4go_noptpho_regression$'
)
```

回归编排器使用 `scripts/run_beam_eminus.mac` 依次运行单核 CPU、全核 CPU、全核 GPU 和 ROOT 比较。默认情况下，“全核”使用检测到的全部物理核心；可通过 `--threads N` 覆盖 worker 数量。普通模式只打印阶段摘要，同时将子进程完整输出保存到阶段日志；直接运行编排器时增加 `--verbose` 可以实时显示子进程输出。统计比较使用全核 CPU 结果作为 Geant4 参考，同时报告 GPU 相对于两种 CPU 运行的加速比。GPU 运行显式使用 `--backend gpu`，因此 GPU、OptiX runtime、驱动或 ROOT 不可用时测试会失败并保留诊断文件。

命令特意不使用 CTest 的 `-V` 选项。`--no-label-summary` 隐藏重复的标签耗时表，`--output-on-failure` 仅在测试失败时显示测试输出。需要查看完整回归输出时，直接运行：

```bash
bash build/test/regression_test.sh --build-dir build --verbose
```

ROOT 比较宏 `test/TestOpticalTransport.cxx` 使用未加权 `Chi2TestX(..., "UU P OF")` 比较 `nOptPho` 分布，同时检查 CPU/GPU 总光子数相对差异。`Z > 5`、卡方条件无效或总量相对差异超过 10% 时测试失败；`3 < Z <= 5` 标记为 `SUSPICIOUS`，其余有效结果通过。

每次运行的产物位于 `build/test/regression/noptpho_<timestamp>/`：

- `noptpho_cpu_single.root`、`noptpho_cpu_all.root`、`noptpho_gpu.root`：单核 CPU、全核 CPU 和 GPU 输入数据。
- `noptpho_comparison.png`、`noptpho_regression_report.root`：分布、pull 和统计结果。
- `regression_result.txt`：结论、三组墙钟时间和 GPU 相对于两种 CPU 运行的加速比。
- `cpu_single.log`、`cpu_all.log`、`gpu.log`、`comparison.log`：各阶段完整日志；`regression.log` 保存编排输出，verbose 模式下也包含透传的子进程输出。

## WSL2 OptiX runtime 排障

WSL2 的 CUDA 与 OptiX runtime 依赖 Windows 主机 NVIDIA 驱动映射。环境准备请参考 [CUDA on WSL User Guide](https://docs.nvidia.com/cuda/wsl-user-guide/index.html)。WSL 内安装 CUDA Toolkit 即可；NVIDIA display driver 由 Windows 主机提供。

部分 WSL2 环境中的 `/usr/lib/wsl/lib/libnvoptix.so.1` 仅提供 loader shim，缺少 `optixQueryFunctionTable` 时会出现：

```text
optixInit failed: OPTIX_ERROR_ENTRY_SYMBOL_NOT_FOUND
```

检查当前环境：

```bash
nvidia-smi
nvcc --version
ls -lh /usr/lib/wsl/lib/libnvoptix.so.1
nm -D /usr/lib/wsl/lib/libnvoptix.so.1 | rg optixQueryFunctionTable
```

可从与主机驱动兼容的 NVIDIA Linux display driver 安装包中仅提取 OptiX 用户态库，将 `libnvoptix.so.*`、`libnvidia-rtcore.so.*`、`libnvidia-ptxjitcompiler.so.*`、`libnvidia-gpucomp.so.*` 和 `nvoptix.bin` 放入用户目录，并让该目录在 `LD_LIBRARY_PATH` 中排在 `/usr/lib/wsl/lib` 前面。该流程用于提取库文件，避免在 WSL 内安装 Linux display driver。

验证自定义 runtime：

```bash
export G4GO_OPTIX_RUNTIME=/path/to/extracted/optix-runtime

nm -D "$G4GO_OPTIX_RUNTIME/libnvoptix.so.1" \
  | rg optixQueryFunctionTable

LD_LIBRARY_PATH="$G4GO_OPTIX_RUNTIME:/usr/lib/wsl/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ldd "$G4GO_OPTIX_RUNTIME/libnvoptix.so.1" \
  | rg 'nvoptix|rtcore|ptxjit|gpucomp|not found'

./build/g4go --backend gpu build/scripts/run_optical_test.mac
```

更新 Windows NVIDIA 驱动或 WSL 映射后，在 PowerShell 执行 `wsl --shutdown`，重新打开 WSL 并再次验证动态库加载顺序。

## 贡献

提交 issue 或 pull request 时请说明问题背景、复现方式、backend、Geant4/CUDA/OptiX 版本和验证结果。代码改动应保持模块边界，补充与改动直接相关的测试，并在提交前运行：

```bash
cmake --build build --target g4go-format-check
ctest --test-dir build --output-on-failure -L 'unit|smoke'
```

提交信息遵循 [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/)，例如：

```text
fix(optical): preserve photon polarization across reflection
```

## 许可证

Copyright 2026 Siyuan Chen

本项目采用 [Apache License 2.0](LICENSE)。完整许可条款见仓库根目录的 [LICENSE](LICENSE) 文件。
