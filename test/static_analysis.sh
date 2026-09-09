#!/usr/bin/env bash

set -euo pipefail

usage() {
    printf '%s\n' \
        "Usage: $0 --fix" \
        "       $0 --check" \
        "" \
        "Run clang-tidy over the G4GO C/C++ source files." \
        "" \
        "Modes:" \
        "  fix    Apply the stable, configured automatic fixes" \
        "  check  Run all checks from .clang-tidy and print diagnostics" \
        "" \
        "Environment:" \
        "  G4GO_BUILD_DIR             Override the build directory" \
        "  G4GO_CLANG_TIDY_JOBS      Number of parallel clang-tidy jobs"
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

say() {
    printf '>> %s\n' "$*"
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command was not found: $1"
}

if (($# != 1)); then
    usage >&2
    exit 1
fi

case "$1" in
--fix)
    mode="fix"
    ;;
--check)
    mode="check"
    ;;
--help|-h)
    usage
    exit 0
    ;;
*)
    printf 'error: unknown mode: %s\n' "$1" >&2
    usage >&2
    exit 1
    ;;
esac

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
script_parent="$(cd -- "${script_dir}/.." && pwd)"
if [[ -f "${script_parent}/CMakeLists.txt" ]]; then
    project_dir="$script_parent"
    default_build_dir="${project_dir}/build"
elif [[ -r "${script_parent}/CMakeCache.txt" ]]; then
    project_dir="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${script_parent}/CMakeCache.txt" | sed -n '1p')"
    [[ -f "${project_dir}/CMakeLists.txt" ]] ||
        die "unable to determine the source directory from ${script_parent}/CMakeCache.txt"
    default_build_dir="$script_parent"
else
    die "unable to determine the project directory from ${script_dir}"
fi
build_dir="${G4GO_BUILD_DIR:-${default_build_dir}}"
config_file="${project_dir}/.clang-tidy"

[[ -f "$config_file" ]] || die "clang-tidy configuration is missing: $config_file"

require_command cmake
require_command clang-tidy

run_clang_tidy=""
for candidate in run-clang-tidy.py run-clang-tidy; do
    if command -v "$candidate" >/dev/null 2>&1; then
        run_clang_tidy="$(command -v "$candidate")"
        break
    fi
done
[[ -n "$run_clang_tidy" ]] || die "run-clang-tidy.py was not found in PATH"

if [[ "$mode" == fix ]]; then
    require_command clang-apply-replacements
fi

jobs="${G4GO_CLANG_TIDY_JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || printf '1')}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "G4GO_CLANG_TIDY_JOBS must be a positive integer: $jobs"

mkdir -p "$build_dir"
if [[ ! -r "${build_dir}/compile_commands.json" ]]; then
    say "configuring CMake in ${build_dir}"
    cmake -S "$project_dir" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
fi
[[ -r "${build_dir}/compile_commands.json" ]] ||
    die "compile_commands.json is missing: ${build_dir}/compile_commands.json"

escape_regex() {
    sed 's/[.[\*^$()+?{|\\]/\\&/g'
}

project_regex="$(printf '%s\n' "$project_dir" | escape_regex)"
# The OptiX CUDA files use a custom nvcc/OptiX-IR command that is not a
# clang-tidy-compatible compilation command. Audit C++ translation units here;
# project headers are included through header-filter.
source_filter="^${project_regex}/(src|test)/.*\\.(cc|cpp|cxx)$"
header_filter="^${project_regex}/(include|src|test)/"

common_arguments=(
    -p "$build_dir"
    -config-file "$config_file"
    -header-filter "$header_filter"
    -source-filter "$source_filter"
    -j "$jobs"
)

if [[ "$mode" == fix ]]; then
    auto_fix_checks=(
        '-*'
        modernize-use-nullptr
        modernize-use-override
        modernize-use-using

        readability-container-size-empty
        readability-redundant-access-specifiers
        readability-redundant-string-init
        readability-redundant-smartptr-get
        readability-redundant-control-flow

        google-readability-namespace-comments
    )
    checks="$(IFS=,; printf '%s' "${auto_fix_checks[*]}")"
    say "applying stable clang-tidy fixes"
    "$run_clang_tidy" "${common_arguments[@]}" "-checks=${checks}" -fix
else
    say "running clang-tidy checks"
    "$run_clang_tidy" "${common_arguments[@]}"
fi
