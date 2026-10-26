#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s [esp32|rp2354|all]\n' "${0##*/}"
    printf 'Build local Docker builder images (default: both).\n'
}

if [[ "$#" == 1 && ( "$1" == --help || "$1" == -h ) ]]; then
    usage
    exit 0
fi

if [[ "$#" -gt 1 ]]; then
    usage >&2
    exit 1
fi

target=${1:-all}

case "$target" in
    esp32) targets=(esp32) ;;
    rp2354) targets=(rp2354) ;;
    all) targets=(esp32 rp2354) ;;
    *)
        usage >&2
        exit 1
        ;;
esac

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)

if ! command -v docker > /dev/null; then
    printf 'Docker is required.\n' >&2
    exit 1
fi

for target in "${targets[@]}"; do
    docker build --platform linux/amd64 \
        -f "$repo_dir/docker/$target/Dockerfile" \
        -t "ildawave-$target-builder:local" "$repo_dir"
done
