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

项目面向单晶体光学探测单元，提供统一的 CPU/GPU optical backend、Geant4 event adapter 和 ROOT ntuple 输出。GPU backend 负责光学光子的批量输运，Geant4 继续负责粒子相互作用、几何定义、材料属性、事件管理和输出数据写入。

## 主要能力

- 使用 Geant4 完成探测器建模、初级粒子产生、能量沉积和 CPU 光学输运。
- 使用 CUDA 与 NVIDIA OptiX 9.1 执行 GPU 光学光子输运。
- 支持 `auto`、`cpu` 和 `gpu` 三种 backend 选择策略。
- 将 `CrystalHit` 和 `SensorHit` 写入 ROOT ntuple。
- 提供光学边界单元测试、端到端 smoke test，以及包含 TOF 和传感器占用检查的 CPU/GPU 光学回归测试。

## Backend

| Backend | 光学光子处理 | 适用场景 | 失败行为 |
| --- | --- | --- | --- |
| `auto` | 构建了 GPU backend 时选择 OptiX，否则选择 Geant4 | 通用运行与 smoke test | OptiX 运行时错误直接上报 |
| `cpu` | Geant4 原生 optical tracking | 基准输出、无 NVIDIA GPU 环境 | Geant4 错误直接上报 |
| `gpu` | 捕获 Geant4 生成的光学光子并交给 OptiX | GPU 验证、性能测试、回归测试 | OptiX 不可用时直接失败 |

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

### 测试与输出检查

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

`G4GO_CUDA_ARCHITECTURE` 使用目标 GPU 的 compute capability 数值，例如 `compute_89` 对应 `89`。

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

使用 `--backend cpu` 或 `--backend gpu` 可显式选择 backend。

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
| `--batch-timeout-ms UINT32` | `10` | 发射 GPU batch 前等待更多 event 的最长时间 |
| `--max-photons UINT32` | `5000000` | 单事件允许捕获的最大光子数 |
| `--max-bounces UINT32` | `4096` | 单光子的最大边界交互次数 |
| `--perf-diagnostics` | disabled | 输出 capture、scheduler、CUDA stage、OptiX 和 transport counter 诊断 |
| `--mesh-rotation-steps UINT32` | `360` | Geant4 polyhedron 旋转采样数，有效范围为 8 到 4096 |
| `macro` | — | Geant4 macro 文件；无 UI 构建时必须提供 |

内置 macro：

| 文件 | 用途 | Analysis 文件名 |
| --- | --- | --- |
| `scripts/run_optical_test.mac` | 从晶体内部发射一个确定性的 optical photon | `run_optical_test.root` |
| `scripts/run_eminus_regression.mac` | 5 MeV 电子束 CPU/GPU 回归，共 50000 个 event | `run_eminus_regression.root` |
| `scripts/vis.mac` | 交互式几何与轨迹可视化 | `vis.root` |

ROOT 文件写入当前工作目录，文件名由 macro 中的 `/analysis/setFileName` 确定。



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

Geant4 是几何真值来源；scene exporter 将 solid 转换为 OptiX mesh，并保留 placement、material、surface、copy number 和 parent 信息。

## GPU 光学范围与限制

当前 GPU backend 支持以下边界能力：

- `glisur` 和 `unified` surface model 下的 `polished` 与 `ground` finish。
- `dielectric_metal` 和 `dielectric_dielectric` surface type。
- 包含 S/P 偏振分量的 Fresnel 反射、折射和全反射。
- `REFLECTIVITY`、`TRANSMITTANCE` 与 absorption 路径中的 `EFFICIENCY`。
- GLISUR polish、UNIFIED sigma-alpha、specular lobe/spike/backscatter 参数。

scene export 会显式拒绝当前未实现的 Rayleigh、Mie、WLS、LUT、DAVIS、dichroic、coated surface type/model 及 painted finish。达到 `--max-bounces` 的 photon 计入 `truncated`；无效状态计入 `invalid`，两者都会出现在 GPU backend 运行统计中。

## 测试

构建完成后运行快速测试：

```bash
ctest --test-dir build --output-on-failure -L 'unit|smoke'
```

| CTest | 标签 | 内容 |
| --- | --- | --- |
| `g4go_optical_boundary` | `unit;cpu` | Fresnel、Brewster 角、全反射、表面概率和 Lambertian 方向 |
| `g4go_smoke_auto` | `smoke;integration;auto` | 单光子端到端初始化与输运 |
| `g4go_noptpho_regression` | `regression;gpu;slow` | `GPU-1T` 和 `GPU-6T` 分别与缓存或按需生成的 `CPU-6T` reference 比较 `Edep`、`nOptPho`、TOF 和传感器占用率 |

### 性能基准

`test/regression_test.sh` 同时负责性能测量和 CPU/GPU 回归检验。为便于说明，本文统一使用以下配置名称：

- `CPU-6T`：6 个 Geant4 worker 的 CPU 运行，生成回归 reference 和 CPU 性能基准。
- `GPU-1T`：1 个 Geant4 worker 的 GPU/OptiX 运行。
- `GPU-6T`：6 个 Geant4 worker 的 GPU/OptiX 运行。

脚本按以下顺序执行：

1. 运行或读取缓存的 `CPU-6T` reference。
2. 测量 `GPU-1T` 和 `GPU-6T` 的墙钟时间并保存 ROOT 输出。
3. 将两组 GPU 输出分别与同一个 `CPU-6T` reference 比较 `Edep`、`nOptPho`、TOF 和传感器占用率。
4. 比较性能：`GPU-1T` 对照估算的 `CPU-1T`，`GPU-6T` 对照实测的 `CPU-6T`：

   ```text
   估算的 CPU-1T 时间 = CPU-6T 墙钟时间 × 6
   GPU-1T 加速比 = 估算的 CPU-1T 时间 / GPU-1T 墙钟时间
   GPU-6T 加速比 = CPU-6T 墙钟时间 / GPU-6T 墙钟时间
   ```

直接运行完整流程：

```bash
test/regression_test.sh --build-dir build
```

汇总、比较报告和图片写入 `build/test/regression/`。设置 `G4GO_PERF_DIAGNOSTICS=1` 可输出两组 GPU 分阶段计时。

### CPU/GPU 回归

CTest 测试 `g4go_noptpho_regression` 是上述脚本的项目测试入口，使用相同的 `CPU-6T` reference、`GPU-1T` 和 `GPU-6T` 流程：

```bash
ctest --test-dir build --output-on-failure \
  -R '^g4go_noptpho_regression$'
```

该测试要求两组 GPU 比较均通过，并检查对应的 ROOT 报告、汇总文件和图片。需要查看完整日志时，可直接运行：

```bash
bash build/test/regression_test.sh --build-dir build --verbose
```

每次运行的产物保存在 `build/test/regression/` 下的时间戳目录中；两组回归报告以 `gpu_1t` 和 `gpu_6t` 文件名前缀区分，图片保存在 `figures/`，日志保存在 `logs/`。

## WSL2 OptiX runtime 排障

WSL2 使用 Windows 主机 NVIDIA 驱动和 WSL 内的 CUDA Toolkit。环境准备参考 [CUDA on WSL User Guide](https://docs.nvidia.com/cuda/wsl-user-guide/index.html)。缺少 `optixQueryFunctionTable` symbol 时会出现：

```text
optixInit failed: OPTIX_ERROR_ENTRY_SYMBOL_NOT_FOUND
```

检查驱动、Toolkit 和 OptiX loader：

```bash
nvidia-smi
nvcc --version
nm -D /usr/lib/wsl/lib/libnvoptix.so.1 | rg optixQueryFunctionTable
```

缺少该 symbol 时更新 Windows NVIDIA 驱动，在 PowerShell 执行 `wsl --shutdown`，重新打开 WSL 后再次运行 GPU smoke test。

## 贡献

提交 issue 或 pull request 时请说明问题背景、复现方式、backend、Geant4/CUDA/OptiX 版本和验证输出。代码改动应保持模块边界，补充与改动直接相关的测试，并在提交前运行：

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
