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

G4GO uses Geant4 to describe particle interactions, detector geometry, and materials, and CUDA/OptiX to accelerate the most expensive part: optical-photon transport. The current detector is a single crystal coupled to a SiPM. Energy deposition, generated and detected photon counts, and photon arrival times are written to ROOT.

The same executable supports three optical backends:

| Backend | How optical photons are handled | When to use it |
| --- | --- | --- |
| `auto` | Use OptiX when it is included in the build; otherwise use Geant4 | Normal runs and quick checks |
| `cpu` | Track every optical photon with Geant4 | Generate a CPU reference or run without an NVIDIA GPU |
| `gpu` | Generate photons in Geant4 and transport them in CUDA/OptiX batches | GPU validation, regression, and performance measurements |

## Start from a clean checkout

The commands below start in the repository root. After the build is complete, runtime commands are executed from `build/` so that macro, test, and output paths remain consistent.

### Step 1: Install the dependencies

A CPU build requires:

- CMake 3.24 or newer;
- a compiler with C++20 support;
- Geant4 11.4.2;
- Git and network access, because CMake fetches CLI11 during the first configure step.

The GPU backend also requires:

- an NVIDIA GPU and an R590 or newer driver;
- CUDA Toolkit 12.8 or newer, including `nvcc` and `bin2c`;
- a CUDA architecture matching the GPU compute capability. CMake fetches the OptiX 9.1 headers during the first configure step, while the NVIDIA driver provides the OptiX runtime.

ROOT is not needed to build the main executable. Its command-line tools are required for CPU/GPU regression and for inspecting ROOT output.

If Geant4 is installed in a custom location, load its environment first, for example:

```bash
source /path/to/geant4/bin/geant4.sh
```

### Step 2: Check the current environment

```bash
./test/environment.sh
```

The script lists the detected Geant4, CMake, CUDA, GPU, driver, OptiX, and ROOT versions and marks each current project requirement as `OK` or `FAIL`. GPU, CUDA, and OptiX failures can be ignored when only the CPU backend is needed.

### Step 3: Choose a build

For a GPU build, first select the CUDA Toolkit that CMake should use:

```bash
export CUDA_HOME=/usr/local/cuda
export CUDACXX="${CUDA_HOME}/bin/nvcc"
export PATH="${CUDA_HOME}/bin:${PATH}"
nvcc --version
```

Then configure and build. The default architecture is `120`; change the value below to the target GPU compute capability when necessary. For example, an RTX 4090 uses `89`:

```bash
cmake -S . -B build \
  -DG4GO_BUILD_UIVIS=OFF \
  -DG4GO_ENABLE_OPTIX=ON \
  -DG4GO_CUDA_ARCHITECTURE=120 \
  -DCMAKE_CUDA_COMPILER="${CUDACXX}"
cmake --build build -j
```

For a Geant4-only CPU build:

```bash
cmake -S . -B build \
  -DG4GO_BUILD_UIVIS=OFF \
  -DG4GO_ENABLE_OPTIX=OFF
cmake --build build -j
```

CMake remembers the CUDA compiler after the first configure step. Use a new build directory or clear the old CMake cache when switching CUDA Toolkits.

Common configuration options are:

| CMake option | Default | Purpose |
| --- | --- | --- |
| `G4GO_BUILD_UIVIS` | `ON` | Build Geant4 UI and visualization support |
| `G4GO_ENABLE_OPTIX` | `ON` | Try to build the CUDA/OptiX backend |
| `G4GO_CUDA_ARCHITECTURE` | `120` | CUDA architecture for the OptiX device program |
| `G4GO_ENABLE_CLANG_TIDY` | `OFF` | Enable clang-tidy during compilation |
| `BUILD_TESTING` | `ON` | Build and register CTest tests |

### Step 4: Verify that the executable runs

Start with the smoke macro, which emits one optical photon:

```bash
cd build
./g4go --backend auto scripts/run_test_optics.mac
```

Run each backend explicitly when it needs to be verified:

```bash
./g4go --backend cpu scripts/run_test_optics.mac
./g4go --backend gpu scripts/run_test_optics.mac
```

The `gpu` command reports an error directly when OptiX was not included in the build or runtime initialization fails, so it also provides a practical GPU environment check.

### Step 5: Run a simulation

The basic command is:

```text
g4go [OPTIONS] [macro]
```

For example, run the bundled electron-beam regression macro with six Geant4 workers on the CPU backend:

```bash
cd build
./g4go --backend cpu --threads 6 scripts/run_regression_eminus.mac
```

Switch to the GPU backend by changing one option:

```bash
./g4go --backend gpu --threads 6 scripts/run_regression_eminus.mac
```

The bundled macros serve these purposes:

| Macro | Purpose | Workload and output |
| --- | --- | --- |
| `scripts/run_test_optics.mac` | Single-photon smoke test | 1 event, no ROOT output |
| `scripts/run_regression_eminus.mac` | CPU/GPU correctness regression | 5,000 events, written to `run_regression_eminus/` |
| `scripts/run_benchmark_eminus.mac` | Full performance measurement | 50,000 events, written to `run_benchmark_eminus/` |
| `scripts/run_cpu_scaling.mac` | CPU scaling estimate | 5,000 events, written to `run_cpu_scaling/` |
| `scripts/vis.mac` | Interactive geometry and track display | Used by a UI/visualization build |

After changing runtime files in `scripts/` or `test/`, run `cmake -S . -B build` again to copy the current versions to `build/scripts/` and `build/test/`.

### Step 6: Find and inspect the output

An analysis macro creates a same-named output directory under the current working directory. G4GO disables ROOT ntuple merging, so a multithreaded run produces a set of worker files:

```text
build/run_regression_eminus/
├── run_regression_eminus_t0.root
├── run_regression_eminus_t1.root
└── ...
```

Inspect a file with ROOT:

```bash
rootls run_regression_eminus/run_regression_eminus_t0.root
```

Each ROOT file contains two trees:

| Tree | Branch | Content |
| --- | --- | --- |
| `CrystalHit` | `eventID`, `moduleID` | Event and crystal-module identifiers |
| `CrystalHit` | `Edep` | Energy deposited in the crystal |
| `CrystalHit` | `nOptPho` | Number of SiPM-detected photons in the event |
| `CrystalHit` | `nGenOptPho` | Number of optical photons generated in the event |
| `SensorHit` | `eventID` | Geant4 event ID |
| `SensorHit` | `sensorID` | Vector of SiPM copy numbers, one per detection |
| `SensorHit` | `timeOfFlight` | Vector of arrival times, one per detection |

`SensorHit` writes one row only for an event with at least one detection. The `sensorID` and `timeOfFlight` vectors have the same length, and elements at the same index describe the same detection.

## Common workflows

### Fast tests

After building, run the short unit and smoke tests first:

```bash
cd build
ctest --output-on-failure -L 'unit|smoke'
```

The current tests are `g4go_optical_boundary` and `g4go_smoke_auto`. The longer CPU/GPU regression has its own driver script.

### CPU/GPU correctness regression

```bash
cd build
./test/regression.sh --threads 6
```

The script generates or reuses a CPU reference with the same worker count, then runs `GPU-1T` and `GPU-6T` and compares `Edep`, `nOptPho`, TOF, and sensor occupancy. Stream complete logs with `--verbose`:

```bash
./test/regression.sh --threads 6 --verbose
```

Results are written to `build/test/regression/<timestamp>/`:

- `regression_summary.txt` contains the overall result and statistical gates;
- `figures/` contains comparison plots;
- `logs/` contains logs for each phase;
- `gpu_1t/` and `gpu_6t/` contain the GPU ROOT datasets from this run.

The CPU reference is cached in `build/test/regression/cpu_6t/`. The script regenerates it when the executable, macro, worker count, or event count changes.

### Performance benchmark

```bash
cd build
./test/benchmark.sh --threads 6
```

The benchmark measures CPU partial scaling, a full CPU run, a full GPU-1T run, and a full GPU-6T run, then calculates thread-matched speedups. It is dedicated to performance measurement and does not run ROOT correctness comparisons.

CPU scheduling is left to the operating system by default. Select physical cores near the GPU's NUMA node automatically with:

```bash
./test/benchmark.sh --threads 6 --cpu-affinity auto
```

A CPU list for `taskset` can also be provided directly:

```bash
./test/benchmark.sh --threads 6 --cpu-affinity 0-6
```

Results are written to `build/test/benchmark/<timestamp>/`. `benchmark_summary.txt` records the run configuration, wall time, GPU transport time, and speedups. Enable detailed CUDA, scheduler, and OptiX diagnostics with:

```bash
G4GO_PERF_DIAGNOSTICS=1 ./test/benchmark.sh --threads 6
```

This also creates `performance_summary.txt` in the same result directory. Speedups are paired as `GPU-1T` versus estimated `CPU-1T` and `GPU-6T` versus `CPU-6T`.

### Interactive visualization

Configure with `G4GO_BUILD_UIVIS=ON`, build, and start the executable from `build/`. The program loads `scripts/vis.mac` automatically:

```bash
cd build
./g4go
```

### Static analysis

The project provides two ways to run clang-tidy. To check each C++ file as it is compiled, enable the CMake integration:

```bash
cmake -S . -B build -DG4GO_ENABLE_CLANG_TIDY=ON
cmake --build build -j
```

This applies `.clang-tidy` to every project C++ target. CMake reports an error during configuration when `clang-tidy` is unavailable.

To check the entire project in one pass, install `clang-tidy` and `run-clang-tidy`, prepare a normal CMake build directory, and run:

```bash
cmake -S . -B build
./test/static_analysis.sh --check
```

The script uses `build/compile_commands.json` to analyze project C/C++ files. `--fix` additionally requires `clang-apply-replacements` and applies the stable automatic fixes configured in `.clang-tidy`; inspect the worktree before using it.

### Pre-commit formatting

`.pre-commit-config.yaml` configures a `clang-format` hook that checks and formats staged C/C++ files before a commit. Install and enable pre-commit with:

```bash
pipx install pre-commit
pre-commit install
```

The hook runs automatically on `git commit`. Its configured clang-format environment is downloaded on the first run. To check every file in the repository manually, run:

```bash
pre-commit run --all-files
```

This hook only runs clang-format. Continue to run clang-tidy through the CMake option or `test/static_analysis.sh` described above.

## Command-line options

| Option | Default | Description |
| --- | --- | --- |
| `-b, --backend auto\|cpu\|gpu` | `auto` | Select the optical backend |
| `-s, --seed UINT64` | `0` | Set the Geant4 and OptiX random seeds |
| `-t, --threads UINT32` | `0` | Number of Geant4 workers; `0` uses the detected CPU core count |
| `--batch-photons UINT32` | `1000000` | Target photon count per GPU batch |
| `--batch-timeout-ms UINT32` | `10` | Maximum wait for more events before launching a GPU batch |
| `--max-photons UINT32` | `5000000` | Maximum captured photons per event |
| `--max-bounces UINT32` | `4096` | Maximum boundary interactions per photon |
| `-d, --diagnostics` | off | Print optical-transport performance diagnostics |
| `--mesh-rotation-steps UINT32` | `360` | Geant4 polyhedron rotation samples, from 8 to 4096 |
| `macro` | — | Geant4 macro; required by a build without UI support |

Run `./g4go --help` for help generated by the current executable.

## Current detector model

The current geometry is a self-contained single-crystal detector cell:

- a `3 cm × 3 cm × 8 cm` `G4_CESIUM_IODIDE` crystal;
- a `0.1 mm` silicone-grease coupling layer followed by a `0.2 mm` epoxy window;
- an `8 × 8` SiPM array with each SiPM measuring `3 mm × 3 mm × 0.1 mm`;
- crystal refractive index, absorption length, scintillation spectrum, yield, and time constant, plus a photon-energy-dependent detection efficiency on the SiPM surface.

The current geometry, materials, and optical surfaces are defined in `src/detector/DetectorConstruction.cpp`. When changing the detector, verify that both Geant4 CPU transport and GPU scene export support the optical processes and surface models in use.

## Project layout and responsibilities

```text
G4GO/
├── include/g4go/          # Public headers
├── src/
│   ├── detector/          # Geometry, materials, and sensitive detectors
│   ├── optical/geant4/    # Event adapter, scene export, and batch scheduling
│   ├── optical/optix/     # OptiX host/device transport
│   └── simulation/        # Geant4 actions and ROOT analysis
├── scripts/               # Ready-to-run Geant4 macros
└── test/                  # Unit tests, regression, benchmark, and environment tools
```

Geant4 remains the source of truth for geometry and materials. When the GPU backend starts, the scene exporter converts Geant4 solids into OptiX meshes while preserving placements, materials, surfaces, copy numbers, and parent relationships. Geant4 continues to own non-optical particle physics and the event lifecycle.

The current GPU boundary transport supports the `glisur` and `unified` models, `polished` and `ground` finishes, `dielectric_metal` and `dielectric_dielectric` surfaces, polarized Fresnel reflection and refraction, total internal reflection, and common surface-probability properties. Unsupported configurations such as Rayleigh, Mie, WLS, LUT, DAVIS, dichroic and coated surfaces, and painted finishes produce an explicit error during scene export.

## OptiX troubleshooting on WSL2

On WSL2, the Windows host driver provides the OptiX runtime. The following error commonly means that `libnvoptix` in the Windows NVIDIA driver is too old:

```text
optixInit failed: OPTIX_ERROR_ENTRY_SYMBOL_NOT_FOUND
```

Inspect the current environment:

```bash
nvidia-smi
nvcc --version
nm -D /usr/lib/wsl/lib/libnvoptix.so.1 | rg optixQueryFunctionTable
```

If `optixQueryFunctionTable` is missing, update the Windows NVIDIA driver, run `wsl --shutdown` in PowerShell, reopen WSL, and retry the GPU smoke test. See the [CUDA on WSL User Guide](https://docs.nvidia.com/cuda/wsl-user-guide/index.html) for environment setup.

## Contributing

Include reproduction steps, the selected backend, Geant4/CUDA/OptiX versions, and validation results in issues and pull requests. Before committing, run the tests relevant to the change and at least:

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

## License

Copyright 2026 Siyuan Chen

This project is licensed under the [Apache License 2.0](LICENSE). See [LICENSE](LICENSE) for the complete terms.
