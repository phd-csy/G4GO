#!/usr/bin/env bash

set -u -o pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "${script_dir}/../CMakeLists.txt" ]]; then
    project_dir="$(cd -- "${script_dir}/.." && pwd)"
else
    project_dir="$(cd -- "${script_dir}/../.." && pwd)"
fi

if [[ -n "${G4GO_BUILD_DIR:-}" ]]; then
    build_dir="${G4GO_BUILD_DIR}"
elif [[ -r "${script_dir}/../CMakeCache.txt" ]]; then
    build_dir="$(cd -- "${script_dir}/.." && pwd)"
else
    build_dir="${project_dir}/build"
fi
if [[ -r "${build_dir}/CMakeCache.txt" ]]; then
    cmake_home_directory="$(sed -n \
        's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${build_dir}/CMakeCache.txt" | sed -n '1p')"
    if [[ -n "${cmake_home_directory}" && -f "${cmake_home_directory}/CMakeLists.txt" ]]; then
        project_dir="${cmake_home_directory}"
    fi
fi
minimum_cmake_version="3.24"
minimum_cuda_version="12.8"
minimum_geant4_version="11.4.2"
minimum_driver_major=590
required_optix_version="9.1"
expected_cuda_architecture="120"
failures=0

usage() {
    printf '%s\n' \
        "Usage: $0 [-b DIR] [-h]" \
        "       $0 [--build-dir DIR] [--help]" \
        "" \
        "Check the G4GO build and runtime environment." \
        "  -b, --build-dir DIR  Build directory to inspect" \
        "  -h, --help           Show this help"
}

while (($# > 0)); do
    case "$1" in
    -b|--build-dir)
        (($# >= 2)) || { printf 'error: missing value for --build-dir\n' >&2; exit 1; }
        build_dir="$2"
        shift 2
        ;;
    -b=*|--build-dir=*)
        build_dir="${1#*=}"
        shift
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        printf 'error: unknown argument: %s\n' "$1" >&2
        usage >&2
        exit 1
        ;;
    esac
done

version_at_least() {
    local current="$1"
    local required="$2"
    local current_major current_minor current_patch
    local required_major required_minor required_patch

    IFS=. read -r current_major current_minor current_patch _ <<<"${current}"
    IFS=. read -r required_major required_minor required_patch _ <<<"${required}"
    current_major="${current_major:-0}"
    current_minor="${current_minor:-0}"
    current_patch="${current_patch:-0}"
    required_major="${required_major:-0}"
    required_minor="${required_minor:-0}"
    required_patch="${required_patch:-0}"

    ((current_major > required_major ||
      (current_major == required_major && current_minor > required_minor) ||
      (current_major == required_major && current_minor == required_minor &&
       current_patch >= required_patch)))
}

first_line() {
    sed -n '1p'
}

print_row() {
    printf '%-15s %-10s %s\n' "$1" "$2" "$3"
}

record_row() {
    local label="$1"
    local value="$2"
    local status="$3"
    print_row "${label}" "${value}" "${status}"
    if [[ "${status}" != "OK" ]]; then
        failures=$((failures + 1))
    fi
}

geant4_config="$(command -v geant4-config || true)"
if [[ -z "${geant4_config}" ]] && [[ -r "${build_dir}/CMakeCache.txt" ]]; then
    geant4_dir="$(sed -n 's/^Geant4_DIR:PATH=//p' "${build_dir}/CMakeCache.txt" |
        first_line)"
    if [[ -n "${geant4_dir}" &&
          -x "${geant4_dir%/lib/cmake/Geant4}/bin/geant4-config" ]]; then
        geant4_config="${geant4_dir%/lib/cmake/Geant4}/bin/geant4-config"
    fi
fi
geant4_version=""
if [[ -n "${geant4_config}" ]]; then
    geant4_version="$(${geant4_config} --version 2>/dev/null | first_line)"
fi
if [[ -n "${geant4_version}" ]] &&
   version_at_least "${geant4_version}" "${minimum_geant4_version}"; then
    record_row "Geant4" "${geant4_version}" "OK"
elif [[ -n "${geant4_version}" ]]; then
    record_row "Geant4" "${geant4_version}" "FAIL"
else
    record_row "Geant4" "missing" "FAIL"
fi

cmake_version=""
cmake_executable="$(command -v cmake || true)"
if [[ -n "${cmake_executable}" ]]; then
    cmake_version="$(${cmake_executable} --version 2>/dev/null |
        first_line | sed -nE 's/.*version[[:space:]]+([0-9]+(\.[0-9]+){1,2}).*/\1/p')"
fi
if [[ -n "${cmake_version}" ]] &&
   version_at_least "${cmake_version}" "${minimum_cmake_version}"; then
    record_row "CMake" "${cmake_version}" "OK"
elif [[ -n "${cmake_version}" ]]; then
    record_row "CMake" "${cmake_version}" "FAIL"
else
    record_row "CMake" "missing" "FAIL"
fi

nvcc_executable="$(command -v nvcc || true)"
bin2c_executable="$(command -v bin2c || true)"
if [[ -z "${bin2c_executable}" && -n "${nvcc_executable}" ]]; then
    bin2c_candidate="$(dirname "${nvcc_executable}")/bin2c"
    if [[ -x "${bin2c_candidate}" ]]; then
        bin2c_executable="${bin2c_candidate}"
    fi
fi
cuda_version=""
if [[ -n "${nvcc_executable}" ]]; then
    cuda_version="$(${nvcc_executable} --version 2>/dev/null |
        sed -nE 's/.*release[[:space:]]+([0-9]+\.[0-9]+).*/\1/p' | first_line)"
fi
if [[ -n "${cuda_version}" && -n "${nvcc_executable}" &&
      -n "${bin2c_executable}" ]] &&
   version_at_least "${cuda_version}" "${minimum_cuda_version}"; then
    record_row "CUDA" "${cuda_version}" "OK"
elif [[ -n "${cuda_version}" ]]; then
    record_row "CUDA" "${cuda_version}" "FAIL"
else
    record_row "CUDA" "missing" "FAIL"
fi
if [[ -n "${nvcc_executable}" ]]; then
    record_row "nvcc" "" "OK"
else
    record_row "nvcc" "" "FAIL"
fi

nvidia_smi="$(command -v nvidia-smi || true)"
gpu_name=""
compute_capability=""
driver_version=""
if [[ -n "${nvidia_smi}" ]]; then
    gpu_name="$(${nvidia_smi} --query-gpu=name --format=csv,noheader,nounits 2>/dev/null |
        first_line)"
    compute_capability="$(${nvidia_smi} --query-gpu=compute_cap --format=csv,noheader,nounits 2>/dev/null |
        first_line)"
    driver_version="$(${nvidia_smi} --query-gpu=driver_version --format=csv,noheader,nounits 2>/dev/null |
        first_line)"
fi
gpu_name="${gpu_name#NVIDIA GeForce }"
gpu_name="${gpu_name#NVIDIA }"
if [[ -n "${gpu_name}" ]]; then
    record_row "GPU" "${gpu_name}" "OK"
else
    record_row "GPU" "missing" "FAIL"
fi

configured_architecture="${G4GO_CUDA_ARCHITECTURE:-}"
if [[ -z "${configured_architecture}" && -r "${build_dir}/CMakeCache.txt" ]]; then
    configured_architecture="$(sed -n \
        's/^G4GO_CUDA_ARCHITECTURE:STRING=//p' "${build_dir}/CMakeCache.txt" |
        first_line)"
fi
if [[ -z "${configured_architecture}" ]]; then
    configured_architecture="${expected_cuda_architecture}"
fi
configured_architecture="${configured_architecture%%-*}"
expected_compute_capability=""
if [[ "${configured_architecture}" =~ ^([0-9]+)([0-9])$ ]]; then
    expected_compute_capability="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}"
fi
if [[ -n "${compute_capability}" &&
      "${compute_capability}" == "${expected_compute_capability}" ]]; then
    record_row "compute cap" "${compute_capability}" "OK"
elif [[ -n "${compute_capability}" ]]; then
    record_row "compute cap" "${compute_capability}" "FAIL"
else
    record_row "compute cap" "missing" "FAIL"
fi

driver_major="${driver_version%%.*}"
if [[ "${driver_major}" =~ ^[0-9]+$ ]] &&
   ((driver_major >= minimum_driver_major)); then
    record_row "Driver" "${driver_version}" "OK"
elif [[ -n "${driver_version}" ]]; then
    record_row "Driver" "${driver_version}" "FAIL"
else
    record_row "Driver" "missing" "FAIL"
fi

optix_include_dir=""
optix_include_candidates=(
    "${G4GO_OPTIX_HEADERS_DIR:-}"
    "${build_dir}/_deps/optix_dev-src/include"
    "${project_dir}/build/_deps/optix_dev-src/include"
    "${OPTIX_ROOT:-}/include"
    "/opt/optix/include"
    "/usr/local/optix/include"
)
for candidate in "${optix_include_candidates[@]}"; do
    if [[ -f "${candidate}/optix.h" &&
          -f "${candidate}/optix_stubs.h" &&
          -f "${candidate}/optix_function_table_definition.h" ]]; then
        optix_include_dir="${candidate}"
        break
    fi
done

optix_version=""
if [[ -n "${optix_include_dir}" ]]; then
    optix_version_number="$(sed -nE \
        's/^[[:space:]]*#define[[:space:]]+OPTIX_VERSION[[:space:]]+([0-9]+).*/\1/p' \
        "${optix_include_dir}/optix.h" | first_line)"
    if [[ "${optix_version_number}" =~ ^[0-9]+$ ]]; then
        optix_version="$((optix_version_number / 10000)).$(((optix_version_number / 100) % 100))"
    fi
fi
if [[ "${optix_version}" == "${required_optix_version}" ]]; then
    record_row "OptiX headers" "${optix_version}" "OK"
elif [[ -n "${optix_version}" ]]; then
    record_row "OptiX headers" "${optix_version}" "FAIL"
else
    record_row "OptiX headers" "missing" "FAIL"
fi

optix_runtime="${G4GO_OPTIX_RUNTIME_LIBRARY:-}"
if [[ -z "${optix_runtime}" ]]; then
    optix_runtime="$(ldconfig -p 2>/dev/null |
        awk '/libnvoptix\.so/{print $NF; exit}')"
fi
if [[ -z "${optix_runtime}" ]]; then
    for candidate in \
        /usr/lib/wsl/lib/libnvoptix.so* \
        /usr/lib/x86_64-linux-gnu/libnvoptix.so* \
        /usr/lib/libnvoptix.so*; do
        if [[ -f "${candidate}" || -L "${candidate}" ]]; then
            optix_runtime="${candidate}"
            break
        fi
    done
fi

optix_runtime_status=1
if [[ -n "${optix_runtime}" && -r "${optix_runtime}" ]]; then
    if command -v python3 >/dev/null 2>&1; then
        if python3 - "${optix_runtime}" >/dev/null 2>&1 <<'PY'
import ctypes
import sys

ctypes.CDLL(sys.argv[1])
PY
        then
            optix_runtime_status=0
        fi
    else
        optix_runtime_status=0
    fi
fi
if ((optix_runtime_status == 0)); then
    record_row "OptiX runtime" "" "OK"
else
    record_row "OptiX runtime" "missing" "FAIL"
fi

root_executable="$(command -v root || true)"
root_config="$(command -v root-config || true)"
root_version=""
if [[ -n "${root_config}" ]]; then
    root_version="$(${root_config} --version 2>/dev/null | first_line)"
fi
if [[ -z "${root_version}" && -n "${root_executable}" ]]; then
    root_version="$(${root_executable} --version 2>/dev/null |
        sed -nE 's/.*ROOT[[:space:]]+([0-9]+(\.[0-9]+){1,2}).*/\1/p' | first_line)"
fi
if [[ -n "${root_executable}" && -n "${root_version}" ]]; then
    record_row "ROOT" "${root_version}" "OK"
else
    record_row "ROOT" "missing" "FAIL"
fi

if ((failures > 0)); then
    exit 1
fi
