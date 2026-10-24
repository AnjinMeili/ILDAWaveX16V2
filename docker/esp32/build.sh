#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

board_version=2

case "$#" in
    0) ;;
    2)
        [[ "$1" == --board-version ]] || fail 'Expected --board-version 1|2.'
        board_version=$2
        ;;
    *) fail 'Usage: build-firmware [--board-version 1|2]' ;;
esac

[[ "$board_version" == 1 || "$board_version" == 2 ]] || fail 'Board version must be 1 or 2.'
[[ -d /src/.git || -f /src/.git ]] || fail '/src must be a Git checkout.'
[[ -d /out && -w /out ]] || fail 'Mount an empty, writable directory at /out.'
[[ -z "$(find /out -mindepth 1 -maxdepth 1 -print -quit)" ]] || fail '/out must be empty.'

# Containers isolate this fixed path. Stable debug paths allow safe cache reuse
# without disabling ccache's directory hashing or changing compiler flags.
workspace=/tmp/ildawave-esp32
mkdir "$workspace"
export_stage=

cleanup() {
    rm -rf -- "$workspace"

    if [[ -n "$export_stage" ]]; then
        rm -rf -- "$export_stage"
    fi
}

trap cleanup EXIT

# Arbitrary Linux UIDs need writable tool state without a passwd entry.
export HOME="$workspace/home"
mkdir -p "$HOME" "$workspace/project"

git_source() {
    git -c safe.directory=/src -C /src "$@"
}

source_commit=$(git_source rev-parse HEAD)
source_state=clean

if [[ -n "$(git_source status --porcelain --untracked-files=normal)" ]]; then
    source_state=dirty
fi

# Track the selected input set explicitly; never copy host build/configuration state.
git_source ls-files -z -- \
    firmware/ESP32S3/CMakeLists.txt \
    firmware/ESP32S3/sdkconfig.defaults \
    firmware/ESP32S3/dependencies.lock \
    firmware/ESP32S3/main > "$workspace/inputs"

[[ -s "$workspace/inputs" ]] || fail 'No ESP sources found.'

tar -C /src --null -T "$workspace/inputs" -cf - \
    | tar -C "$workspace/project" --strip-components=2 -xf -

cd "$workspace/project"
[[ -f dependencies.lock && -f sdkconfig.defaults ]] || fail 'Missing locked dependencies or configuration defaults.'
cp dependencies.lock "$workspace/dependencies.lock"

find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > "$workspace/input-sha256.txt"
input_digest=$(sha256sum "$workspace/input-sha256.txt" | cut -d ' ' -f 1)

# The upstream exporter is not compatible with nounset in every shell path.
set +u
# ESP-IDF supplies this file inside the pinned image.
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh"
set -u

[[ "$(idf.py --version)" == 'ESP-IDF v6.0' ]] || fail 'Unexpected ESP-IDF version.'

export CCACHE_BASEDIR="$workspace/project"

if [[ -d /cache ]]; then
    [[ -w /cache ]] || fail '/cache must be writable by the container user.'
    config_digest=$(sha256sum sdkconfig.defaults | cut -d ' ' -f 1)
    export CCACHE_DIR="/cache/esp32s3-idf6.0-board${board_version}-${config_digest}"
else
    export CCACHE_DIR="$workspace/ccache"
fi

# GCC finds this specs file in its installation; ccache 4.9.1 looks in the
# compile directory. Expose the same file for hashing without changing flags.
specs_file=$(xtensa-esp32s3-elf-gcc -print-file-name=picolibc.specs)
[[ -f "$specs_file" ]] || fail 'Compiler picolibc.specs file is missing.'
mkdir -p build/bootloader
ln -s "$specs_file" build/picolibc.specs
ln -s "$specs_file" build/bootloader/picolibc.specs

idf.py --ccache -B build -D IDF_TARGET=esp32s3 -D "BOARD_VERSION=$board_version" build
cmp dependencies.lock "$workspace/dependencies.lock" || fail 'Build changed dependencies.lock; review dependency resolution separately.'

bundle="$workspace/artifacts"
mkdir -p "$bundle"

# Preserve generated relative paths so flash metadata still references exported files.
jq -e '.flash_files | type == "object" and length > 0' build/flasher_args.json > /dev/null
jq -r '.flash_files | to_entries[] | [.key, .value] | @tsv' build/flasher_args.json > "$workspace/flash-files"

while IFS=$'\t' read -r offset file; do
    [[ "$offset" =~ ^0x[0-9a-fA-F]+$ ]] || fail "Invalid flash offset: $offset"
    [[ "$file" =~ ^[a-zA-Z0-9_./-]+$ && "$file" != /* && "/$file/" != */../* ]] || fail "Unsafe artifact path: $file"
    [[ -s "build/$file" ]] || fail "Missing or empty flash artifact: $file"
    install -D -m 644 "build/$file" "$bundle/$file"
done < "$workspace/flash-files"

# Check ranges against the actual selected build configuration, without inventing offsets.
flash_mb=$(sed -n 's/^CONFIG_ESPTOOLPY_FLASHSIZE="\([0-9][0-9]*\)MB"$/\1/p' sdkconfig)
[[ -n "$flash_mb" ]] || fail 'Cannot determine configured flash size.'

while IFS=$'\t' read -r offset file; do
    size=$(stat -c %s "build/$file")
    printf '%s %s\n' "$((offset))" "$((offset + size))"
done < "$workspace/flash-files" | sort -n > "$workspace/flash-ranges"

previous_end=0

while read -r start end; do
    (( start >= previous_end && end <= flash_mb * 1024 * 1024 )) || fail 'Flash artifacts overlap or exceed configured flash size.'
    previous_end=$end
done < "$workspace/flash-ranges"

for file in ILDAWaveX16V2-ESP32S3.elf ILDAWaveX16V2-ESP32S3.map flasher_args.json flash_args; do
    [[ -s "build/$file" ]] || fail "Missing artifact: $file"
    cp "build/$file" "$bundle/"
done

cp sdkconfig dependencies.lock "$bundle/"
cp "$workspace/input-sha256.txt" "$bundle/"

{
    printf 'source_commit=%s\nsource_state=%s\ninput_digest=%s\n' "$source_commit" "$source_state" "$input_digest"
    printf 'board_version=%s\ntarget=esp32s3\n' "$board_version"
    printf 'base_image=espressif/idf:v6.0@sha256:c41bed8b8b28815526b70eac307452fb432c7904a9157fbaaa04784ac4840ad7\n'
    idf.py --version
    xtensa-esp-elf-gcc --version
    cmake --version
    ninja --version
    sha256sum /usr/local/bin/build-firmware
} > "$bundle/build-info.txt"

(
    cd "$bundle"
    find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > "$workspace/SHA256SUMS"
)

cp "$workspace/SHA256SUMS" "$bundle/"

# Publish a complete directory with one rename; failure leaves no successful bundle.
export_stage=$(mktemp -d /out/.export.XXXXXX)
cp -R "$bundle/." "$export_stage/"
chmod 755 "$export_stage"
mv -T "$export_stage" /out/artifacts
export_stage=

printf 'Exported ESP V%s artifacts to /out/artifacts\n' "$board_version"
