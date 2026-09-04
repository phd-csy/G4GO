#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
build_dir="${project_dir}/build"
cpu_affinity="${G4GO_CPU_AFFINITY:-2}"
batch_timeout_ms="${G4GO_BATCH_TIMEOUT_MS:-15}"
max_in_flight_batches="${G4GO_MAX_IN_FLIGHT_BATCHES:-2}"
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
        echo "             G4GO_MAX_IN_FLIGHT_BATCHES, G4GO_PERF_DIAGNOSTICS"
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
if [[ ! "$max_in_flight_batches" =~ ^[12]$ ]]; then
    echo "G4GO_MAX_IN_FLIGHT_BATCHES must be 1 or 2: $max_in_flight_batches" >&2
    exit 1
fi
if [[ "$perf_diagnostics" != 0 && "$perf_diagnostics" != 1 ]]; then
    echo "G4GO_PERF_DIAGNOSTICS must be 0 or 1: $perf_diagnostics" >&2
    exit 1
fi

g4go="${build_dir}/g4go"
cpu_benchmark_macro="${project_dir}/scripts/run_cpu_benchmark.mac"
regression_macro="${project_dir}/scripts/run_eminus_regression.mac"
compare_macro="${script_dir}/TestOpticalTransport.cxx"
root_executable="$(command -v root || true)"
regression_dir="${build_dir}/test/regression"
run_timestamp="$(TZ=Asia/Shanghai date +%Y%m%d-%H%M%S)-$$"
run_dir="${regression_dir}/${run_timestamp}"

if [[ ! -x "$g4go" ]]; then
    echo "Executable is missing or not executable" >&2
    exit 1
fi
if [[ ! -f "$cpu_benchmark_macro" ]]; then
    echo "CPU benchmark macro is missing" >&2
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
cpu_benchmark_macro_sha256="$(
    "$sha256sum_executable" "$cpu_benchmark_macro" | awk '{print $1}'
)"
regression_macro_sha256="$(
    "$sha256sum_executable" "$regression_macro" | awk '{print $1}'
)"

benchmark_events="$(sed -nE \
    's@^[[:space:]]*/run/beamOn[[:space:]]+([0-9]+).*@\1@p' \
    "$cpu_benchmark_macro" | tail -n 1)"
target_events="$(sed -nE \
    's@^[[:space:]]*/run/beamOn[[:space:]]+([0-9]+).*@\1@p' \
    "$regression_macro" | tail -n 1)"
if [[ ! "$benchmark_events" =~ ^[0-9]+$ ]] || ((benchmark_events == 0)); then
    echo "Unable to parse a positive /run/beamOn count from CPU benchmark macro" >&2
    exit 1
fi
if [[ ! "$target_events" =~ ^[0-9]+$ ]] || ((target_events == 0)); then
    echo "Unable to parse a positive /run/beamOn count from regression macro" >&2
    exit 1
fi

# The first 1000-event benchmark timing is retained in the CPU timing cache
# and provides the single-core timing used for later event-count extrapolation.
cpu_single_reference_events=1000
if ((benchmark_events != cpu_single_reference_events)); then
    echo "run_cpu_benchmark.mac must contain ${cpu_single_reference_events} events; found ${benchmark_events}" >&2
    exit 1
fi

cpu_single_reference="${regression_dir}/cpu_single_benchmark.root"
cpu_full_reference="${regression_dir}/cpu_full.root"
cpu_timing_file="${regression_dir}/cpu_timing.txt"
logs_dir="${run_dir}/logs"
cpu_single_log="${logs_dir}/cpu_single_benchmark.log"
cpu_full_log="${logs_dir}/cpu_full.log"
comparison_log="${logs_dir}/comparison.log"
regression_log="${logs_dir}/regression.log"

mkdir -p "$regression_dir" "$logs_dir"
cd "$regression_dir" || exit 1
# The full-core calibration only contributes its elapsed time. Remove the
# transient benchmark ROOT output before each run so it cannot be mistaken for
# a persisted reference artifact.
rm -f -- run_cpu_benchmark.root
# Remove stale GPU output names before writing the current single-run result.
rm -f -- "${regression_dir}/gpu_run_1.root" "${regression_dir}/gpu_single.root"
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

run_phase() {
    local phase_name="$1"
    local log_file="$2"
    local command_status
    shift 2

    run_logged "$log_file" "$@"
    command_status="$?"
    if ((command_status == 0)); then
        return 0
    fi
    echo "[${phase_name}] FAIL exit_code=${command_status}"
    fail_test "$command_status"
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

read_first_single_benchmark_log_time() {
    local benchmark_log benchmark_time
    while IFS= read -r benchmark_log; do
        benchmark_time="$(awk '
            /Time runs:/ {
                for (field = 1; field <= NF; ++field) {
                    if ($field ~ /^Real=/) {
                        split($field, value, "=");
                        sub(/s$/, "", value[2]);
                        print value[2];
                        exit;
                    }
                }
            }
        ' "$benchmark_log")"
        if is_positive_number "$benchmark_time"; then
            printf '%s\n' "$benchmark_time"
            return 0
        fi
    done < <(find "$regression_dir" -mindepth 2 -maxdepth 3 -type f \
        -name 'cpu_single_benchmark.log' -print | LC_ALL=C sort)
    return 1
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
    local single_measured="$1"
    local full_regression_elapsed="$2"
    {
        echo "seed=42"
        echo "cpu_affinity=${cpu_affinity}"
        echo "g4go_sha256=${g4go_sha256}"
        echo "cpu_benchmark_macro_sha256=${cpu_benchmark_macro_sha256}"
        echo "regression_macro_sha256=${regression_macro_sha256}"
        echo "cpu_single_reference_events=${cpu_single_reference_events}"
        echo "cpu_single_benchmark_time_for_estimate_s=${single_measured}"
        echo "cpu_single_benchmark_events=${benchmark_events}"
        echo "cpu_single_benchmark_measured_real_time_s=${single_measured}"
        echo "cpu_target_events=${target_events}"
        echo "cpu_full_regression_events=${target_events}"
        echo "cpu_full_regression_real_time_s=${full_regression_elapsed}"
    } > "$cpu_timing_file"
}

echo "[regression] target_events=${target_events}"

cpu_single_measured="$(read_timing_value cpu_single_benchmark_measured_real_time_s)"
cpu_single_benchmark_events_cached="$(read_timing_value cpu_single_benchmark_events)"
cached_g4go_sha256="$(read_timing_value g4go_sha256)"
cached_cpu_benchmark_macro_sha256="$(
    read_timing_value cpu_benchmark_macro_sha256
)"
cached_regression_macro_sha256="$(read_timing_value regression_macro_sha256)"
cached_cpu_affinity="$(read_timing_value cpu_affinity)"
if ! is_positive_number "$cpu_single_measured" &&
   [[ -s "$cpu_single_reference" ]]; then
    cpu_single_measured="$(read_first_single_benchmark_log_time || true)"
    if is_positive_number "$cpu_single_measured"; then
        cpu_single_benchmark_events_cached="$benchmark_events"
    fi
fi
cpu_full_regression_elapsed="$(read_timing_value cpu_full_regression_real_time_s)"
cpu_single_source=cached
cpu_full_regression_source=cached

if [[ ! -s "$cpu_single_reference" ]] ||
   [[ "$cached_cpu_affinity" != "$cpu_affinity" ]] ||
   [[ "$cached_g4go_sha256" != "$g4go_sha256" ]] ||
   [[ "$cached_cpu_benchmark_macro_sha256" != "$cpu_benchmark_macro_sha256" ]] ||
   [[ "$cpu_single_benchmark_events_cached" != "$benchmark_events" ]] ||
   ! is_positive_number "$cpu_single_measured"; then
    run_timed_cpu_phase \
        cpu-single-benchmark "$cpu_single_log" run_cpu_benchmark.root \
        cpu_single_measured single \
        "$benchmark_events" \
        --backend cpu --threads 1 --seed 42 "$cpu_benchmark_macro"
    cp -- run_cpu_benchmark.root "$cpu_single_reference" || fail_test 1
    rm -f -- run_cpu_benchmark.root
    cpu_single_source=generated
else
    echo "[cpu-single-benchmark] CACHED events=${benchmark_events} time=${cpu_single_measured}s"
fi

if [[ ! -s "$cpu_full_reference" ]] ||
   [[ "$cached_g4go_sha256" != "$g4go_sha256" ]] ||
   [[ "$cached_regression_macro_sha256" != "$regression_macro_sha256" ]] ||
   [[ "$(read_timing_value cpu_full_regression_events)" != "$target_events" ]] ||
   ! is_positive_number "$cpu_full_regression_elapsed"; then
    run_timed_cpu_phase \
        cpu-full-regression "$cpu_full_log" run_eminus_regression.root \
        cpu_full_regression_elapsed full \
        "$target_events" \
        "$g4go" --backend cpu --threads 14 --seed 42 "$regression_macro"
    cp -- run_eminus_regression.root "$cpu_full_reference" || fail_test 1
    cpu_full_regression_source=generated
else
    echo "[cpu-full-regression] CACHED events=${target_events} time=${cpu_full_regression_elapsed}s"
fi

write_cpu_timing \
    "$cpu_single_measured" \
    "$cpu_full_regression_elapsed"

if ! is_positive_number "$cpu_full_regression_elapsed"; then
    echo "CPU timing cache contains invalid values"
    fail_test 1
fi

cpu_estimated_real_time_s="$(awk \
    -v target_events="$target_events" \
    -v benchmark_events="$benchmark_events" \
    -v single_benchmark="$cpu_single_measured" \
    'BEGIN {
        if (target_events > 0.0 && benchmark_events > 0.0 &&
            single_benchmark > 0.0) {
            printf "%.6f", target_events * single_benchmark / benchmark_events;
        }
    }')"
if ! is_positive_number "$cpu_estimated_real_time_s"; then
    echo "Unable to estimate single-core CPU time"
    fail_test 1
fi

gpu_run_log="${run_dir}/gpu.log"
gpu_output_root="${regression_dir}/gpu_single.root"
rm -f -- run_eminus_regression.root "$gpu_output_root"
gpu_start_ns="$(now_ns)"
gpu_arguments=(
    --backend gpu
    --threads 1
    --seed 42
    --batch-timeout-ms "$batch_timeout_ms"
    --in-flight-batches "$max_in_flight_batches"
)
if ((perf_diagnostics)); then
    gpu_arguments+=(--perf-diagnostics)
fi
run_logged "$gpu_run_log" run_g4go_single \
    "${gpu_arguments[@]}" "$regression_macro"
command_status="$?"
if ((command_status != 0)); then
    echo "[gpu] FAIL events=${target_events} exit_code=${command_status}"
    fail_test "$command_status"
fi
gpu_elapsed_seconds="$(seconds_from_ns "$(( $(now_ns) - gpu_start_ns ))")"
if ! grep -F "[g4go] optical backend: gpu" "$gpu_run_log" >/dev/null 2>&1; then
    echo "[gpu] GPU backend marker is missing"
    fail_test 1
fi
if [[ ! -s run_eminus_regression.root ]]; then
    echo "[gpu] ROOT output is missing events=${target_events}"
    fail_test 1
fi
cp -- run_eminus_regression.root "$gpu_output_root" || fail_test 1
echo "[gpu] PASS events=${target_events} wall_time=${gpu_elapsed_seconds}s"

gpu_speedup="$(awk -v cpu="$cpu_estimated_real_time_s" \
                       -v gpu="$gpu_elapsed_seconds" \
                       'BEGIN { if (gpu > 0.0) printf "%.6f", cpu / gpu; }')"
if ! is_positive_number "$gpu_speedup"; then
    echo "Unable to calculate GPU speedup"
    fail_test 1
fi

{
    echo "cpu_affinity=${cpu_affinity}"
    echo "g4go_sha256=${g4go_sha256}"
    echo "cpu_benchmark_macro_sha256=${cpu_benchmark_macro_sha256}"
    echo "regression_macro_sha256=${regression_macro_sha256}"
    echo "cpu_geant4_threads=14"
    echo "gpu_geant4_threads=1"
    echo "batch_timeout_ms=${batch_timeout_ms}"
    echo "max_in_flight_batches=${max_in_flight_batches}"
    echo "cpu_single_reference_events=${cpu_single_reference_events}"
    echo "cpu_single_benchmark_time_for_estimate_s=${cpu_single_measured}"
    echo "cpu_single_benchmark_measured_real_time_s=${cpu_single_measured}"
    echo "cpu_single_benchmark_source=${cpu_single_source}"
    echo "cpu_target_events=${target_events}"
    echo "cpu_full_regression_events=${target_events}"
    echo "cpu_full_regression_real_time_s=${cpu_full_regression_elapsed}"
    echo "cpu_full_regression_source=${cpu_full_regression_source}"
    echo "cpu_estimated_real_time_s=${cpu_estimated_real_time_s}"
    echo "cpu_data_reference=$(basename "$cpu_full_reference")"
    echo "gpu_wall_time_s=${gpu_elapsed_seconds}"
    echo "gpu_speedup_vs_single_core_reference=${gpu_speedup}x"
    echo "gpu_data_reference=$(basename "$gpu_output_root")"
} > "${run_dir}/benchmark_summary.txt"

if ((perf_diagnostics)); then
    {
        echo "cpu_affinity=${cpu_affinity}"
        echo "cpu_geant4_threads=14"
        echo "gpu_geant4_threads=1"
        echo "batch_timeout_ms=${batch_timeout_ms}"
        echo "max_in_flight_batches=${max_in_flight_batches}"
        echo "[gpu]"
        rg '^\[g4go\] performance(_output)?:' \
            "$gpu_run_log" || true
    } > "${run_dir}/performance_summary.txt"
fi

compare_call="${compare_macro}(\"${cpu_full_reference}\",\"${gpu_output_root}\",${cpu_estimated_real_time_s},${gpu_elapsed_seconds},\"${run_dir}\")"
run_phase comparison "$comparison_log" "$root_executable" -l -b -q "$compare_call"
echo "[comparison] PASS"

for output_file in \
    regression_summary.txt \
    figures/noptpho_comparison.png figures/tof_comparison.png \
    figures/edep_comparison.png \
    noptpho_regression_report.root; do
    if [[ ! -s "${run_dir}/${output_file}" ]]; then
        echo "[comparison] required output is missing: ${output_file}"
        fail_test 1
    fi
done

mv -f -- "${run_dir}/noptpho_regression_report.root" \
    "${regression_dir}/noptpho_regression_report.root" || fail_test 1

echo "[regression] OUTPUT: PASSED"
print_summary
