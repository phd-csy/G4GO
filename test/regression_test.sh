#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
build_dir="${project_dir}/build"
threads=""
verbose=0

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
    --verbose)
        verbose=1
        shift
        ;;
    -h|--help)
        echo "Usage: $0 [--build-dir BUILD_DIR] [--threads N] [--verbose]"
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
source_macro="${project_dir}/scripts/run_beam_eminus.mac"
determinism_source_macro="${project_dir}/scripts/run_optical_determinism.mac"
compare_macro="${script_dir}/TestOpticalTransport.cxx"
root_executable="$(command -v root || true)"

if [[ ! -x "$g4go" ]]; then
    echo "Executable is missing or not executable: $g4go" >&2
    exit 1
fi
if [[ ! -f "$source_macro" ]]; then
    echo "Source macro is missing: $source_macro" >&2
    exit 1
fi
if [[ ! -f "$determinism_source_macro" ]]; then
    echo "Determinism macro is missing: $determinism_source_macro" >&2
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
    threads="$physical_cores"
fi

if [[ ! "$threads" =~ ^[1-9][0-9]*$ ]]; then
    echo "--threads must be a positive integer: $threads" >&2
    exit 1
fi

timestamp="$(date --utc +%Y%m%d-%H%M%S)-$$"
test_dir="${build_dir}/test/regression/noptpho_${timestamp}"
mkdir -p "${build_dir}/test/regression"
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
    echo "[regression] start_time=$(date --iso-8601=seconds -d "@$start_time")"
    echo "[regression] end_time=$(date --iso-8601=seconds -d "@$end_time")"
    echo "[regression] total_wall_time=${hours}h${minutes}m${seconds}s"
    echo "[regression] artifacts=${test_dir}"
}

fail_test() {
    local exit_code="$1"
    echo "[regression] RESULT: FAILED exit_code=$exit_code"
    print_summary
    exit "$exit_code"
}

run_logged() {
    local log_file="$1"
    shift
    if ((verbose)); then
        "$@" 2>&1 | tee "$log_file"
        return "${PIPESTATUS[0]}"
    fi
    "$@" >"$log_file" 2>&1
}

run_command() {
    local command_text command_status
    "$@" >/dev/null 2>&1
    command_status="$?"
    if ((command_status != 0)); then
        printf -v command_text '%q ' "$@"
        echo "[regression] command failed exit_code=${command_status} command=${command_text}"
        fail_test "$command_status"
    fi
}

run_phase() {
    local phase_number="$1"
    local phase_name="$2"
    local log_file="$3"
    local command_status
    shift 3

    echo "[${phase_number}] ${phase_name}: START"
    run_logged "$log_file" "$@"
    command_status="$?"
    if ((command_status == 0)); then
        return 0
    fi
    echo "[${phase_number}] ${phase_name}: FAIL exit_code=${command_status} log=${log_file}"
    fail_test "$command_status"
}

now_ns() {
    date +%s%N
}

seconds_from_ns() {
    awk -v nanoseconds="$1" 'BEGIN {printf "%.6f", nanoseconds / 1000000000.0}'
}

echo "[regression] build_dir=${build_dir}"
echo "[regression] threads.single=1"
echo "[regression] threads.all=${threads}"
echo "[regression] seed=42"
echo "[regression] verbose=${verbose}"
echo "[regression] output_dir=${test_dir}"

cpu_single_start_ns="$(now_ns)"
run_phase "1/5" cpu-single cpu_single.log "$g4go" \
    --backend cpu \
    --threads 1 \
    --seed 42 \
    "$source_macro"
cpu_single_elapsed_seconds="$(seconds_from_ns "$(( $(now_ns) - cpu_single_start_ns ))")"
echo "[1/5] cpu-single: PASS wall_time=${cpu_single_elapsed_seconds}s log=cpu_single.log"
run_command test -s run_beam_eminus.root
run_command mv run_beam_eminus.root noptpho_cpu_single.root

cpu_all_start_ns="$(now_ns)"
run_phase "2/5" cpu-all cpu_all.log "$g4go" \
    --backend cpu \
    --threads "$threads" \
    --seed 42 \
    "$source_macro"
cpu_all_elapsed_seconds="$(seconds_from_ns "$(( $(now_ns) - cpu_all_start_ns ))")"
echo "[2/5] cpu-all: PASS wall_time=${cpu_all_elapsed_seconds}s log=cpu_all.log"
run_command test -s run_beam_eminus.root
run_command mv run_beam_eminus.root noptpho_cpu_all.root

gpu_start_ns="$(now_ns)"
run_phase "3/5" gpu gpu.log "$g4go" \
    --backend gpu \
    --threads "$threads" \
    --seed 42 \
    "$source_macro"
gpu_elapsed_seconds="$(seconds_from_ns "$(( $(now_ns) - gpu_start_ns ))")"
echo "[3/5] gpu: PASS wall_time=${gpu_elapsed_seconds}s log=gpu.log"
run_command grep -F "[g4go] optical backend: gpu" gpu.log
run_command test -s run_beam_eminus.root
run_command mv run_beam_eminus.root noptpho_gpu.root

compare_call="${compare_macro}(\"${test_dir}/noptpho_cpu_single.root\",\"${test_dir}/noptpho_cpu_all.root\",\"${test_dir}/noptpho_gpu.root\",${cpu_single_elapsed_seconds},${cpu_all_elapsed_seconds},${gpu_elapsed_seconds},\"${test_dir}\")"
run_phase "4/5" comparison comparison.log "$root_executable" -l -b -q "$compare_call"
echo "[4/5] comparison: PASS log=comparison.log"
run_command test -s regression_result.txt
run_command test -s noptpho_comparison.png
run_command test -s noptpho_regression_report.root

run_gpu_determinism() {
    local thread_count log_file command_status stats reference_stats
    for thread_count in 1 2 4 8; do
        log_file="gpu_threads_${thread_count}.log"
        echo "[determinism] threads=${thread_count}: START"
        run_logged "$log_file" "$g4go" \
            --backend gpu \
            --threads "$thread_count" \
            --seed 42 \
            "$determinism_source_macro"
        command_status="$?"
        if ((command_status != 0)); then
            echo "[determinism] threads=${thread_count}: FAIL exit_code=${command_status} log=${log_file}"
            return "$command_status"
        fi
        if ! grep -F "[g4go] optical backend: gpu" "$log_file" >/dev/null 2>&1; then
            echo "[determinism] threads=${thread_count}: GPU backend marker is missing"
            return 1
        fi
        stats="$(sed -n 's/.*\[g4go\] optical backend: gpu, //p' "$log_file" | sed -E 's/, transport_ms:.*//')"
        if [[ -z "$stats" ]]; then
            echo "[determinism] threads=${thread_count}: GPU statistics are missing"
            return 1
        fi
        if [[ "$thread_count" == 1 ]]; then
            reference_stats="$stats"
        elif [[ "$stats" != "$reference_stats" ]]; then
            echo "[determinism] threads=${thread_count}: statistics differ"
            echo "[determinism] reference=${reference_stats}"
            echo "[determinism] candidate=${stats}"
            return 1
        fi
        echo "[determinism] threads=${thread_count}: PASS"
    done
}

run_phase "5/5" gpu-determinism gpu_determinism.log run_gpu_determinism
echo "[5/5] gpu-determinism: PASS log=gpu_determinism.log"

echo "[regression] RESULT: PASSED"
print_summary
