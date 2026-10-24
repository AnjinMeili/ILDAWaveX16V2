#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s esp32 [--board-version 1|2]\n' "${0##*/}"
    printf '       %s rp2354\n' "${0##*/}"
    printf 'Build the matching Docker image, then export firmware under dist/.\n'
}

if [[ "$#" == 1 && ( "$1" == --help || "$1" == -h ) ]]; then
    usage
    exit 0
fi

if [[ "$#" == 0 ]]; then
    usage >&2
    exit 1
fi

target=$1
shift
container_args=()

case "$target" in
    esp32)
        board_version=2

        if [[ "$#" != 0 ]]; then
            if [[ "$#" != 2 || "$1" != --board-version || ! "$2" =~ ^[12]$ ]]; then
                usage >&2
                exit 1
            fi

            board_version=$2
        fi

        output_name="esp32-v$board_version"
        container_args=(--board-version "$board_version")
        ;;
    rp2354)
        if [[ "$#" != 0 ]]; then
            usage >&2
            exit 1
        fi

        output_name=rp2354
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
output_dir="$repo_dir/dist/$output_name"

if ! command -v docker > /dev/null; then
    printf 'Docker is required.\n' >&2
    exit 1
fi

if [[ -L "$repo_dir/dist" || -L "$output_dir" || -L "$output_dir/artifacts" ]]; then
    printf 'Refusing to replace artifacts through a symlink: %s\n' "$output_dir" >&2
    exit 1
fi

mkdir -p "$output_dir"
lock_dir="$output_dir/.build-lock"

if ! mkdir "$lock_dir" 2>/dev/null; then
    printf 'Build or cleanup already active, or stale lock: %s\n' "$lock_dir" >&2
    exit 1
fi

cleanup() {
    # Restore the previous bundle if publication failed after moving it aside.
    if [[ -d "$lock_dir/previous" && ! -e "$output_dir/artifacts" ]]; then
        if ! mv -- "$lock_dir/previous" "$output_dir/artifacts"; then
            printf 'Previous artifacts retained at %s/previous\n' "$lock_dir" >&2
            return 1
        fi
    fi

    rm -rf -- "$lock_dir"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir "$lock_dir/output"

"$repo_dir/tools/build-images.sh" "$target"

docker run --rm --platform linux/amd64 \
    --user "$(id -u):$(id -g)" \
    --mount "type=bind,source=$repo_dir,target=/src,readonly" \
    --mount "type=bind,source=$lock_dir/output,target=/out" \
    "ildawave-$target-builder:local" "${container_args[@]}"

[[ -d "$lock_dir/output/artifacts" ]] || { printf 'Build produced no artifact bundle.\n' >&2; exit 1; }

if [[ -e "$output_dir/artifacts" ]]; then
    mv -- "$output_dir/artifacts" "$lock_dir/previous"
fi

mv -- "$lock_dir/output/artifacts" "$output_dir/artifacts"

printf 'Firmware exported to %s/artifacts/\n' "$output_dir"
