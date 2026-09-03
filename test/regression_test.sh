#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
build_dir="${project_dir}/build"
cpu_affinity="${G4GO_CPU_AFFINITY:-2}"
batch_timeout_ms="${G4GO_BATCH_TIMEOUT_MS:-10}"
benchmark_runs="${G4GO_BENCHMARK_RUNS:-3}"
perf_diagnostics="${G4GO_PERF_DIAGNOSTICS:-0}"
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
    --verbose)
        verbose=1
        shift
        ;;
    -h|--help)
        echo "Usage: $0 [--build-dir BUILD_DIR] [--verbose]"
        echo "Environment: G4GO_CPU_AFFINITY, G4GO_BATCH_TIMEOUT_MS,"
        echo "             G4GO_BENCHMARK_RUNS, G4GO_PERF_DIAGNOSTICS"
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

taskset_executable="$(command -v taskset || true)"
if [[ -z "$taskset_executable" ]]; then
    echo "taskset is required for the strict single-core benchmark" >&2
    exit 1
fi
if [[ ! "$cpu_affinity" =~ ^[0-9]+$ ]]; then
    echo "G4GO_CPU_AFFINITY must be a single non-negative CPU number: $cpu_affinity" >&2
    exit 1
fi
if ! "$taskset_executable" --cpu-list "$cpu_affinity" true >/dev/null 2>&1; then
    echo "G4GO_CPU_AFFINITY is not an available CPU: $cpu_affinity" >&2
    "$taskset_executable" --cpu-list "$cpu_affinity" true
    exit 1
fi
if [[ ! "$batch_timeout_ms" =~ ^[0-9]+$ ]] ||
   ((batch_timeout_ms > 10000)); then
    echo "G4GO_BATCH_TIMEOUT_MS must be an integer from 0 to 10000: $batch_timeout_ms" >&2
    exit 1
fi
if [[ ! "$benchmark_runs" =~ ^[1-9][0-9]*$ ]] ||
   ((benchmark_runs > 9)); then
    echo "G4GO_BENCHMARK_RUNS must be an integer from 1 to 9: $benchmark_runs" >&2
    exit 1
fi
if [[ "$perf_diagnostics" != 0 && "$perf_diagnostics" != 1 ]]; then
    echo "G4GO_PERF_DIAGNOSTICS must be 0 or 1: $perf_diagnostics" >&2
    exit 1
fi

g4go="${build_dir}/g4go"
source_macro="${project_dir}/scripts/run_beam_eminus.mac"
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
if [[ ! -f "$compare_macro" ]]; then
    echo "ROOT comparison macro is missing: $compare_macro" >&2
    exit 1
fi
if [[ -z "$root_executable" ]]; then
    echo "ROOT executable was not found in PATH" >&2
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
    echo "[regression] OUTPUT: FAILED exit_code=$exit_code"
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

run_g4go() {
    "$taskset_executable" --cpu-list "$cpu_affinity" "$g4go" "$@"
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
echo "[regression] cpu_affinity=${cpu_affinity}"
echo "[regression] geant4_threads=1"
echo "[regression] seed=42"
echo "[regression] batch_timeout_ms=${batch_timeout_ms}"
echo "[regression] benchmark_runs=${benchmark_runs}"
echo "[regression] perf_diagnostics=${perf_diagnostics}"
echo "[regression] verbose=${verbose}"
echo "[regression] output_dir=${test_dir}"

run_benchmark_phase() {
    local phase_number="$1"
    local backend="$2"
    local log_prefix="$3"
    local elapsed_name="$4"
    local directory_name="$5"
    local -n elapsed_values="$elapsed_name"
    local -n run_directories="$directory_name"
    local run_number run_directory start_ns elapsed_seconds command_status
    local -a g4go_arguments

    g4go_arguments=(
        --backend "$backend"
        --threads 1
        --seed 42
        --batch-timeout-ms "$batch_timeout_ms"
    )
    if ((perf_diagnostics)); then
        g4go_arguments+=(--perf-diagnostics)
    fi

    for ((run_number = 1; run_number <= benchmark_runs; ++run_number)); do
        run_directory="${test_dir}/benchmark_runs/${log_prefix}_${run_number}"
        if ! mkdir -p "$run_directory"; then
            echo "Unable to create benchmark directory: $run_directory" >&2
            fail_test 1
        fi
        echo "[${phase_number}] ${backend}-single run=${run_number}/${benchmark_runs}: START"
        start_ns="$(now_ns)"
        (
            cd "$run_directory" || exit 1
            run_logged "${log_prefix}.log" run_g4go \
                "${g4go_arguments[@]}" "$source_macro"
        )
        command_status="$?"
        if ((command_status != 0)); then
            echo "[${phase_number}] ${backend}-single run=${run_number}: FAIL exit_code=${command_status} log=${run_directory}/${log_prefix}.log"
            fail_test "$command_status"
        fi
        elapsed_seconds="$(seconds_from_ns "$(( $(now_ns) - start_ns ))")"
        if [[ "$backend" == gpu ]]; then
            if ! grep -F "[g4go] optical backend: gpu" \
                "${run_directory}/${log_prefix}.log" >/dev/null 2>&1; then
                echo "[${phase_number}] GPU backend marker is missing in ${run_directory}/${log_prefix}.log"
                fail_test 1
            fi
        fi
        if [[ ! -s "${run_directory}/run_beam_eminus.root" ]]; then
            echo "[${phase_number}] ROOT output is missing: ${run_directory}/run_beam_eminus.root"
            fail_test 1
        fi
        elapsed_values+=("$elapsed_seconds")
        run_directories+=("$run_directory")
        echo "[${phase_number}] ${backend}-single run=${run_number}: PASS wall_time=${elapsed_seconds}s log=${run_directory}/${log_prefix}.log"
    done
}

select_median_run() {
    local elapsed_name="$1"
    local -n elapsed_values="$elapsed_name"
    local record middle
    local -a records
    records=()
    for ((record = 0; record < ${#elapsed_values[@]}; ++record)); do
        records+=("${elapsed_values[record]} ${record}")
    done
    middle=$(((${#records[@]} + 1) / 2))
    printf '%s\n' "${records[@]}" | sort -n -k1,1 | sed -n "${middle}p"
}

cpu_elapsed_values=()
gpu_elapsed_values=()
cpu_run_directories=()
gpu_run_directories=()
run_benchmark_phase "1/3" cpu cpu_single cpu_elapsed_values cpu_run_directories
run_benchmark_phase "2/3" gpu gpu_single gpu_elapsed_values gpu_run_directories

read -r cpu_median_elapsed cpu_median_index <<<"$(select_median_run cpu_elapsed_values)"
read -r gpu_median_elapsed gpu_median_index <<<"$(select_median_run gpu_elapsed_values)"
cpu_median_index=$((cpu_median_index))
gpu_median_index=$((gpu_median_index))
cpu_single_elapsed_seconds="$cpu_median_elapsed"
gpu_elapsed_seconds="$gpu_median_elapsed"

run_command cp -- \
    "${cpu_run_directories[cpu_median_index]}/cpu_single.log" cpu_single.log
run_command cp -- \
    "${cpu_run_directories[cpu_median_index]}/run_beam_eminus.root" \
    noptpho_cpu_single.root
run_command cp -- \
    "${gpu_run_directories[gpu_median_index]}/gpu_single.log" gpu.log
run_command cp -- \
    "${gpu_run_directories[gpu_median_index]}/run_beam_eminus.root" \
    noptpho_gpu_single.root

gpu_speedup="$(awk -v cpu="$cpu_single_elapsed_seconds" \
                       -v gpu="$gpu_elapsed_seconds" \
                       'BEGIN { if (gpu > 0) printf "%.6f", cpu / gpu; }')"
{
    echo "benchmark_runs=${benchmark_runs}"
    echo "cpu_affinity=${cpu_affinity}"
    echo "geant4_threads=1"
    echo "batch_timeout_ms=${batch_timeout_ms}"
    echo "cpu_median_wall_time_s=${cpu_single_elapsed_seconds}"
    echo "gpu_median_wall_time_s=${gpu_elapsed_seconds}"
    echo "gpu_speedup_vs_cpu_single_core=${gpu_speedup}x"
    echo "cpu_median_run=$((cpu_median_index + 1))"
    echo "gpu_median_run=$((gpu_median_index + 1))"
    for ((run_number = 0; run_number < benchmark_runs; ++run_number)); do
        echo "cpu_run_$((run_number + 1))_wall_time_s=${cpu_elapsed_values[run_number]}"
        echo "gpu_run_$((run_number + 1))_wall_time_s=${gpu_elapsed_values[run_number]}"
    done
} > benchmark_summary.txt

if ((perf_diagnostics)); then
    {
        echo "cpu_affinity=${cpu_affinity}"
        echo "geant4_threads=1"
        echo "batch_timeout_ms=${batch_timeout_ms}"
        for ((run_number = 0; run_number < benchmark_runs; ++run_number)); do
            echo "[cpu run=$((run_number + 1))]"
            rg '^\[g4go\] performance(_output)?:' \
                "${cpu_run_directories[run_number]}/cpu_single.log" || true
            echo "[gpu run=$((run_number + 1))]"
            rg '^\[g4go\] performance(_output)?:' \
                "${gpu_run_directories[run_number]}/gpu_single.log" || true
        done
    } > performance_summary.txt
fi

compare_call="${compare_macro}(\"${test_dir}/noptpho_cpu_single.root\",\"${test_dir}/noptpho_gpu_single.root\",${cpu_single_elapsed_seconds},${gpu_elapsed_seconds},\"${test_dir}\")"
run_phase "3/3" comparison comparison.log "$root_executable" -l -b -q "$compare_call"
echo "[3/3] comparison: PASS log=comparison.log"
run_command test -s regression_output.txt
run_command test -s noptpho_comparison.png
run_command test -s tof_comparison.png
run_command test -s noptpho_regression_report.root

echo "[regression] OUTPUT: PASSED"
print_summary
