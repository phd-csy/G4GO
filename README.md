# G4GO

G4GO 是一个以 Geant4 为物理基线、支持 CUDA/OptiX 光学传输的 MVP 工程。当前 optical backend 使用统一的 photon/event bridge，输出 `CrystalHit` 和 `SensorHit` ROOT ntuple。

## 构建

Geant4 11.4.2、CMake 3.24 以上和 C++20 是基础依赖。

```bash
cmake -S . -B build \
  -DG4GO_BUILD_UIVIS=OFF \
  -DG4GO_ENABLE_OPTIX=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

配置阶段会自动检查 CUDA compiler、CUDA Toolkit、`bin2c` 和 vendored OptiX headers。依赖完整时，`build/g4go` 包含 OptiX optical transport；依赖缺失时，`auto` backend 使用原生 Geant4 继续完成构建和运行。

配置后，`scripts/` 中的所有 `.mac` 宏会以普通文件形式复制到 `build/` 根目录，不会生成 `build/scripts/` 子目录。宏文件通过 `configure_file()` 纳入 CMake 的配置依赖；保存宏后执行构建目标时，构建系统会自动重新运行 CMake，并将最新内容复制到 `build/`。

每个宏的 ROOT 输出文件名都使用对应宏文件名去掉 `.mac` 后的名称，例如 `run_smoke.mac` 输出 `run_smoke.root`。

可以显式关闭 OptiX：

```bash
cmake -S . -B build \
  -DG4GO_BUILD_UIVIS=OFF \
  -DG4GO_ENABLE_OPTIX=OFF
```

`G4GO_CUDA_ARCHITECTURE` 使用 CUDA 的 `compute_XX` 数值。部署到其他 GPU 时按目标 GPU 的 compute capability 设置。

## WSL2 OptiX runtime

WSL2 中的 CUDA 和 OptiX 运行时依赖 Windows 主机上的 NVIDIA 驱动。请先按照 [CUDA on WSL User Guide](https://docs.nvidia.com/cuda/wsl-user-guide/index.html) 完成 WSL2、Windows NVIDIA 驱动和 WSL CUDA Toolkit 的安装。WSL 内只需要 CUDA Toolkit，Linux display driver 会干扰 WSL 映射的 `libcuda.so`。

部分 WSL2 环境中的 `/usr/lib/wsl/lib/libnvoptix.so.1` 只有 loader shim，文件很小并且不导出 `optixQueryFunctionTable`。此时程序可能报错：

```text
optixInit failed: OPTIX_ERROR_ENTRY_SYMBOL_NOT_FOUND
```

G4GO 使用下面的方式补充完整的 Linux OptiX 用户态库。该过程只提取 NVIDIA Linux display driver 的文件，不在 WSL 中安装 display driver。

### 1. 检查 WSL CUDA 环境

```bash
nvidia-smi
nvcc --version
ls -lh /usr/lib/wsl/lib/libnvoptix.so.1
nm -D /usr/lib/wsl/lib/libnvoptix.so.1 | rg optixQueryFunctionTable
```

`nvidia-smi` 应能看到 Windows 主机上的 NVIDIA GPU。`nm` 没有找到 `optixQueryFunctionTable` 时，继续执行下面的提取步骤。

### 2. 提取 NVIDIA Linux driver

下面以 `610.43.02` 为例。实际使用时应选择与 Windows 主机驱动兼容的 Linux driver 版本，并从 NVIDIA 官方 driver 下载页面获取对应的 `.run` 文件。

```bash
export OPTIX_DRIVER_VERSION=610.43.02
export OPTIX_DRIVER_ROOT=/tmp/g4go-optix-${OPTIX_DRIVER_VERSION}
export OPTIX_DRIVER_FILE=NVIDIA-Linux-x86_64-${OPTIX_DRIVER_VERSION}-no-compat32.run

mkdir -p "$OPTIX_DRIVER_ROOT"
cd "$OPTIX_DRIVER_ROOT"
wget -O "$OPTIX_DRIVER_FILE" \
  "https://us.download.nvidia.com/XFree86/Linux-x86_64/${OPTIX_DRIVER_VERSION}/${OPTIX_DRIVER_FILE}"
chmod +x "$OPTIX_DRIVER_FILE"
sh "$OPTIX_DRIVER_FILE" -x
```

提取完成后，目录通常为：

```text
/tmp/g4go-optix-610.43.02/NVIDIA-Linux-x86_64-610.43.02/
```

### 3. 安装用户态 OptiX runtime

将完整的 OptiX、RTCore、PTX JIT 和 GPU compiler 库复制到用户目录。用户目录便于管理多个驱动版本，也避免修改系统文件。

```bash
export OPTIX_DRIVER_DIR="$OPTIX_DRIVER_ROOT/NVIDIA-Linux-x86_64-${OPTIX_DRIVER_VERSION}"
export G4GO_OPTIX_RUNTIME="$HOME/.local/share/g4go/optix/${OPTIX_DRIVER_VERSION}"

mkdir -p "$G4GO_OPTIX_RUNTIME"
cp "$OPTIX_DRIVER_DIR"/libnvoptix.so.* \
   "$OPTIX_DRIVER_DIR"/libnvidia-rtcore.so.* \
   "$OPTIX_DRIVER_DIR"/libnvidia-ptxjitcompiler.so.* \
   "$OPTIX_DRIVER_DIR"/libnvidia-gpucomp.so.* \
   "$OPTIX_DRIVER_DIR"/nvoptix.bin \
   "$G4GO_OPTIX_RUNTIME"/

ln -sfn "libnvoptix.so.${OPTIX_DRIVER_VERSION}" \
  "$G4GO_OPTIX_RUNTIME/libnvoptix.so.1"
ln -sfn "libnvidia-ptxjitcompiler.so.${OPTIX_DRIVER_VERSION}" \
  "$G4GO_OPTIX_RUNTIME/libnvidia-ptxjitcompiler.so.1"
```

`libnvidia-rtcore.so.${OPTIX_DRIVER_VERSION}` 保持版本化文件名。`libnvoptix.so.1` 和 `libnvidia-ptxjitcompiler.so.1` 通过软链接指向对应版本。

### 4. 配置动态库搜索路径

完整的 OptiX runtime 必须排在 `/usr/lib/wsl/lib` 前面，这样程序会优先加载提取出的完整 `libnvoptix.so.1`，同时继续从 WSL 映射目录加载 CUDA 驱动库。

将下面内容加入 `~/.bashrc`：

```bash
# G4GO OptiX runtime
export G4GO_OPTIX_RUNTIME="$HOME/.local/share/g4go/optix/610.43.02"
if [ -d "$G4GO_OPTIX_RUNTIME" ]; then
  case ":${LD_LIBRARY_PATH:-}:" in
    *":$G4GO_OPTIX_RUNTIME:"*) ;;
    *) export LD_LIBRARY_PATH="$G4GO_OPTIX_RUNTIME:/usr/lib/wsl/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
  esac
fi
```

重新加载 shell：

```bash
source ~/.bashrc
echo "$G4GO_OPTIX_RUNTIME"
echo "$LD_LIBRARY_PATH" | tr ':' '\n' | head
```

`G4GO_OPTIX_RUNTIME` 应位于 `LD_LIBRARY_PATH` 第一项。修改 Windows 驱动或 WSL 映射后，可以在 PowerShell 中执行下面的命令重建 WSL2 的运行时缓存，然后重新打开 WSL：

```powershell
wsl --shutdown
```

### 5. 验证 OptiX 库和 G4GO

先验证动态库导出的入口和依赖：

```bash
nm -D "$G4GO_OPTIX_RUNTIME/libnvoptix.so.1" \
  | rg optixQueryFunctionTable

LD_LIBRARY_PATH="$G4GO_OPTIX_RUNTIME:/usr/lib/wsl/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ldd "$G4GO_OPTIX_RUNTIME/libnvoptix.so.1" \
  | rg 'nvoptix|rtcore|ptxjit|gpucomp|not found'
```

然后重新配置、构建并显式运行 OptiX backend：

```bash
cmake -S . -B build \
  -DG4GO_BUILD_UIVIS=OFF \
  -DG4GO_ENABLE_OPTIX=ON
cmake --build build -j

./build/g4go --backend gpu build/run_optical_smoke.mac
```

成功时应看到：

```text
[g4go] optical backend: gpu, ...
```

显式使用 `--backend gpu` 时，OptiX 初始化失败会终止程序并显示原因。`--backend auto` 会在 OptiX 初始化失败时回退到 Geant4，因此 GPU 环境验证阶段应使用显式的 `gpu` 参数。

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

```

`simulation` 保存 Geant4 action，`detector` 保存几何、材料和 sensitive detector，`optical` 保存场景、OptiX 传输以及 Geant4 bridge。材料和光学表面定义集中在 `DetectorConstruction.cpp` 中；模块内部辅助实现放在对应目录中，例如 Geant4 场景导出器 `SceneExporter`。

CMake 将模块编译为内部静态库：`g4go_optical_core`、可选的 `g4go_optical_optix`、`g4go_optical_geant4`、`g4go_detector` 和 `g4go_simulation`。最终 `g4go` 目标只编译 `src/main.cpp`，依赖关系由模块库表达。

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
./build/g4go --backend cpu --threads 1 build/run_smoke.mac
./build/g4go --backend gpu build/run_smoke.mac
```

命令行选项：

- `--backend auto|cpu|gpu`：选择传输后端，默认 `auto`。
- `auto` 优先尝试 OptiX；OptiX 构建依赖或运行时驱动不可用时切换到 Geant4。
- 显式指定 `--backend gpu` 时，OptiX 初始化失败会报告错误并终止，便于定位 GPU 环境问题。
- `--seed UINT64`：设置 Geant4 和 OptiX 的随机种子，默认 `42`。
- `--threads UINT32`：Geant4 worker 数量；默认使用 `G4Threading::G4GetNumberOfCores()` 的结果，显式传入该参数可以覆盖默认值。`gpu` backend 使用这些 worker 并行产生事件，GPU 光学传输由共享批处理服务异步执行。
- `--batch-photons UINT32`：GPU 批次的目标光子数，默认 `1'000'000`。批次也受事件数量和等待时间限制，运行结束时会排空队列。
- `--max-photons UINT32`：单事件 photon capture 上限，默认 `5'000'000`。
- `--max-bounces UINT32`：单 photon 最大边界交互次数，默认 `4096`。
- `--mesh-rotation-steps UINT32`：`G4Polyhedron` mesh 转换的旋转采样数，默认 `360`，有效范围为 `8` 到 `4096`。

## Backend 结构

GPU backend 采用事件聚合的异步流水线。Geant4 worker 只负责产生和捕获本事件的光子，并将事件提交到共享的 MPSC 队列；独立的 GPU executor 线程把多个事件合并到约 `1'000'000` 个光子的批次后调用一次 OptiX。结果通过事件 ID 分发回原 worker，由该 worker 完成 ROOT ntuple 写入；队列容量、批次目标光子数和批次等待窗口共同提供背压，运行结束时排空所有未完成事件。

| Backend | 光学光子处理 | 光学传输 | 结果写入 |
| --- | --- | --- | --- |
| `auto` | OptiX 时捕获；失败时由 Geant4 跟踪 | OptiX，失败时 Geant4 | `SensorHC` / `SensorHit` |
| `cpu` | 由 Geant4 跟踪 | Geant4 | Geant4 SD |
| `gpu` | 捕获后终止 Geant4 光子 | OptiX 9.1 | `SensorHC` / `SensorHit` |

Geant4 是 geometry truth source。scene exporter 使用每个 solid 的 `CreatePolyhedron()` 生成 triangle mesh，保存 Geant4 的 placement、material、surface、copy number 和 parent topology；相同 `G4VSolid` 的 mesh 可以复用，physical volume 仍然独立建立 OptiX instance。OptiX 只负责 triangle traversal 和求交，G4GO 负责拓扑解析、光学边界过程和 hit 生成。当前默认 mesh 后端覆盖能生成 polyhedron 的 Geant4 solid，解析 primitive 求交保留给后续性能优化。

`--mesh-rotation-steps 360` 是 correctness-first 默认值。复杂几何正式 profiling 时应比较 `90/180/360/720` 对 mesh 误差、triangle 数量、GAS build time、显存和 transport time 的影响。

边界 surface 查找遵循 directed border surface、daughter skin surface、current skin surface 的优先级。参数化和 replica volume 按 copy 展开为独立的 mesh instance，并通过 touchable history 保留 copy-aware photon volume key；参数化 solid 和 material 也会逐 copy 求值。

无显式 optical surface 的透明材料边界使用包含 S/P 偏振分量的 Fresnel 反射、折射和全反射计算；下一材料缺少 `RINDEX` 时按非透明边界吸收。显式 surface 先由 `REFLECTIVITY/TRANSMITTANCE` 分类为 absorption、direct transmission 或 surface-specific interaction；进入 surface-specific interaction 后再根据 dielectric type、surface model 和 finish 执行 Fresnel 或金属反射。`EFFICIENCY` 仅在 absorption 路径中决定 Detection 或普通 Absorption。polished surface 采用镜面反射，ground surface 采用 Lambertian 漫反射。达到 `--max-bounces` 的 photon 单独计入 `truncated`，不会混入物理吸收统计。

GPU scene export 会拒绝当前未实现的 Rayleigh、Mie、WLS、LUT/DAVIS/dichroic/coated surface type 或 model，以及 painted finish，避免忽略配置后继续产生结果。GLISUR 的 polish、UNIFIED 的 sigma-alpha、specular lobe/spike/backscatter 参数会参与 surface-specific interaction；基础 surface metadata 和 `TRANSMITTANCE` 已纳入 scene/device 数据结构。

OptiX backend 将 device program 编译为 OptiX IR，使用每个唯一 mesh geometry 的 triangle GAS 和 volume instance 的 IAS。正常边界解析使用一次 closest-hit；标记为可能存在 sibling/touching 歧义的 volume 才进行第二次 `[t0-tolerance,t0+tolerance]` any-hit candidate trace，并按 parent topology 排序。transport 使用 triangle winding 的 face normal，同时用于 entering/leaving、Fresnel、反射/折射和 boundary offset；G4Polyhedron normal 只参与导出时的 winding 校验。每个 photon 对应一个 raygen launch slot，hit 结果通过稳定的 photon slot 回读，并携带 `eventID` 和 `photonID`，随机数使用 host/device 一致的 Philox4x32-10。GPU 光子与 hit buffer 在连续批次之间复用。

## ROOT 输出

ROOT 文件包含两个 tree，名称采用 PascalCase 的记录类型命名，branch 使用 lowerCamelCase；已有的 `Edep` 和 `nOptPho` 保持约定名称。

| Tree | Branches |
| --- | --- |
| `CrystalHit` | `eventID`, `moduleID`, `Edep`, `nOptPho` |
| `SensorHit` | `eventID`, `sensorID`, `timeOfFlight` |

## nOptPho CPU/GPU 回归测试

回归测试直接使用 `scripts/source_beam.mac`。shell 编排器按照 CPU、GPU、ROOT 比较宏的顺序执行，并将每次运行的输出保存到 `build/regression/noptpho_<timestamp>/`。

```bash
cmake --build build -j
ctest --test-dir build -V -R '^g4go_noptpho_regression$'
```

测试使用 `test/TestOpticalTransport.C` 比较 `CrystalHit.nOptPho` 谱。ROOT 宏使用包含 overflow bin 的未加权 `Chi2TestX(..., "UU P OF")`，比较区间使用 `[0,10,20,30,40,50,75,100,250]` 分段，并在最大值超过 250 时扩展到 `max(nOptPho)+1`；图形继续保留 200 个 bin。宏将 p-value 换算为双侧 Gaussian significance，并按 Z 值输出 `FAILED`、`SUSPICIOUS`、`PASSED` 或 `IDENTICAL`；卡方统计条件无效时输出 `INVALID`。`Z > 5`、卡方条件无效或 CPU/GPU 的 `nOptPho` 总量相对差异超过 10% 时测试失败；`3 < Z <= 5` 标记为可疑，其余有效状态通过测试。测试同时生成以下文件：

- `noptpho_cpu.root` 和 `noptpho_gpu.root`：CPU/GPU 输入结果。
- `noptpho_comparison.png`：200 个 bin 的原始计数 CPU/GPU 谱和 pull 图，图例中标注 GPU 加速比。
- `noptpho_regression_report.root`：ROOT 直方图、卡方统计量、运行时间和 `gpuSpeedup` 参数。
- `regression_result.txt`：文本化回归结果，包含 CPU/GPU 墙钟时间和 `gpu_speedup`。
- `cpu.log`、`gpu.log` 和 `regression.log`：执行日志。

GPU 加速比使用 CPU 回归运行墙钟时间除以 GPU 回归运行墙钟时间计算，表达为 `CPU wall time / GPU wall time`。所有文件位于同一次运行的 `build/regression/noptpho_<timestamp>/` 目录。

测试显式使用 `--backend gpu`。GPU、OptiX runtime、ROOT 或驱动不可用时测试直接失败，并保留时间戳工作目录中的诊断文件。

## 验证

```bash
cmake --build build --target g4go-format-check
ctest --test-dir build --output-on-failure
rootls -t build/run_smoke.root
```

`g4go_optical_boundary` 专项测试覆盖正入射 Fresnel、Brewster 角、全反射、显式表面概率和 Lambertian 反射方向。

`build/run_optical_smoke.mac` 使用一个从 core 内部发射的 deterministic optical photon，适合验证初始化、OptiX transport、SensorHC 和 ROOT 输出。`build/run_smoke.mac` 保留一个 gamma event 的源链路 smoke，gamma 是否发生能量沉积由 Geant4 随机过程决定；`build/run_regression.mac` 保留原有 1000 event 基线。
`build/run_optical_benchmark.mac` 使用单个 run 中的 1000 个直接光子，用于检查 OptiX 的传输统计。

OptiX 的编译验证覆盖 device IR、IR embedding 和 host pipeline 链接。`auto` backend 需要驱动提供完整的 `libnvoptix.so.1` API；部分 WSL 环境只提供 loader shim，缺少 `optixQueryFunctionTable`，此时运行阶段会自动切换到 Geant4。显式使用 `--backend gpu` 时会报告驱动能力不足。
