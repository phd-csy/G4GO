#!/usr/bin/env bash

set -euo pipefail

usage() {
    printf '%s\n' \
        "Usage: $0 -c N [-g N] [-v] [-h]" \
        "       $0 --cores N [--gpu N] [--verbose] [--help]" \
        "" \
        "Select one allowed logical CPU from each GPU-local physical core." \
        "" \
        "Options:" \
        "  -g, --gpu N      NVIDIA GPU index (default: 0)" \
        "  -c, --cores N    Number of independent physical cores to select (required)" \
        "  -v, --verbose    Print detected topology to stderr" \
        "  -h, --help       Show this help"
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

trim() {
    local value="$1"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "${value}"
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command was not found: $1"
}

normalize_pci_bdf() {
    local raw_bdf domain remainder
    raw_bdf="$(trim "$1")"
    raw_bdf="${raw_bdf,,}"

    if [[ "${raw_bdf}" =~ ^[0-9a-f]{8}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]$ ]]; then
        domain="${raw_bdf%%:*}"
        remainder="${raw_bdf#*:}"
        printf '%s:%s' "${domain: -4}" "${remainder}"
        return 0
    fi
    if [[ "${raw_bdf}" =~ ^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]$ ]]; then
        printf '%s' "${raw_bdf}"
        return 0
    fi
    return 1
}

cpu_is_allowed() {
    local cpu="$1"
    local range first last
    local -a ranges

    IFS=',' read -r -a ranges <<<"${allowed_cpu_list}"
    for range in "${ranges[@]}"; do
        if [[ "${range}" == *-* ]]; then
            first="${range%%-*}"
            last="${range##*-}"
        else
            first="${range}"
            last="${range}"
        fi
        if ((cpu >= first && cpu <= last)); then
            return 0
        fi
    done
    return 1
}

gpu_index=0
requested_cores=""
verbose=0

while (($# > 0)); do
    case "$1" in
    -g|--gpu)
        (($# >= 2)) || die "missing value for --gpu"
        gpu_index="$2"
        shift 2
        ;;
    -g=*|--gpu=*)
        gpu_index="${1#*=}"
        shift
        ;;
    -c|--cores)
        (($# >= 2)) || die "missing value for --cores"
        requested_cores="$2"
        shift 2
        ;;
    -c=*|--cores=*)
        requested_cores="${1#*=}"
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

[[ "${gpu_index}" =~ ^[0-9]+$ ]] || die "--gpu must be a non-negative integer: ${gpu_index}"
[[ -n "${requested_cores}" ]] || die "--cores N is required"
[[ "${requested_cores}" =~ ^[1-9][0-9]*$ ]] ||
    die "--cores must be a positive integer: ${requested_cores}"

require_command nvidia-smi
require_command lscpu

if ! gpu_name="$(nvidia-smi --query-gpu=name --format=csv,noheader -i "${gpu_index}" 2>/dev/null)"; then
    die "NVIDIA GPU index does not exist or is inaccessible: ${gpu_index}"
fi
gpu_name="$(trim "${gpu_name%%$'\n'*}")"
[[ -n "${gpu_name}" ]] || die "NVIDIA GPU index does not exist or is inaccessible: ${gpu_index}"

if ! raw_gpu_bdf="$(nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader -i "${gpu_index}" 2>/dev/null)"; then
    die "unable to query PCI bus ID for NVIDIA GPU index ${gpu_index}"
fi
raw_gpu_bdf="$(trim "${raw_gpu_bdf%%$'\n'*}")"
gpu_bdf=""
if normalized_bdf="$(normalize_pci_bdf "${raw_gpu_bdf}")"; then
    gpu_bdf="${normalized_bdf}"
fi

# Prefer the PCI device's sysfs NUMA assignment. A value of -1 means that the
# kernel cannot associate the device with a NUMA node, so topology is tried.
gpu_numa_node=""
numa_source=""
if [[ -n "${gpu_bdf}" && -r "/sys/bus/pci/devices/${gpu_bdf}/numa_node" ]]; then
    sysfs_numa="$(<"/sys/bus/pci/devices/${gpu_bdf}/numa_node")"
    sysfs_numa="$(trim "${sysfs_numa}")"
    if [[ "${sysfs_numa}" =~ ^[0-9]+$ ]]; then
        gpu_numa_node="${sysfs_numa}"
        numa_source="sysfs"
    fi
fi

if [[ -z "${gpu_numa_node}" ]]; then
    gpu_indices="$(nvidia-smi --query-gpu=index --format=csv,noheader 2>/dev/null || true)"
    gpu_count=0
    while IFS= read -r detected_index; do
        detected_index="$(trim "${detected_index}")"
        if [[ "${detected_index}" =~ ^[0-9]+$ ]]; then
            gpu_count=$((gpu_count + 1))
        fi
    done <<<"${gpu_indices}"

    topology_matrix="$(nvidia-smi topo -m 2>/dev/null || true)"
    if ((gpu_count > 0)) && [[ -n "${topology_matrix}" ]]; then
        # A GPU row contains one connectivity column per GPU, followed by CPU
        # Affinity, NUMA Affinity, and GPU NUMA ID. Discrete GPUs commonly put
        # the useful node in NUMA Affinity and report GPU NUMA ID as N/A.
        topology_numa="$(awk -v gpu="GPU${gpu_index}" -v count="${gpu_count}" '
            $1 == gpu {
                numa_affinity = $(count + 3)
                gpu_numa_id = $NF
                if (numa_affinity ~ /^[0-9]+$/) {
                    print numa_affinity
                    exit
                }
                if (gpu_numa_id ~ /^[0-9]+$/) {
                    print gpu_numa_id
                    exit
                }
            }
        ' <<<"${topology_matrix}")"
        if [[ "${topology_numa}" =~ ^[0-9]+$ ]]; then
            gpu_numa_node="${topology_numa}"
            numa_source="nvidia-smi topo -m"
        fi
    fi
fi

if [[ -z "${gpu_numa_node}" ]]; then
    display_bdf="${gpu_bdf:-${raw_gpu_bdf:-unknown}}"
    die "unable to determine NUMA node for GPU ${gpu_index} (PCI BDF ${display_bdf})"
fi

allowed_cpu_list="$(awk '/^Cpus_allowed_list:/ {print $2; exit}' /proc/self/status)"
allowed_cpu_list="$(trim "${allowed_cpu_list}")"
[[ -n "${allowed_cpu_list}" ]] || die "unable to read Cpus_allowed_list from /proc/self/status"
if [[ ! "${allowed_cpu_list}" =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]]; then
    die "unable to parse Cpus_allowed_list: ${allowed_cpu_list}"
fi

if ! topology_output="$(lscpu -p=CPU,CORE,SOCKET,NODE 2>/dev/null)"; then
    die "unable to query CPU topology with lscpu"
fi

declare -A all_core_counts=()
declare -A gpu_local_core_seen=()
declare -a gpu_local_cpus=()
declare -a gpu_local_cores=()
declare -a gpu_local_sockets=()
declare -a gpu_local_nodes=()
parsed_cpu_rows=0

while IFS=, read -r cpu core socket node extra; do
    [[ -n "${cpu}" && "${cpu}" != \#* ]] || continue
    if [[ ! "${cpu}" =~ ^[0-9]+$ || ! "${core}" =~ ^[0-9]+$ ||
          ! "${socket}" =~ ^[0-9]+$ || ! "${node}" =~ ^[0-9]+$ ]]; then
        continue
    fi

    parsed_cpu_rows=$((parsed_cpu_rows + 1))
    physical_core_key="${socket}:${core}"
    all_core_counts["${physical_core_key}"]=$((${all_core_counts["${physical_core_key}"]:-0} + 1))

    [[ "${node}" == "${gpu_numa_node}" ]] || continue
    cpu_is_allowed "${cpu}" || continue
    [[ -z "${gpu_local_core_seen["${physical_core_key}"]+x}" ]] || continue

    # The first allowed logical CPU encountered becomes this physical core's
    # representative. Later SMT siblings with the same SOCKET:CORE are skipped.
    gpu_local_core_seen["${physical_core_key}"]=1
    gpu_local_cpus+=("${cpu}")
    gpu_local_cores+=("${core}")
    gpu_local_sockets+=("${socket}")
    gpu_local_nodes+=("${node}")
done <<<"${topology_output}"

((parsed_cpu_rows > 0)) || die "unable to parse CPU topology from lscpu"

available_cores=${#gpu_local_cpus[@]}
if ((available_cores < requested_cores)); then
    printf 'error: insufficient allowed physical cores on the GPU-local NUMA node\n' >&2
    printf '  requested cores : %s\n' "${requested_cores}" >&2
    printf '  available cores : %s\n' "${available_cores}" >&2
    printf '  GPU NUMA node   : %s\n' "${gpu_numa_node}" >&2
    printf '  allowed CPUs    : %s\n' "${allowed_cpu_list}" >&2
    exit 1
fi

declare -a selected_cpus=()
declare -a selected_cores=()
declare -a selected_sockets=()
declare -a selected_nodes=()
for ((index = 0; index < requested_cores; ++index)); do
    selected_cpus+=("${gpu_local_cpus[index]}")
    selected_cores+=("${gpu_local_cores[index]}")
    selected_sockets+=("${gpu_local_sockets[index]}")
    selected_nodes+=("${gpu_local_nodes[index]}")
done
selected_cpu_list="$(IFS=,; printf '%s' "${selected_cpus[*]}")"

smt_status="disabled"
for logical_count in "${all_core_counts[@]}"; do
    if ((logical_count > 1)); then
        smt_status="enabled"
        break
    fi
done

cpu_quota_description="unavailable"
cpu_quota_cores=""
cpu_quota_file="/sys/fs/cgroup/cpu.max"
if [[ ! -r "${cpu_quota_file}" ]]; then
    current_cgroup_path="$(awk -F: '$1 == "0" {print $3; exit}' /proc/self/cgroup)"
    if [[ "${current_cgroup_path}" == /* &&
          -r "/sys/fs/cgroup${current_cgroup_path}/cpu.max" ]]; then
        cpu_quota_file="/sys/fs/cgroup${current_cgroup_path}/cpu.max"
    fi
fi
if [[ -r "${cpu_quota_file}" ]]; then
    cpu_max="$(<"${cpu_quota_file}")"
    quota_value=""
    quota_period=""
    read -r quota_value quota_period _ <<<"${cpu_max}"
    if [[ "${quota_value}" == "max" ]]; then
        cpu_quota_description="unlimited"
    elif [[ "${quota_value}" =~ ^[0-9]+$ && "${quota_period}" =~ ^[1-9][0-9]*$ ]]; then
        cpu_quota_cores="$(awk -v quota="${quota_value}" -v period="${quota_period}" \
            'BEGIN {printf "%.2f", quota / period}')"
        cpu_quota_description="${cpu_quota_cores} cores"
        if awk -v requested="${requested_cores}" -v quota="${quota_value}" \
            -v period="${quota_period}" 'BEGIN {exit !(requested > quota / period)}'; then
            printf 'warning: requested %s physical cores, but cgroup CPU quota is approximately %s cores\n' \
                "${requested_cores}" "${cpu_quota_cores}" >&2
        fi
    fi
fi

if ((verbose)); then
    printf '%-20s: %s\n' "GPU index" "${gpu_index}" >&2
    printf '%-20s: %s\n' "GPU name" "${gpu_name}" >&2
    printf '%-20s: %s\n' "GPU PCI BDF" "${gpu_bdf:-${raw_gpu_bdf}}" >&2
    printf '%-20s: %s\n' "GPU NUMA node" "${gpu_numa_node}" >&2
    printf '%-20s: %s\n' "NUMA source" "${numa_source}" >&2
    printf '%-20s: %s\n' "Cpus_allowed_list" "${allowed_cpu_list}" >&2
    printf '%-20s: %s\n' "CPU quota" "${cpu_quota_description}" >&2
    printf '%-20s: %s\n' "SMT" "${smt_status}" >&2
    printf '%-20s: %s\n' "GPU-local physical" "${available_cores}" >&2
    printf '%-20s: %s\n' "Requested cores" "${requested_cores}" >&2
    printf '%-20s: %s\n' "Selected CPUs" "${selected_cpu_list}" >&2
    printf '\n%-6s %-6s %-8s %-6s\n' "CPU" "CORE" "SOCKET" "NODE" >&2
    for ((index = 0; index < requested_cores; ++index)); do
        printf '%-6s %-6s %-8s %-6s\n' \
            "${selected_cpus[index]}" "${selected_cores[index]}" \
            "${selected_sockets[index]}" "${selected_nodes[index]}" >&2
    done
fi

printf '%s\n' "${selected_cpu_list}"
