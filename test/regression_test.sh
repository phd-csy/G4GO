#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
build_dir="${project_dir}/build"
threads=""

while (($# > 0)); do
    case "$1" in
    --build-dir)
        if (($# < 2)); then
            echo "Missing value for --build-dir" >&2
            exit 1
        fi
        build_dir="$2"
        shift 2
        ;;
    --threads)
        if (($# < 2)); then
            echo "Missing value for --threads" >&2
            exit 1
        fi
        threads="$2"
        shift 2
        ;;
    -h|--help)
        echo "Usage: $0 [--build-dir BUILD_DIR] [--threads N]"
        exit 0
        ;;
    *)
        echo "Unknown argument: $1" >&2
        exit 1
        ;;
    esac
done

if [[ ! -d "$build_dir" ]]; then
    echo "Build directory does not exist: $build_dir" >&2
    exit 1
fi
build_dir="$(cd -- "$build_dir" && pwd)"

g4go="${build_dir}/g4go"
source_macro="${project_dir}/scripts/source_beam.mac"
compare_macro="${script_dir}/TestOpticalTransport.C"
root_executable="$(command -v root || true)"

if [[ ! -x "$g4go" ]]; then
    echo "Executable is missing or not executable: $g4go" >&2
    exit 1
fi
if [[ ! -f "$source_macro" ]]; then
    echo "Source macro is missing: $source_macro" >&2
    exit 1
fi
if [[ ! -f "$compare_macro" ]]; then
    echo "ROOT comparison macro is missing: $compare_macro" >&2
    exit 1
fi
if [[ -z "$root_executable" ]]; then
    echo "ROOT executable was not found in PATH" >&2
    exit 1
fi

if [[ -z "$threads" ]]; then
    if command -v lscpu >/dev/null 2>&1; then
        physical_cores="$(
            lscpu -p=CORE,SOCKET |
                awk '!/^#/ && NF {print}' |
                sort -u |
                wc -l
        )"
    else
        physical_cores="$(nproc 2>/dev/null || echo 1)"
    fi
    if [[ ! "$physical_cores" =~ ^[1-9][0-9]*$ ]]; then
        physical_cores=1
    fi
    if ((physical_cores > 8)); then
        threads=8
    else
        threads="$physical_cores"
    fi
fi

if [[ ! "$threads" =~ ^[1-9][0-9]*$ ]]; then
    echo "--threads must be a positive integer: $threads" >&2
    exit 1
fi

timestamp="$(date --utc +%Y%m%d-%H%M%S)-$$"
test_dir="${build_dir}/regression/noptpho_${timestamp}"
mkdir -p "${build_dir}/regression"
if ! mkdir "$test_dir"; then
    echo "Unable to create regression directory: $test_dir" >&2
    exit 1
fi
cd "$test_dir" || exit 1

start_time="$(date +%s)"
exec > >(tee -a regression.log) 2>&1

print_summary() {
    local end_time total_time hours minutes seconds
    end_time="$(date +%s)"
    total_time=$((end_time - start_time))
    hours=$((total_time / 3600))
    minutes=$(((total_time % 3600) / 60))
    seconds=$((total_time % 60))
    echo "################################################################################"
    echo "# Start at: $(date --iso-8601=seconds -d "@$start_time")"
    echo "# End at: $(date --iso-8601=seconds -d "@$end_time")"
    echo "# Total running time: ${hours}h ${minutes}m ${seconds}s"
    echo "# Details in: $test_dir"
    echo "################################################################################"
}

fail_test() {
    local exit_code="$1"
    echo "# Regression test failed with exit code $exit_code"
    print_summary
    exit "$exit_code"
}

run_logged() {
    local log_file="$1"
    shift
    "$@" 2>&1 | tee "$log_file"
    local command_status="${PIPESTATUS[0]}"
    return "$command_status"
}

run_command() {
    local command_text command_status
    printf -v command_text '%q ' "$@"
    echo "################################################################################"
    echo "# Executing ${command_text}"
    echo "################################################################################"
    "$@"
    command_status="$?"
    if ((command_status == 0)); then
        echo "# Command completed successfully"
        return 0
    fi
    echo "# Command failed with exit code $command_status"
    fail_test "$command_status"
}

now_ns() {
    date +%s%N
}

seconds_from_ns() {
    awk -v nanoseconds="$1" 'BEGIN {printf "%.6f", nanoseconds / 1000000000.0}'
}

echo "Working directory: $(pwd)"
echo "Using CPU threads: $threads"

cpu_start_ns="$(now_ns)"
run_command run_logged cpu.log "$g4go" \
    --backend cpu \
    --threads "$threads" \
    --seed 42 \
    "$source_macro"
cpu_elapsed_seconds="$(seconds_from_ns "$(( $(now_ns) - cpu_start_ns ))")"
echo "CPU wall time: ${cpu_elapsed_seconds} s"
run_command test -s source_beam.root
run_command mv source_beam.root noptpho_cpu.root

gpu_start_ns="$(now_ns)"
run_command run_logged gpu.log "$g4go" \
    --backend gpu \
    --seed 42 \
    "$source_macro"
gpu_elapsed_seconds="$(seconds_from_ns "$(( $(now_ns) - gpu_start_ns ))")"
echo "GPU wall time: ${gpu_elapsed_seconds} s"
run_command grep -F "[g4go] optical backend: gpu" gpu.log
run_command test -s source_beam.root
run_command mv source_beam.root noptpho_gpu.root

compare_call="${compare_macro}(\"${test_dir}/noptpho_cpu.root\",\"${test_dir}/noptpho_gpu.root\",${cpu_elapsed_seconds},${gpu_elapsed_seconds},\"${test_dir}\")"
run_command "$root_executable" -l -b -q "$compare_call"
run_command test -s regression_result.txt
run_command test -s noptpho_comparison.png
run_command test -s noptpho_regression_report.root

echo "All commands completed successfully"
print_summary
