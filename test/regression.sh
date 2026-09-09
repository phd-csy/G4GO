#!/usr/bin/env bash

set -uo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$(cd -- "${script_dir}/.." && pwd)"
batch_timeout_ms="${G4GO_BATCH_TIMEOUT_MS:-10}"
regression_threads=6
verbose=0

usage() {
    printf '%s\n' \
        "Usage: $0 [-t N|--threads N] [-v|--verbose]" \
        "" \
        "Run CPU/GPU optical correctness regression." \
        "" \
        "Options:" \
        "  -t, --threads N  CPU/GPU worker count (default: 6, minimum: 2)" \
        "  -v, --verbose    Stream phase logs" \
        "  -h, --help       Show this help" \
        "" \
        "Environment:" \
        "  G4GO_BATCH_TIMEOUT_MS  GPU batch timeout from 0 to 10000 (default: 10)"
}

while (($# > 0)); do
    case "$1" in
    -t|--threads)
        if (($# < 2)); then
            echo "Missing value for --threads" >&2
            exit 1
        fi
        regression_threads="$2"
        shift 2
        ;;
    -v|--verbose)
        verbose=1
        shift
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "Unknown argument: $1" >&2
        exit 1
        ;;
    esac
done

if [[ "$(pwd -P)" != "$build_dir" ]]; then
    echo "Run this script from the build directory: cd ${build_dir} && ./test/regression.sh" >&2
    exit 1
fi
if [[ ! "$regression_threads" =~ ^[1-9][0-9]*$ ]] || ((regression_threads < 2)); then
    echo "--threads must be an integer of at least 2: $regression_threads" >&2
    exit 1
fi
if [[ ! "$batch_timeout_ms" =~ ^[0-9]+$ ]] || ((batch_timeout_ms > 10000)); then
    echo "G4GO_BATCH_TIMEOUT_MS must be an integer from 0 to 10000: $batch_timeout_ms" >&2
    exit 1
fi

g4go="${build_dir}/g4go"
regression_macro="${build_dir}/scripts/run_eminus_regression.mac"
compare_macro="${build_dir}/test/TestOpticalTransport.cxx"
root_executable="$(command -v root || true)"
sha256sum_executable="$(command -v sha256sum || true)"
regression_dir="${build_dir}/test/regression"
run_timestamp="$(TZ=Asia/Shanghai date +%Y%m%d-%H%M%S)-$$"
run_dir="${regression_dir}/${run_timestamp}"
logs_dir="${run_dir}/logs"
figures_dir="${run_dir}/figures"
cpu_reference="${regression_dir}/cpu_${regression_threads}t.root"
cpu_cache_file="${regression_dir}/cpu_${regression_threads}t.cache"
gpu_1t_reference="${run_dir}/gpu_1t.root"
gpu_threads_reference="${run_dir}/gpu_${regression_threads}t.root"
cpu_log="${logs_dir}/cpu_${regression_threads}t.log"
gpu_1t_log="${logs_dir}/gpu_1t.log"
gpu_threads_log="${logs_dir}/gpu_${regression_threads}t.log"
comparison_1t_log="${logs_dir}/comparison_gpu_1t.log"
comparison_threads_log="${logs_dir}/comparison_gpu_${regression_threads}t.log"
regression_log="${logs_dir}/regression.log"

for required_file in "$g4go" "$regression_macro" "$compare_macro"; do
    if [[ ! -f "$required_file" ]]; then
        echo "Required runtime input is missing: $required_file" >&2
        exit 1
    fi
done
if [[ ! -x "$g4go" ]]; then
    echo "Executable is not runnable: $g4go" >&2
    exit 1
fi
if [[ -z "$root_executable" ]]; then
    echo "ROOT executable was not found in PATH" >&2
    exit 1
fi
if [[ -z "$sha256sum_executable" ]]; then
    echo "sha256sum is required to validate the CPU reference cache" >&2
    exit 1
fi

target_events="$(sed -nE \
    's@^[[:space:]]*/run/beamOn[[:space:]]+([0-9]+).*@\1@p' \
    "$regression_macro" | tail -n 1)"
if [[ ! "$target_events" =~ ^[1-9][0-9]*$ ]]; then
    echo "Unable to parse a positive /run/beamOn count from $regression_macro" >&2
    exit 1
fi

g4go_sha256="$("$sha256sum_executable" "$g4go" | awk '{print $1}')"
regression_macro_sha256="$("$sha256sum_executable" "$regression_macro" | awk '{print $1}')"

mkdir -p "$regression_dir" "$logs_dir" "$figures_dir"
cd "$regression_dir" || exit 1
: > "$regression_log"

exec > >(tee -a "$regression_log") 2>&1

print_summary() {
    echo "[regression] artifacts=${run_dir}"
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

read_cache_value() {
    local key="$1"
    if [[ -s "$cpu_cache_file" ]]; then
        awk -F= -v key="$key" \
            '$1 == key {print substr($0, index($0, "=") + 1); exit}' \
            "$cpu_cache_file"
    fi
}

write_cpu_cache() {
    {
        echo "seed=42"
        echo "g4go_sha256=${g4go_sha256}"
        echo "regression_macro_sha256=${regression_macro_sha256}"
        echo "threads=${regression_threads}"
        echo "events=${target_events}"
    } > "$cpu_cache_file"
}

run_phase() {
    local phase_name="$1"
    local log_file="$2"
    local destination="$3"
    shift 3

    rm -f -- run_eminus_regression.root
    run_logged "$log_file" "$@"
    local command_status="$?"
    if ((command_status != 0)); then
        echo "[${phase_name}] FAIL events=${target_events} exit_code=${command_status}"
        fail_test "$command_status"
    fi
    if [[ ! -s run_eminus_regression.root ]]; then
        echo "[${phase_name}] ROOT output is missing"
        fail_test 1
    fi
    mv -- run_eminus_regression.root "$destination" || fail_test 1
    echo "[${phase_name}] PASS events=${target_events}"
}

run_comparison() {
    local phase_name="$1"
    local log_file="$2"
    local label="$3"
    local gpu_file="$4"
    local compare_call summary_file figure_name

    compare_call="${compare_macro}(\"${cpu_reference}\",\"${gpu_file}\",\"${run_dir}\")"
    run_logged "$log_file" "$root_executable" -l -b -q "$compare_call"
    local command_status="$?"
    if ((command_status != 0)); then
        echo "[${phase_name}] FAIL exit_code=${command_status}"
        return "$command_status"
    fi

    summary_file="${run_dir}/${label}_regression_summary.txt"
    mv -- "${run_dir}/regression_summary.txt" "$summary_file" || return 1
    for figure_name in noptpho_comparison.png tof_comparison.png edep_comparison.png; do
        mv -- "${figures_dir}/${figure_name}" \
            "${figures_dir}/${label}_${figure_name}" || return 1
    done
    [[ -s "$summary_file" ]] || return 1
    echo "[${phase_name}] PASS"
}

echo "[regression] target_events=${target_events} threads=${regression_threads}"

cpu_reference_source=cached
if [[ ! -s "$cpu_reference" ]] ||
   [[ "$(read_cache_value g4go_sha256)" != "$g4go_sha256" ]] ||
   [[ "$(read_cache_value regression_macro_sha256)" != "$regression_macro_sha256" ]] ||
   [[ "$(read_cache_value threads)" != "$regression_threads" ]] ||
   [[ "$(read_cache_value events)" != "$target_events" ]]; then
    run_phase "cpu-${regression_threads}t" "$cpu_log" "$cpu_reference" \
        "$g4go" --backend cpu --threads "$regression_threads" \
        --seed 42 "$regression_macro"
    write_cpu_cache
    cpu_reference_source=generated
else
    echo "[cpu-${regression_threads}t] CACHED events=${target_events}"
fi

gpu_arguments=(
    --backend gpu
    --seed 42
    --batch-timeout-ms "$batch_timeout_ms"
)
run_phase gpu-1t "$gpu_1t_log" "$gpu_1t_reference" \
    "$g4go" "${gpu_arguments[@]}" --threads 1 "$regression_macro"
if ! grep -F "[g4go] optical backend: gpu" "$gpu_1t_log" >/dev/null 2>&1; then
    echo "[gpu-1t] GPU backend marker is missing"
    fail_test 1
fi

run_phase "gpu-${regression_threads}t" "$gpu_threads_log" "$gpu_threads_reference" \
    "$g4go" "${gpu_arguments[@]}" --threads "$regression_threads" \
    "$regression_macro"
if ! grep -F "[g4go] optical backend: gpu" "$gpu_threads_log" >/dev/null 2>&1; then
    echo "[gpu-${regression_threads}t] GPU backend marker is missing"
    fail_test 1
fi

comparison_failed=0
run_comparison "comparison-gpu-1t-vs-cpu-${regression_threads}t" \
    "$comparison_1t_log" gpu_1t "$gpu_1t_reference" || comparison_failed=1
run_comparison "comparison-gpu-${regression_threads}t-vs-cpu-${regression_threads}t" \
    "$comparison_threads_log" "gpu_${regression_threads}t" \
    "$gpu_threads_reference" || comparison_failed=1
if ((comparison_failed)); then
    fail_test 1
fi

{
    printf '%s\n' \
        'G4GO Optical Regression Summary' \
        '================================' \
        '' \
        'Overall result' \
        '--------------' \
        'Status                 : PASSED' \
        "Reference              : CPU-${regression_threads}T ROOT output (${cpu_reference_source})" \
        "Regression events      : ${target_events}" \
        '' \
        "Optical comparison: GPU-1T vs CPU-${regression_threads}T" \
        '-----------------------------------------------'
    cat "${run_dir}/gpu_1t_regression_summary.txt"
    printf '%s\n' \
        '' \
        "Optical comparison: GPU-${regression_threads}T vs CPU-${regression_threads}T" \
        '--------------------------------------------------'
    cat "${run_dir}/gpu_${regression_threads}t_regression_summary.txt"
} > "${run_dir}/regression_summary.txt"
rm -f -- "${run_dir}/gpu_1t_regression_summary.txt" \
    "${run_dir}/gpu_${regression_threads}t_regression_summary.txt"

echo "[regression] OUTPUT: PASSED"
print_summary
