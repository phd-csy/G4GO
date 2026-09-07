#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# The script is copied to <build>/test/regression_test.sh. When that copy is
# invoked, the build dir is the directory one level up (which holds the g4go
# binary) and the source tree is its parent; otherwise both live under the
# source tree. Both copies then run without --build-dir.
if [[ -x "${script_dir}/../g4go" ]]; then
    build_dir="$(cd -- "${script_dir}/.." && pwd)"
    source_dir="$(cd -- "${build_dir}/.." && pwd)"
else
    source_dir="$(cd -- "${script_dir}/.." && pwd)"
    build_dir="${source_dir}/build"
fi
cpu_affinity="${G4GO_CPU_AFFINITY:-2}"
batch_timeout_ms="${G4GO_BATCH_TIMEOUT_MS:-10}"
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
        echo "             G4GO_PERF_DIAGNOSTICS"
        exit 0
        ;;
    *)
        echo "Unknown argument: $1" >&2
        exit 1
        ;;
    esac
done

if [[ ! -d "$build_dir" ]]; then
    echo "Build directory does not exist" >&2
    exit 1
fi
build_dir="$(cd -- "$build_dir" && pwd)"

taskset_executable="$(command -v taskset || true)"
if [[ -z "$taskset_executable" ]]; then
    echo "taskset is required for the single-core CPU/GPU policy" >&2
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
if [[ "$perf_diagnostics" != 0 && "$perf_diagnostics" != 1 ]]; then
    echo "G4GO_PERF_DIAGNOSTICS must be 0 or 1: $perf_diagnostics" >&2
    exit 1
fi

g4go="${build_dir}/g4go"
regression_macro="${source_dir}/scripts/run_eminus_regression.mac"
# Use the comparison macro copied into the build tree so both the source-tree
# and the copied script exercise the same build artifact.
compare_macro="${build_dir}/test/TestOpticalTransport.cxx"
root_executable="$(command -v root || true)"
regression_dir="${build_dir}/test/regression"
run_timestamp="$(TZ=Asia/Shanghai date +%Y%m%d-%H%M%S)-$$"
run_dir="${regression_dir}/${run_timestamp}"

if [[ ! -x "$g4go" ]]; then
    echo "Executable is missing or not executable" >&2
    exit 1
fi
if [[ ! -f "$regression_macro" ]]; then
    echo "Regression macro is missing" >&2
    exit 1
fi
if [[ ! -f "$compare_macro" ]]; then
    echo "ROOT comparison macro is missing" >&2
    exit 1
fi
if [[ -z "$root_executable" ]]; then
    echo "ROOT executable was not found in PATH" >&2
    exit 1
fi

sha256sum_executable="$(command -v sha256sum || true)"
if [[ -z "$sha256sum_executable" ]]; then
    echo "sha256sum is required to validate regression caches" >&2
    exit 1
fi

g4go_sha256="$("$sha256sum_executable" "$g4go" | awk '{print $1}')"
regression_macro_sha256="$(
    "$sha256sum_executable" "$regression_macro" | awk '{print $1}'
)"

target_events="$(sed -nE \
    's@^[[:space:]]*/run/beamOn[[:space:]]+([0-9]+).*@\1@p' \
    "$regression_macro" | tail -n 1)"
if [[ ! "$target_events" =~ ^[0-9]+$ ]] || ((target_events == 0)); then
    echo "Unable to parse a positive /run/beamOn count from regression macro" >&2
    exit 1
fi

cpu_6t_reference="${regression_dir}/cpu_6t.root"
gpu_1t_reference="${regression_dir}/gpu_1t.root"
gpu_6t_reference="${regression_dir}/gpu_6t.root"
cpu_timing_file="${regression_dir}/cpu_timing.txt"
cpu_regression_threads=6
logs_dir="${run_dir}/logs"
cpu_6t_log="${logs_dir}/cpu_6t.log"
gpu_1t_log="${logs_dir}/gpu_1t.log"
gpu_6t_log="${logs_dir}/gpu_6t.log"
comparison_1t_log="${logs_dir}/comparison_gpu_1t.log"
comparison_6t_log="${logs_dir}/comparison_gpu_6t.log"
regression_log="${logs_dir}/regression.log"

mkdir -p "$regression_dir" "$logs_dir"
cd "$regression_dir" || exit 1
# The measured CPU-6T run provides both the regression reference and the
# CPU timing baseline. Remove deprecated single-core benchmark artifacts so
# they cannot be mistaken for current cache entries.
rm -f -- run_cpu_benchmark.root "${regression_dir}/cpu_single_benchmark.root"
# Remove stale GPU output names before writing the current run results.
rm -f -- "${regression_dir}/gpu_run_1.root" \
    "$gpu_1t_reference" "$gpu_6t_reference" \
    "${regression_dir}/gpu_single.root" \
    "${regression_dir}/run_eminus_regression.root"
: > "$regression_log"

start_time="$(date +%s)"
exec > >(tee -a "$regression_log") 2>&1

print_summary() {
    local end_time total_time hours minutes seconds
    end_time="$(date +%s)"
    total_time=$((end_time - start_time))
    hours=$((total_time / 3600))
    minutes=$(((total_time % 3600) / 60))
    seconds=$((total_time % 60))
    echo "[regression] total_wall_time=${hours}h${minutes}m${seconds}s"
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

run_g4go_single() {
    "$taskset_executable" --cpu-list "$cpu_affinity" "$g4go" "$@"
}

run_comparison() {
    local comparison_name="$1"
    local log_file="$2"
    local label="$3"
    local gpu_file_name="$4"
    local gpu_elapsed="$5"
    local cpu_elapsed="$6"
    local output_directory="${run_dir}"
    local compare_call command_status output_file figure_name

    mkdir -p "${output_directory}/figures"
    compare_call="${compare_macro}(\"${cpu_6t_reference}\",\"${gpu_file_name}\",${cpu_elapsed},${gpu_elapsed},\"${output_directory}\")"
    run_logged "$log_file" "$root_executable" -l -b -q "$compare_call"
    command_status="$?"
    if ((command_status != 0)); then
        echo "[${comparison_name}] FAIL exit_code=${command_status}"
        return 1
    fi
    # Name each comparison's text summary with the comparison label so the two
    # groups are symmetric and self-describing. The per-group summaries are
    # folded into the aggregate regression_summary.txt and removed below.
    summary_name="${label}_regression_summary.txt"
    mv -- "${output_directory}/regression_summary.txt" \
        "${output_directory}/${summary_name}" || return 1
    for figure_name in noptpho_comparison.png tof_comparison.png edep_comparison.png; do
        mv -- "${output_directory}/figures/${figure_name}" \
            "${output_directory}/figures/${label}_${figure_name}" || return 1
    done
    if [[ ! -s "${output_directory}/${summary_name}" ]]; then
        echo "[${comparison_name}] required output is missing: ${summary_name}"
        return 1
    fi
    for figure_name in noptpho_comparison.png tof_comparison.png edep_comparison.png; do
        if [[ ! -s "${output_directory}/figures/${label}_${figure_name}" ]]; then
            echo "[${comparison_name}] required output is missing: figures/${label}_${figure_name}"
            return 1
        fi
    done
    echo "[${comparison_name}] PASS"
    return 0
}

now_ns() {
    date +%s%N
}

seconds_from_ns() {
    awk -v nanoseconds="$1" 'BEGIN {printf "%.6f", nanoseconds / 1000000000.0}'
}

read_timing_value() {
    local key="$1"
    if [[ ! -s "$cpu_timing_file" ]]; then
        return 0
    fi
    awk -F= -v key="$key" \
        '$1 == key {print substr($0, index($0, "=") + 1); exit}' \
        "$cpu_timing_file"
}

is_positive_number() {
    local value="$1"
    [[ "$value" =~ ^[0-9]+([.][0-9]+)?$ ]] || return 1
    awk -v value="$value" 'BEGIN {exit !(value > 0.0)}'
}

run_timed_cpu_phase() {
    local phase_name="$1"
    local log_file="$2"
    local output_file="$3"
    local elapsed_variable="$4"
    local affinity_policy="$5"
    local event_count="$6"
    shift 6
    local start_ns elapsed_seconds command_status

    rm -f -- "$output_file"
    start_ns="$(now_ns)"
    if [[ "$affinity_policy" == single ]]; then
        run_logged "$log_file" run_g4go_single "$@"
        command_status="$?"
    else
        run_logged "$log_file" "$@"
        command_status="$?"
    fi
    if ((command_status != 0)); then
        echo "[${phase_name}] FAIL events=${event_count} exit_code=${command_status}"
        fail_test "$command_status"
    fi
    elapsed_seconds="$(seconds_from_ns "$(( $(now_ns) - start_ns ))")"
    if [[ ! -s "$output_file" ]]; then
        echo "[${phase_name}] ROOT output is missing events=${event_count}"
        fail_test 1
    fi
    printf -v "$elapsed_variable" '%s' "$elapsed_seconds"
    echo "[${phase_name}] PASS events=${event_count} wall_time=${elapsed_seconds}s"
}

write_cpu_timing() {
    local regression_elapsed="$1"
    {
        echo "seed=42"
        echo "cpu_affinity=${cpu_affinity}"
        echo "g4go_sha256=${g4go_sha256}"
        echo "regression_macro_sha256=${regression_macro_sha256}"
        echo "cpu_target_events=${target_events}"
        echo "cpu_regression_threads=${cpu_regression_threads}"
        echo "cpu_6t_regression_events=${target_events}"
        echo "cpu_6t_regression_real_time_s=${regression_elapsed}"
    } > "$cpu_timing_file"
}

echo "[regression] target_events=${target_events}"

cached_g4go_sha256="$(read_timing_value g4go_sha256)"
cached_regression_macro_sha256="$(read_timing_value regression_macro_sha256)"
cpu_6t_regression_elapsed="$(read_timing_value cpu_6t_regression_real_time_s)"
cpu_6t_regression_source=cached
cached_cpu_regression_threads="$(read_timing_value cpu_regression_threads)"

if [[ ! -s "$cpu_6t_reference" ]] ||
   [[ "$cached_g4go_sha256" != "$g4go_sha256" ]] ||
   [[ "$cached_regression_macro_sha256" != "$regression_macro_sha256" ]] ||
   [[ "$cached_cpu_regression_threads" != "$cpu_regression_threads" ]] ||
   [[ "$(read_timing_value cpu_6t_regression_events)" != "$target_events" ]] ||
   ! is_positive_number "$cpu_6t_regression_elapsed"; then
    run_timed_cpu_phase \
        cpu-6t-regression "$cpu_6t_log" run_eminus_regression.root \
        cpu_6t_regression_elapsed full \
        "$target_events" \
        "$g4go" --backend cpu --threads "$cpu_regression_threads" \
        --seed 42 "$regression_macro"
    cp -- run_eminus_regression.root "$cpu_6t_reference" || fail_test 1
    cpu_6t_regression_source=generated
else
    echo "[cpu-6t-regression] CACHED events=${target_events} time=${cpu_6t_regression_elapsed}s"
fi

write_cpu_timing \
    "$cpu_6t_regression_elapsed"

if ! is_positive_number "$cpu_6t_regression_elapsed"; then
    echo "CPU timing cache contains invalid values"
    fail_test 1
fi

gpu_arguments=(
    --backend gpu
    --seed 42
    --batch-timeout-ms "$batch_timeout_ms"
)
if ((perf_diagnostics)); then
    gpu_arguments+=(--perf-diagnostics)
fi
run_timed_cpu_phase \
    gpu-1t "$gpu_1t_log" run_eminus_regression.root \
    gpu_1t_elapsed_seconds single "$target_events" \
    "${gpu_arguments[@]}" --threads 1 "$regression_macro"
if ! grep -F "[g4go] optical backend: gpu" "$gpu_1t_log" >/dev/null 2>&1; then
    echo "[gpu-1t] GPU backend marker is missing"
    fail_test 1
fi
cp -- run_eminus_regression.root "$gpu_1t_reference" || fail_test 1
echo "[gpu-1t] ROOT reference=$(basename "$gpu_1t_reference")"

run_timed_cpu_phase \
    gpu-6t "$gpu_6t_log" run_eminus_regression.root \
    gpu_6t_elapsed_seconds full "$target_events" \
    "$g4go" "${gpu_arguments[@]}" --threads "$cpu_regression_threads" \
    "$regression_macro"
if ! grep -F "[g4go] optical backend: gpu" "$gpu_6t_log" >/dev/null 2>&1; then
    echo "[gpu-6t] GPU backend marker is missing"
    fail_test 1
fi
cp -- run_eminus_regression.root "$gpu_6t_reference" || fail_test 1
echo "[gpu-6t] ROOT reference=$(basename "$gpu_6t_reference")"

# Estimate the CPU-1T wall time by scaling the measured CPU-6T time with the
# worker count; GPU-1T is compared against that estimate, while GPU-6T is
# compared directly against the measured CPU-6T baseline.
cpu_1t_estimated_real_time_s="$(awk \
    -v cpu_6t_time="$cpu_6t_regression_elapsed" \
    -v cpu_threads="$cpu_regression_threads" \
    'BEGIN {
        if (cpu_6t_time > 0.0 && cpu_threads > 0.0) {
            printf "%.6f", cpu_6t_time * cpu_threads;
        }
    }')"
if ! is_positive_number "$cpu_1t_estimated_real_time_s"; then
    echo "Unable to estimate CPU-1T time"
    fail_test 1
fi

gpu_1t_speedup_vs_cpu_1t_estimated="$(awk \
    -v cpu="$cpu_1t_estimated_real_time_s" \
    -v gpu="$gpu_1t_elapsed_seconds" \
    'BEGIN { if (cpu > 0.0 && gpu > 0.0) printf "%.6f", cpu / gpu; }')"
gpu_6t_speedup_vs_cpu_6t="$(awk \
    -v cpu="$cpu_6t_regression_elapsed" \
    -v gpu="$gpu_6t_elapsed_seconds" \
    'BEGIN { if (cpu > 0.0 && gpu > 0.0) printf "%.6f", cpu / gpu; }')"
if ! is_positive_number "$gpu_1t_speedup_vs_cpu_1t_estimated" ||
   ! is_positive_number "$gpu_6t_speedup_vs_cpu_6t"; then
    echo "Unable to calculate GPU speedups"
    fail_test 1
fi

{
    echo "cpu_affinity=${cpu_affinity}"
    echo "g4go_sha256=${g4go_sha256}"
    echo "regression_macro_sha256=${regression_macro_sha256}"
    echo "cpu_geant4_threads=${cpu_regression_threads}"
    echo "gpu_1t_geant4_threads=1"
    echo "gpu_6t_geant4_threads=${cpu_regression_threads}"
    echo "batch_timeout_ms=${batch_timeout_ms}"
    echo "cpu_target_events=${target_events}"
    echo "cpu_6t_regression_events=${target_events}"
    echo "cpu_6t_regression_real_time_s=${cpu_6t_regression_elapsed}"
    echo "cpu_6t_regression_source=${cpu_6t_regression_source}"
    echo "gpu_1t_wall_time_s=${gpu_1t_elapsed_seconds}"
    echo "gpu_6t_wall_time_s=${gpu_6t_elapsed_seconds}"
    echo "cpu_1t_estimated_real_time_s=${cpu_1t_estimated_real_time_s}"
    echo "gpu_1t_speedup_vs_cpu_1t_estimated=${gpu_1t_speedup_vs_cpu_1t_estimated}x"
    echo "gpu_6t_speedup_vs_cpu_6t=${gpu_6t_speedup_vs_cpu_6t}x"
} > "${run_dir}/benchmark_summary.txt"

if ((perf_diagnostics)); then
    {
        echo "cpu_affinity=${cpu_affinity}"
        echo "cpu_geant4_threads=${cpu_regression_threads}"
        echo "gpu_1t_geant4_threads=1"
        echo "gpu_6t_geant4_threads=${cpu_regression_threads}"
        echo "batch_timeout_ms=${batch_timeout_ms}"
        echo "[gpu-1t]"
        rg '^\[g4go\] performance(_output)?:' \
            "$gpu_1t_log" || true
        echo "[gpu-6t]"
        rg '^\[g4go\] performance(_output)?:' \
            "$gpu_6t_log" || true
    } > "${run_dir}/performance_summary.txt"
fi

comparison_failed=0
if ! run_comparison \
    "comparison-gpu-1t-vs-cpu-6t" "$comparison_1t_log" \
    gpu_1t "$gpu_1t_reference" \
    "$gpu_1t_elapsed_seconds" "$cpu_1t_estimated_real_time_s"; then
    comparison_failed=1
fi
if ! run_comparison \
    "comparison-gpu-6t-vs-cpu-6t" "$comparison_6t_log" \
    gpu_6t "$gpu_6t_reference" \
    "$gpu_6t_elapsed_seconds" "$cpu_6t_regression_elapsed"; then
    comparison_failed=1
fi
if ((comparison_failed)); then
    fail_test 1
fi

{
    echo "Status: PASSED"
    echo "Reference: CPU-6T ROOT and measured CPU-6T wall time"
    echo "Speedup: GPU-1T vs estimated CPU-1T=${gpu_1t_speedup_vs_cpu_1t_estimated}x"
    echo "Speedup: GPU-6T vs CPU-6T=${gpu_6t_speedup_vs_cpu_6t}x"
    echo
    echo "=== GPU-1T vs estimated CPU-1T ==="
    cat "${run_dir}/gpu_1t_regression_summary.txt"
    echo
    echo "=== GPU-6T vs CPU-6T ==="
    cat "${run_dir}/gpu_6t_regression_summary.txt"
} > "${run_dir}/regression_summary.txt"
rm -f -- "${run_dir}/gpu_1t_regression_summary.txt" \
    "${run_dir}/gpu_6t_regression_summary.txt"

echo "[regression] OUTPUT: PASSED"
echo "[regression] GPU-1T speedup vs estimated CPU-1T=${gpu_1t_speedup_vs_cpu_1t_estimated}x"
echo "[regression] GPU-6T speedup vs CPU-6T=${gpu_6t_speedup_vs_cpu_6t}x"
print_summary
