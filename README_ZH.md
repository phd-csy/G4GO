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
  <a href="https://developer.nvidia.com/cuda-toolkit"><img src="https://img.shields.io/badge/CUDA-12.8%2B-76B900.svg" alt="CUDA 12.8 or newer"></a>
  <a href="https://developer.nvidia.com/optix"><img src="https://img.shields.io/badge/OptiX-9.1-76B900.svg" alt="NVIDIA OptiX 9.1"></a>
</p>

G4GO 用 Geant4 描述粒子相互作用、探测器几何和材料，用 CUDA/OptiX 加速其中最耗时的光学光子输运。项目当前模拟一个  SiPM 阵列读出的单 CsI 闪烁晶体探测单元，并把能量沉积、光子产生数、探测数和到达时间写入 ROOT。

同一套程序支持三种 optical backend：

| Backend | 如何处理光学光子 | 使用场景 |
| --- | --- | --- |
| `auto` | 构建中包含 OptiX 时使用 GPU，否则使用 Geant4 | 日常运行和快速检查 |
| `cpu` | 全部交给 Geant4 tracking | 生成 CPU reference，或在没有 NVIDIA GPU 的机器上运行 |
| `gpu` | Geant4 产生光子，CUDA/OptiX 批量完成输运 | GPU 验证、回归和性能测试 |

## 从零开始

下面的命令都从仓库根目录开始。完成构建后，运行命令统一在 `build/` 目录执行，这样 macro、测试脚本和输出路径都保持一致。

### 第 1 步：准备依赖

CPU 构建需要：

- CMake 3.24 或更高版本；
- 支持 C++20 的编译器；
- Geant4 11.4.2；
- Git 和网络访问，首次配置时 CMake 会获取 CLI11。

GPU backend 还需要：

- NVIDIA GPU 和 R590 或更新的驱动；
- CUDA Toolkit 12.8 或更高版本，并包含 `nvcc` 和 `bin2c`；
- 与 GPU compute capability 对应的 CUDA architecture。OptiX 9.1 headers 会在首次配置时由 CMake 获取，OptiX runtime 由 NVIDIA 驱动提供。

ROOT 不参与主程序构建，但运行 CPU/GPU 回归和检查 ROOT 输出时需要 ROOT 命令行工具。

如果 Geant4 安装在自定义位置，先加载它的环境，例如：

```bash
source /path/to/geant4/bin/geant4.sh
```

### 第 2 步：检查当前环境

```bash
./test/environment.sh
```

脚本会列出实际检测到的 Geant4、CMake、CUDA、GPU、驱动、OptiX 和 ROOT，并用 `OK` 或 `FAIL` 标出是否满足当前项目要求。只准备运行 CPU backend 时，可以忽略 GPU、CUDA 和 OptiX 项的失败。

### 第 3 步：选择构建方式

使用 GPU backend 时，先指定 CMake 应使用的 CUDA Toolkit：

```bash
export CUDA_HOME=/usr/local/cuda
export CUDACXX="${CUDA_HOME}/bin/nvcc"
export PATH="${CUDA_HOME}/bin:${PATH}"
nvcc --version
```

然后配置并构建。默认 architecture 是 `120`；使用其他 GPU 时，将下面的值改为对应的 compute capability，例如 RTX 4090 使用 `89`：

```bash
cmake -S . -B build \
  -DG4GO_BUILD_UIVIS=OFF \
  -DG4GO_ENABLE_OPTIX=ON \
  -DG4GO_CUDA_ARCHITECTURE=120 \
  -DCMAKE_CUDA_COMPILER="${CUDACXX}"
cmake --build build -j
```

只使用 Geant4 CPU backend 时，配置更简单：

```bash
cmake -S . -B build \
  -DG4GO_BUILD_UIVIS=OFF \
  -DG4GO_ENABLE_OPTIX=OFF
cmake --build build -j
```

第一次配置后，CMake 会记住 CUDA compiler。切换 CUDA Toolkit 时应使用新的构建目录，或清理旧的 CMake cache 后重新配置。

常用配置项如下：

| CMake 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `G4GO_BUILD_UIVIS` | `ON` | 构建 Geant4 UI 和 visualization 支持 |
| `G4GO_ENABLE_OPTIX` | `ON` | 尝试构建 CUDA/OptiX backend |
| `G4GO_CUDA_ARCHITECTURE` | `120` | OptiX device program 的 CUDA architecture |
| `G4GO_ENABLE_CLANG_TIDY` | `OFF` | 编译时启用 clang-tidy |
| `BUILD_TESTING` | `ON` | 构建并注册 CTest 测试 |

### 第 4 步：确认程序可以运行

先运行一个只发射单个光学光子的 smoke test：

```bash
cd build
./g4go --backend auto scripts/run_test_optics.mac
```

需要确认指定 backend 时，分别运行：

```bash
./g4go --backend cpu scripts/run_test_optics.mac
./g4go --backend gpu scripts/run_test_optics.mac
```

`gpu` 命令会在 OptiX 没有编入程序或 runtime 初始化失败时直接报错，因此它也适合验证 GPU 环境是否真正可用。

### 第 5 步：运行一次模拟

G4GO 的基本调用形式是：

```text
g4go [OPTIONS] [macro]
```

例如，用 6 个 Geant4 worker 在 CPU backend 上运行随仓库提供的电子束回归 macro：

```bash
cd build
./g4go --backend cpu --threads 6 scripts/run_regression_eminus.mac
```

切换到 GPU 只需更换 backend：

```bash
./g4go --backend gpu --threads 6 scripts/run_regression_eminus.mac
```

仓库内置 macro 的用途如下：

| Macro | 用途 | 规模与输出 |
| --- | --- | --- |
| `scripts/run_test_optics.mac` | 单光子 smoke test | 1 个 event，不写 ROOT |
| `scripts/run_regression_eminus.mac` | CPU/GPU 正确性回归 | 5000 个 event，写入 `run_regression_eminus/` |
| `scripts/run_benchmark_eminus.mac` | 完整性能测量 | 50000 个 event，写入 `run_benchmark_eminus/` |
| `scripts/run_cpu_scaling.mac` | CPU scaling 估算 | 5000 个 event，写入 `run_cpu_scaling/` |
| `scripts/vis.mac` | 交互式几何和轨迹显示 | UI/visualization 构建使用 |

修改了 `scripts/` 或 `test/` 下的运行文件后，重新执行 `cmake -S . -B build`，把当前版本复制到 `build/scripts/` 和 `build/test/`。

### 第 6 步：找到并查看输出

分析 macro 会在当前目录创建同名输出目录。由于 G4GO 关闭了 ROOT ntuple merging，多线程运行会得到一组 worker 文件：

```text
build/run_regression_eminus/
├── run_regression_eminus_t0.root
├── run_regression_eminus_t1.root
└── ...
```

可以用 ROOT 查看文件内容：

```bash
rootls run_regression_eminus/run_regression_eminus_t0.root
```

每个 ROOT 文件包含两个 tree：

| Tree | Branch | 内容 |
| --- | --- | --- |
| `CrystalHit` | `eventID`, `moduleID` | event 和晶体模块标识 |
| `CrystalHit` | `Edep` | 晶体能量沉积 |
| `CrystalHit` | `nOptPho` | 该 event 的 SiPM 探测光子数 |
| `CrystalHit` | `nGenOptPho` | 该 event 的光学光子产生数 |
| `SensorHit` | `eventID` | Geant4 event ID |
| `SensorHit` | `sensorID` | 每次 detection 对应的 SiPM copy number vector |
| `SensorHit` | `timeOfFlight` | 每次 detection 对应的到达时间 vector |

`SensorHit` 只为至少发生一次 detection 的 event 写一行。`sensorID` 和 `timeOfFlight` 长度相同，相同下标共同描述一次 detection。

## 常用工作流

### 快速测试

构建完成后先跑耗时较短的单元测试和 smoke test：

```bash
cd build
ctest --output-on-failure -L 'unit|smoke'
```

当前包含 `g4go_optical_boundary` 和 `g4go_smoke_auto` 两个测试。CPU/GPU 回归耗时更长，由独立脚本运行。

### CPU/GPU 正确性回归

```bash
cd build
./test/regression.sh --threads 6
```

脚本会生成或复用同线程数的 CPU reference，再分别运行 `GPU-1T` 和 `GPU-6T`，比较 `Edep`、`nOptPho`、TOF 和 sensor occupancy。完整日志可用 `--verbose` 实时显示：

```bash
./test/regression.sh --threads 6 --verbose
```

结果保存在 `build/test/regression/<timestamp>/`：

- `regression_summary.txt` 给出总结果和各项统计门限；
- `figures/` 保存比较图；
- `logs/` 保存每个阶段的日志；
- `gpu_1t/` 和 `gpu_6t/` 保存本次 GPU ROOT dataset。

CPU reference 缓存在 `build/test/regression/cpu_6t/`。可执行文件、macro、线程数或 event 数变化时，脚本会自动重新生成缓存。

### 性能 benchmark

```bash
cd build
./test/benchmark.sh --threads 6
```

benchmark 依次测量 CPU partial scaling、CPU full run、GPU-1T full run 和 GPU-6T full run，再计算线程数匹配的加速比。它只负责性能测量，不执行 ROOT 正确性比较。

默认交给操作系统调度 CPU。需要自动选择 GPU 所在 NUMA node 的 physical cores 时使用：

```bash
./test/benchmark.sh --threads 6 --cpu-affinity auto
```

也可以直接传给 `taskset` 一个 CPU list：

```bash
./test/benchmark.sh --threads 6 --cpu-affinity 0-6
```

结果保存在 `build/test/benchmark/<timestamp>/`。`benchmark_summary.txt` 记录运行配置、wall time、GPU transport 时间和加速比；需要 CUDA、scheduler 和 OptiX 的细分诊断时运行：

```bash
G4GO_PERF_DIAGNOSTICS=1 ./test/benchmark.sh --threads 6
```

此时同一结果目录还会生成 `performance_summary.txt`。加速比的配对关系是 `GPU-1T` 对 estimated `CPU-1T`，`GPU-6T` 对 `CPU-6T`。

### 交互式 visualization

配置时启用 `G4GO_BUILD_UIVIS=ON`，构建后从 `build/` 目录启动。程序会自动加载 `scripts/vis.mac`：

```bash
cd build
./g4go
```

### 静态检查

项目提供两种 clang-tidy 运行方式。需要在每次编译 C++ 文件时同步检查，可以通过 CMake 启用：

```bash
cmake -S . -B build -DG4GO_ENABLE_CLANG_TIDY=ON
cmake --build build -j
```

该选项会将 `.clang-tidy` 配置应用到所有项目 C++ target。`clang-tidy` 未安装时，CMake 配置会直接报错。

需要一次检查整个项目时，安装 `clang-tidy` 和 `run-clang-tidy`，准备好普通 CMake 构建目录后执行：

```bash
cmake -S . -B build
./test/static_analysis.sh --check
```

检查脚本使用 `build/compile_commands.json` 分析项目 C/C++ 文件。`--fix` 还需要 `clang-apply-replacements`，它会应用 `.clang-tidy` 中稳定且可自动修复的规则，使用前应先检查工作区状态。

### 提交前格式检查

`.pre-commit-config.yaml` 配置了 `clang-format` hook，用于在提交前检查并格式化暂存的 C/C++ 文件。安装并启用 pre-commit：

```bash
pipx install pre-commit
pre-commit install
```

安装 hook 后，执行 `git commit` 时会自动运行。第一次运行会下载配置中指定的 clang-format 环境。也可以在提交前手动检查仓库中的所有文件：

```bash
pre-commit run --all-files
```

## 命令行参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `-b, --backend auto\|cpu\|gpu` | `auto` | 选择 optical backend |
| `-s, --seed UINT64` | `0` | 设置 Geant4 和 OptiX 随机种子 |
| `-t, --threads UINT32` | `0` | Geant4 worker 数；`0` 使用检测到的 CPU core 数量 |
| `--batch-photons UINT32` | `1000000` | 每个 GPU batch 的目标光子数 |
| `--batch-timeout-ms UINT32` | `10` | GPU batch 等待更多 event 的最长时间 |
| `--max-photons UINT32` | `5000000` | 单个 event 最多捕获的光子数 |
| `--max-bounces UINT32` | `4096` | 单个光子的最大边界交互次数 |
| `-d, --diagnostics` | 关闭 | 输出 optical transport 性能诊断 |
| `--mesh-rotation-steps UINT32` | `360` | Geant4 polyhedron 旋转采样数，范围为 8～4096 |
| `macro` | — | Geant4 macro；关闭 UI 的构建必须提供 |

完整且与当前二进制一致的帮助可通过 `./g4go --help` 查看。

## 当前探测器模型

当前几何是一个可独立运行的单晶体探测单元：

- 主体为 `3 cm × 3 cm × 8 cm` 的 `G4_CESIUM_IODIDE` 晶体；
- 晶体前端依次放置 `0.1 mm` 硅脂耦合层和 `0.2 mm` 环氧窗口；
- 读出端为 `8 × 8` SiPM 阵列，每个单元尺寸为 `3 mm × 3 mm × 0.1 mm`；
- 晶体包含折射率、吸收长度、闪烁光谱、产额和时间常数，**SiPM surface 包含随波长变化的光子探测效率（PDE）**。

几何、材料和 optical surface 的当前定义集中在 `src/detector/DetectorConstruction.cpp`。修改探测器时，应同时检查 CPU Geant4 输运和 GPU scene export 是否支持所使用的 optical process 与 surface model。

## 项目结构与职责

```text
G4GO/
├── include/g4go/          # 对外头文件
├── src/
│   ├── detector/          # 几何、材料和 sensitive detector
│   ├── optical/geant4/    # event adapter、scene export 和 batch scheduling
│   ├── optical/optix/     # OptiX host/device transport
│   └── simulation/        # Geant4 actions 和 ROOT analysis
├── scripts/               # 可直接运行的 Geant4 macro
└── test/                  # 单元测试、回归、benchmark 和环境工具
```

Geant4 始终是几何和材料的真值来源。GPU backend 启动时，scene exporter 把 Geant4 solid 转成 OptiX mesh，并保留 placement、material、surface、copy number 和父子关系；Geant4 继续负责非光学粒子的物理过程和 event 生命周期。

当前 GPU 边界输运支持 `glisur`/`unified` model、`polished`/`ground` finish、`dielectric_metal`/`dielectric_dielectric` surface，以及带偏振的 Fresnel 反射、折射、全反射和常用表面概率属性。Rayleigh、Mie、WLS、LUT、DAVIS、dichroic、coated surface 和 painted finish 等尚未实现的配置会在 scene export 阶段明确报错。

## WSL2 下的 OptiX 问题

WSL2 使用 Windows 主机驱动提供的 OptiX runtime。出现下面的错误时，通常是 Windows NVIDIA 驱动中的 `libnvoptix` 版本过旧：

```text
optixInit failed: OPTIX_ERROR_ENTRY_SYMBOL_NOT_FOUND
```

检查当前环境：

```bash
nvidia-smi
nvcc --version
nm -D /usr/lib/wsl/lib/libnvoptix.so.1 | rg optixQueryFunctionTable
```

缺少 `optixQueryFunctionTable` 时，更新 Windows NVIDIA 驱动，在 PowerShell 执行 `wsl --shutdown`，重新进入 WSL 后再次运行 GPU smoke test。环境准备可参考 [CUDA on WSL User Guide](https://docs.nvidia.com/cuda/wsl-user-guide/index.html)。

## 贡献

提交 issue 或 pull request 时，请附上复现步骤、使用的 backend、Geant4/CUDA/OptiX 版本和验证结果。提交前至少运行与改动相关的测试以及：

```bash
git diff --check
```

Commit messages follow [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/). Choose a type that matches the purpose of the change:

- `feat`: add a new feature;
- `fix`: fix a bug;
- `docs`: change documentation only;
- `style`: change formatting or style without affecting code meaning;
- `refactor`: restructure code without fixing a bug or adding a feature;
- `perf`: improve performance;
- `test`: add missing tests or correct existing tests;
- `build`: change the build system or external dependencies;
- `ci`: change continuous-integration configuration or scripts;
- `revert`: revert a previous commit.

## 许可证

Copyright 2026 Siyuan Chen

本项目采用 [Apache License 2.0](LICENSE)。完整条款见仓库根目录的 [LICENSE](LICENSE)。
