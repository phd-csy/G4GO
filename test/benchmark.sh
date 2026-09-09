#!/usr/bin/env bash

set -uo pipefail

usage() {
    printf '%s\n' \
        "Usage: $0 [-t N] [-g N] [-a MODE] [-r N] [-v] [-h]" \
        "       $0 [--threads N] [--gpu N] [--cpu-affinity MODE]" \
        "          [--reserve-cores N] [--verbose] [--help]" \
        "" \
        "Run CPU/GPU performance measurements without correctness checks." \
        "" \
        "Options:" \
        "  -t, --threads N          Matching CPU/GPU worker count (default: 6)" \
        "  -g, --gpu N              NVIDIA GPU index (default: 0)" \
        "  -a, --cpu-affinity MODE  none, auto, or a taskset CPU list (default: none)" \
        "  -r, --reserve-cores N    Extra physical cores selected by auto mode (default: 1)" \
        "  -v, --verbose            Stream phase logs and detected CPU topology" \
        "  -h, --help               Show this help" \
        "" \
        "Environment:" \
        "  G4GO_BATCH_TIMEOUT_MS  GPU batch timeout from 0 to 10000 (default: 10)" \
        "  G4GO_PERF_DIAGNOSTICS  Write detailed GPU diagnostics: 0 or 1 (default: 0)" \
        "  G4GO_BUILD_DIR          Override the configured build directory"
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

threads=6
gpu_index=0
cpu_affinity=none
reserve_cores=1
batch_timeout_ms="${G4GO_BATCH_TIMEOUT_MS:-10}"
perf_diagnostics="${G4GO_PERF_DIAGNOSTICS:-0}"
verbose=0

while (($# > 0)); do
    case "$1" in
    -t|--threads)
        (($# >= 2)) || die "missing value for --threads"
        threads="$2"
        shift 2
        ;;
    -t=*|--threads=*)
        threads="${1#*=}"
        shift
        ;;
    -g|--gpu)
        (($# >= 2)) || die "missing value for --gpu"
        gpu_index="$2"
        shift 2
        ;;
    -g=*|--gpu=*)
        gpu_index="${1#*=}"
        shift
        ;;
    -a|--cpu-affinity)
        (($# >= 2)) || die "missing value for --cpu-affinity"
        cpu_affinity="$2"
        shift 2
        ;;
    -a=*|--cpu-affinity=*)
        cpu_affinity="${1#*=}"
        shift
        ;;
    -r|--reserve-cores)
        (($# >= 2)) || die "missing value for --reserve-cores"
        reserve_cores="$2"
        shift 2
        ;;
    -r=*|--reserve-cores=*)
        reserve_cores="${1#*=}"
        shift
        ;;
    -v|--verbose)
        verbose=1
        shift
        ;;
    --help|-h)
        usage
        exit 0
        ;;
    *)
        die "unknown argument: $1"
        ;;
    esac
done

[[ "$threads" =~ ^[1-9][0-9]*$ ]] || die "--threads must be a positive integer: $threads"
((threads >= 2)) || die "--threads must be at least 2; GPU-1T is measured separately"
[[ "$gpu_index" =~ ^[0-9]+$ ]] || die "--gpu must be a non-negative integer: $gpu_index"
[[ "$reserve_cores" =~ ^[0-9]+$ ]] || die "--reserve-cores must be a non-negative integer: $reserve_cores"
[[ "$batch_timeout_ms" =~ ^[0-9]+$ ]] && ((batch_timeout_ms <= 10000)) ||
    die "G4GO_BATCH_TIMEOUT_MS must be an integer from 0 to 10000: $batch_timeout_ms"
[[ "$perf_diagnostics" == 0 || "$perf_diagnostics" == 1 ]] ||
    die "G4GO_PERF_DIAGNOSTICS must be 0 or 1: $perf_diagnostics"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${G4GO_BUILD_DIR:-}"
if [[ -z "$build_dir" ]]; then
    if [[ -x "${script_dir}/../g4go" ]]; then
        build_dir="$(cd -- "${script_dir}/.." && pwd)"
    elif [[ -x "${script_dir}/../build/g4go" ]]; then
        build_dir="$(cd -- "${script_dir}/../build" && pwd)"
    else
        die "unable to locate the g4go build; set G4GO_BUILD_DIR"
    fi
fi
[[ -d "$build_dir" ]] || die "build directory does not exist: $build_dir"
build_dir="$(cd -- "$build_dir" && pwd)"

g4go="${build_dir}/g4go"
full_macro="${build_dir}/scripts/run_beam_eminus.mac"
partial_macro="${build_dir}/scripts/run_cpu_scaling.mac"
affinity_helper="${script_dir}/cpu_affinity.sh"
[[ -x "$g4go" ]] || die "g4go executable is missing: $g4go"
[[ -f "$full_macro" ]] || die "full benchmark macro is missing: $full_macro"
[[ -f "$partial_macro" ]] || die "partial benchmark macro is missing: $partial_macro"

full_events="$(sed -nE 's@^[[:space:]]*/run/beamOn[[:space:]]+([0-9]+).*@\1@p' "$full_macro" | tail -n 1)"
partial_events="$(sed -nE 's@^[[:space:]]*/run/beamOn[[:space:]]+([0-9]+).*@\1@p' "$partial_macro" | tail -n 1)"
[[ "$full_events" =~ ^[1-9][0-9]*$ ]] || die "unable to parse full benchmark event count"
[[ "$partial_events" =~ ^[1-9][0-9]*$ ]] || die "unable to parse partial benchmark event count"

cpu_list=""
case "$cpu_affinity" in
none)
    ;;
auto)
    [[ -x "$affinity_helper" ]] || die "CPU affinity helper is missing: $affinity_helper"
    affinity_arguments=(--gpu "$gpu_index" --cores "$((threads + reserve_cores))")
    if ((verbose)); then
        affinity_arguments+=(--verbose)
    fi
    cpu_list="$($affinity_helper "${affinity_arguments[@]}")" ||
        die "automatic CPU affinity failed; use --cpu-affinity none or provide a CPU list"
    ;;
*)
    cpu_list="$cpu_affinity"
    ;;
esac

taskset_executable=""
if [[ -n "$cpu_list" ]]; then
    taskset_executable="$(command -v taskset || true)"
    [[ -n "$taskset_executable" ]] || die "taskset is required when CPU affinity is enabled"
    "$taskset_executable" --cpu-list "$cpu_list" true >/dev/null 2>&1 ||
        die "CPU affinity list is invalid or unavailable: $cpu_list"
fi

sha256sum_executable="$(command -v sha256sum || true)"
[[ -n "$sha256sum_executable" ]] || die "sha256sum is required to record benchmark inputs"
g4go_sha256="$("$sha256sum_executable" "$g4go" | awk '{print $1}')"
full_macro_sha256="$("$sha256sum_executable" "$full_macro" | awk '{print $1}')"
partial_macro_sha256="$("$sha256sum_executable" "$partial_macro" | awk '{print $1}')"

benchmark_dir="${build_dir}/test/benchmark"
run_timestamp="$(TZ=Asia/Shanghai date +%Y%m%d-%H%M%S)-$$"
run_dir="${benchmark_dir}/${run_timestamp}"
logs_dir="${run_dir}/logs"
benchmark_log="${logs_dir}/benchmark.log"
cpu_1t_partial_log="${logs_dir}/cpu_1t_partial.log"
cpu_threads_partial_log="${logs_dir}/cpu_${threads}t_partial.log"
cpu_threads_full_log="${logs_dir}/cpu_${threads}t_full.log"
gpu_1t_full_log="${logs_dir}/gpu_1t_full.log"
gpu_threads_full_log="${logs_dir}/gpu_${threads}t_full.log"
mkdir -p "$logs_dir"
cd "$run_dir" || exit 1
: > "$benchmark_log"
exec > >(tee -a "$benchmark_log") 2>&1

fail_benchmark() {
    local exit_code="$1"
    echo "[benchmark] OUTPUT: FAILED exit_code=$exit_code"
    echo "[benchmark] artifacts=${run_dir}"
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

run_with_affinity() {
    if [[ -n "$cpu_list" ]]; then
        "$taskset_executable" --cpu-list "$cpu_list" "$@"
    else
        "$@"
    fi
}

now_ns() {
    date +%s%N
}

seconds_from_ns() {
    awk -v nanoseconds="$1" 'BEGIN {printf "%.6f", nanoseconds / 1000000000.0}'
}

run_timed_phase() {
    local phase_name="$1"
    local log_file="$2"
    local output_file="$3"
    local elapsed_variable="$4"
    local event_count="$5"
    shift 5
    local start_ns elapsed_seconds command_status

    rm -f -- "$output_file"
    start_ns="$(now_ns)"
    run_logged "$log_file" run_with_affinity "$@"
    command_status="$?"
    if ((command_status != 0)); then
        echo "[${phase_name}] FAIL events=${event_count} exit_code=${command_status}"
        fail_benchmark "$command_status"
    fi
    elapsed_seconds="$(seconds_from_ns "$(( $(now_ns) - start_ns ))")"
    [[ -s "$output_file" ]] || {
        echo "[${phase_name}] ROOT output is missing"
        fail_benchmark 1
    }
    printf -v "$elapsed_variable" '%s' "$elapsed_seconds"
    echo "[${phase_name}] PASS events=${event_count} wall_time=${elapsed_seconds}s"
}

is_positive_number() {
    [[ "$1" =~ ^[0-9]+([.][0-9]+)?$ ]] || return 1
    awk -v value="$1" 'BEGIN {exit !(value > 0.0)}'
}

is_nonnegative_number() {
    [[ "$1" =~ ^[0-9]+([.][0-9]+)?$ ]] || return 1
    awk -v value="$1" 'BEGIN {exit !(value >= 0.0)}'
}

read_last_transport_time_ms() {
    awk '
        index($0, "[g4go] optical backend:") > 0 {
            for (field = 1; field <= NF; ++field) {
                if ($field == "transport_ms:") {
                    value = $(field + 1)
                    gsub(/,/, "", value)
                    last = value
                }
            }
        }
        END {if (last != "") print last}
    ' "$1"
}

read_last_root_output_time_ms() {
    awk -F= '
        index($0, "performance_output: root_output_ms=") > 0 {
            value = $2
            gsub(/[[:space:]]/, "", value)
            last = value
        }
        END {if (last != "") print last}
    ' "$1"
}

read_performance_value() {
    local key="$1"
    local log_file="$2"
    awk -v key="$key" '
        index($0, "performance:") > 0 {
            line = $0
            sub(/^.*performance:[[:space:]]*/, "", line)
            count = split(line, fields, ", ")
            for (field = 1; field <= count; ++field) {
                prefix = key "="
                if (index(fields[field], prefix) == 1) value = substr(fields[field], length(prefix) + 1)
            }
        }
        END {if (value != "") print value}
    ' "$log_file"
}

write_performance_row() {
    local label="$1"
    local key="$2"
    local gpu_1t_value gpu_threads_value
    gpu_1t_value="$(read_performance_value "$key" "$gpu_1t_full_log")"
    gpu_threads_value="$(read_performance_value "$key" "$gpu_threads_full_log")"
    [[ -n "$gpu_1t_value" ]] || gpu_1t_value="not recorded"
    [[ -n "$gpu_threads_value" ]] || gpu_threads_value="not recorded"
    printf '%-36s %18s %18s\n' "$label" "$gpu_1t_value" "$gpu_threads_value"
}

echo "[benchmark] full_events=${full_events} partial_events=${partial_events} threads=${threads}"
echo "[benchmark] cpu_affinity=${cpu_list:-none} gpu_index=${gpu_index}"

run_timed_phase cpu-1t-partial "$cpu_1t_partial_log" run_cpu_scaling.root \
    cpu_1t_partial_elapsed_seconds "$partial_events" \
    "$g4go" --backend cpu --threads 1 --seed 42 "$partial_macro"
mv -- run_cpu_scaling.root cpu_1t_partial.root || fail_benchmark 1

run_timed_phase "cpu-${threads}t-partial" "$cpu_threads_partial_log" \
    run_cpu_scaling.root cpu_threads_partial_elapsed_seconds "$partial_events" \
    "$g4go" --backend cpu --threads "$threads" --seed 42 "$partial_macro"
mv -- run_cpu_scaling.root "cpu_${threads}t_partial.root" || fail_benchmark 1

run_timed_phase "cpu-${threads}t-full" "$cpu_threads_full_log" \
    run_beam_eminus.root cpu_threads_full_elapsed_seconds "$full_events" \
    "$g4go" --backend cpu --threads "$threads" --seed 42 "$full_macro"
mv -- run_beam_eminus.root "cpu_${threads}t_full.root" || fail_benchmark 1

gpu_arguments=(--backend gpu --seed 42 --batch-timeout-ms "$batch_timeout_ms")
if ((perf_diagnostics)); then
    gpu_arguments+=(--diagnostics)
fi
run_timed_phase gpu-1t-full "$gpu_1t_full_log" run_beam_eminus.root \
    gpu_1t_full_elapsed_seconds "$full_events" env CUDA_VISIBLE_DEVICES="$gpu_index" \
    "$g4go" "${gpu_arguments[@]}" --threads 1 "$full_macro"
mv -- run_beam_eminus.root gpu_1t_full.root || fail_benchmark 1

run_timed_phase "gpu-${threads}t-full" "$gpu_threads_full_log" \
    run_beam_eminus.root gpu_threads_full_elapsed_seconds "$full_events" \
    env CUDA_VISIBLE_DEVICES="$gpu_index" "$g4go" "${gpu_arguments[@]}" \
    --threads "$threads" "$full_macro"
mv -- run_beam_eminus.root "gpu_${threads}t_full.root" || fail_benchmark 1

for gpu_log in "$gpu_1t_full_log" "$gpu_threads_full_log"; do
    grep -F "[g4go] optical backend: gpu" "$gpu_log" >/dev/null 2>&1 || {
        echo "GPU backend marker is missing from $gpu_log"
        fail_benchmark 1
    }
done

cpu_scaling_ratio="$(awk -v one="$cpu_1t_partial_elapsed_seconds" -v many="$cpu_threads_partial_elapsed_seconds" \
    'BEGIN {if (one > 0.0 && many > 0.0) printf "%.9f", one / many}')"
cpu_1t_estimated_full_time_s="$(awk -v cpu="$cpu_threads_full_elapsed_seconds" -v ratio="$cpu_scaling_ratio" \
    'BEGIN {if (cpu > 0.0 && ratio > 0.0) printf "%.6f", cpu * ratio}')"
gpu_1t_speedup="$(awk -v cpu="$cpu_1t_estimated_full_time_s" -v gpu="$gpu_1t_full_elapsed_seconds" \
    'BEGIN {if (cpu > 0.0 && gpu > 0.0) printf "%.6f", cpu / gpu}')"
gpu_threads_speedup="$(awk -v cpu="$cpu_threads_full_elapsed_seconds" -v gpu="$gpu_threads_full_elapsed_seconds" \
    'BEGIN {if (cpu > 0.0 && gpu > 0.0) printf "%.6f", cpu / gpu}')"
if ! is_positive_number "$cpu_scaling_ratio" || ! is_positive_number "$cpu_1t_estimated_full_time_s" ||
   ! is_positive_number "$gpu_1t_speedup" || ! is_positive_number "$gpu_threads_speedup"; then
    echo "Unable to calculate CPU scaling or GPU speedup"
    fail_benchmark 1
fi

gpu_1t_transport_ms="$(read_last_transport_time_ms "$gpu_1t_full_log")"
gpu_threads_transport_ms="$(read_last_transport_time_ms "$gpu_threads_full_log")"
gpu_1t_root_output_ms="$(read_last_root_output_time_ms "$gpu_1t_full_log")"
gpu_threads_root_output_ms="$(read_last_root_output_time_ms "$gpu_threads_full_log")"
if ! is_nonnegative_number "$gpu_1t_transport_ms" || ! is_nonnegative_number "$gpu_threads_transport_ms" ||
   ! is_nonnegative_number "$gpu_1t_root_output_ms" || ! is_nonnegative_number "$gpu_threads_root_output_ms"; then
    echo "GPU transport or ROOT output timing is missing"
    fail_benchmark 1
fi

{
    printf '%s\n' \
        'G4GO Benchmark Summary' \
        '======================' \
        '' \
        'Run configuration' \
        '-----------------' \
        "CPU/GPU workers       : ${threads}" \
        "CPU affinity          : ${cpu_list:-none}" \
        "GPU index             : ${gpu_index}" \
        "Full events           : ${full_events}" \
        "Partial events        : ${partial_events}" \
        "Batch timeout         : ${batch_timeout_ms} ms" \
        "Detailed diagnostics  : $([[ "$perf_diagnostics" == 1 ]] && echo enabled || echo disabled)" \
        '' \
        'Build inputs' \
        '------------' \
        "g4go SHA-256          : ${g4go_sha256}" \
        "Full macro SHA        : ${full_macro_sha256}" \
        "Partial macro SHA     : ${partial_macro_sha256}" \
        '' \
        'CPU scaling' \
        '-----------' \
        "CPU-1T partial        : ${cpu_1t_partial_elapsed_seconds} s" \
        "CPU-${threads}T partial      : ${cpu_threads_partial_elapsed_seconds} s" \
        "Scaling ratio (1T/N)  : ${cpu_scaling_ratio}" \
        "CPU-${threads}T full         : ${cpu_threads_full_elapsed_seconds} s" \
        "Estimated CPU-1T full : ${cpu_1t_estimated_full_time_s} s" \
        '' \
        'GPU timing breakdown' \
        '--------------------'
    printf '%-20s %18s %24s %22s\n' 'Phase' 'Total wall (s)' 'GPU transport (ms)' 'ROOT output (ms)'
    printf '%-20s %18s %24s %22s\n' 'GPU-1T full' "$gpu_1t_full_elapsed_seconds" \
        "$gpu_1t_transport_ms" "$gpu_1t_root_output_ms"
    printf '%-20s %18s %24s %22s\n' "GPU-${threads}T full" "$gpu_threads_full_elapsed_seconds" \
        "$gpu_threads_transport_ms" "$gpu_threads_root_output_ms"
    printf '%s\n' \
        '' \
        'Performance comparison' \
        '----------------------' \
        "GPU-1T vs estimated CPU-1T : ${gpu_1t_speedup} x" \
        "GPU-${threads}T vs CPU-${threads}T : ${gpu_threads_speedup} x" \
        '' \
        'Result: PASSED'
} > "${run_dir}/benchmark_summary.txt"

if ((perf_diagnostics)); then
    {
        printf '%s\n' 'G4GO GPU Pipeline Diagnostics' '=============================' '' 'Timing overview' '----------------'
        printf '%-36s %18s %18s\n' 'Metric' 'GPU-1T' "GPU-${threads}T"
        printf '%-36s %18s %18s\n' 'Total wall (s)' "$gpu_1t_full_elapsed_seconds" "$gpu_threads_full_elapsed_seconds"
        printf '%-36s %18s %18s\n' 'GPU optical transport (ms)' "$gpu_1t_transport_ms" "$gpu_threads_transport_ms"
        printf '%-36s %18s %18s\n' 'ROOT output (ms)' "$gpu_1t_root_output_ms" "$gpu_threads_root_output_ms"
        printf '%s\n' '' 'CUDA and scheduler stages (ms)' '-----------------------------'
        printf '%-36s %18s %18s\n' 'Metric' 'GPU-1T' "GPU-${threads}T"
        write_performance_row 'Capture total' capture_total_ms
        write_performance_row 'Locate volume' capture_locate_volume_ms
        write_performance_row 'Volume mapping' capture_volume_mapping_ms
        write_performance_row 'Scheduler queue wait' scheduler_queue_wait_ms
        write_performance_row 'Batch flatten' scheduler_batch_flatten_ms
        write_performance_row 'Host to device' host_to_device_ms
        write_performance_row 'Device memset' device_memset_ms
        write_performance_row 'OptiX kernel' optix_kernel_ms
        write_performance_row 'Device hit compaction' device_hit_compaction_ms
        write_performance_row 'Device metadata to host' device_metadata_to_host_ms
        write_performance_row 'Device hits to host' device_hits_to_host_ms
        write_performance_row 'Device to host' device_to_host_ms
        write_performance_row 'Host hit compaction' host_hit_compaction_ms
        write_performance_row 'Scheduler input wait' scheduler_input_wait_ms
        write_performance_row 'Scheduler GPU wait' scheduler_gpu_wait_ms
        printf '%s\n' '' 'Transport counters' '------------------'
        printf '%-36s %18s %18s\n' 'Metric' 'GPU-1T' "GPU-${threads}T"
        write_performance_row 'Total bounce count' total_bounce_count
        write_performance_row 'Candidate trace count' coincident_candidate_trace_count
        write_performance_row 'Candidate hit count' coincident_candidate_hit_count
        write_performance_row 'Cerenkov photons' cerenkov_photon_count
        write_performance_row 'Scintillation photons' scintillation_photon_count
        write_performance_row 'Detected photons' detected_count
        write_performance_row 'Absorbed photons' absorbed_count
        write_performance_row 'Escaped photons' escaped_count
        write_performance_row 'Truncated photons' truncated_count
        write_performance_row 'Invalid states' invalid_state_count
        write_performance_row 'Queue wait count' queue_wait_count
        write_performance_row 'Average batch photons' average_batch_photons
        write_performance_row 'Minimum batch photons' min_batch_photons
    } > "${run_dir}/performance_summary.txt"
fi

echo "[benchmark] OUTPUT: PASSED"
echo "[benchmark] GPU-1T speedup vs estimated CPU-1T=${gpu_1t_speedup}x"
echo "[benchmark] GPU-${threads}T speedup vs CPU-${threads}T=${gpu_threads_speedup}x"
echo "[benchmark] artifacts=${run_dir}"
