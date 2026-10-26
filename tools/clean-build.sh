#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s esp32-v1|esp32-v2|rp2354|all\n' "${0##*/}"
    printf 'Remove generated artifact bundles; keep compiler caches and Docker images.\n'
}

if [[ "$#" == 1 && ( "$1" == --help || "$1" == -h ) ]]; then
    usage
    exit 0
fi

if [[ "$#" != 1 ]]; then
    usage >&2
    exit 1
fi

case "$1" in
    esp32-v1|esp32-v2|rp2354) targets=("$1") ;;
    all) targets=(esp32-v1 esp32-v2 rp2354) ;;
    *)
        usage >&2
        exit 1
        ;;
esac

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
lock_dir=

cleanup() {
    if [[ -n "$lock_dir" ]]; then
        rmdir -- "$lock_dir"
    fi
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for target in "${targets[@]}"; do
    output_dir="$repo_dir/dist/$target"

    if [[ -L "$repo_dir/dist" || -L "$output_dir" || -L "$output_dir/artifacts" ]]; then
        printf 'Refusing to clean artifacts through a symlink: %s\n' "$output_dir" >&2
        exit 1
    fi

    if [[ ! -d "$output_dir" ]]; then
        continue
    fi

    if ! mkdir "$output_dir/.build-lock" 2>/dev/null; then
        printf 'Build or cleanup already active, or stale lock: %s/.build-lock\n' "$output_dir" >&2
        exit 1
    fi

    lock_dir="$output_dir/.build-lock"
    rm -rf -- "$output_dir/artifacts"
    rmdir -- "$lock_dir"
    lock_dir=

    printf 'Removed %s/artifacts/\n' "$output_dir"
done
