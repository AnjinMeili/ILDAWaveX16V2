#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

[[ "$#" == 0 ]] || fail 'Usage: build-firmware (no arguments)'
[[ -d /src/.git || -f /src/.git ]] || fail '/src must be a Git checkout.'
[[ -d /out && -w /out ]] || fail 'Mount an empty, writable directory at /out.'
[[ -z "$(find /out -mindepth 1 -maxdepth 1 -print -quit)" ]] || fail '/out must be empty.'

# Stable paths preserve compiler-cache correctness across disposable containers.
workspace=/tmp/ildawave-rp2354
mkdir "$workspace"
export_stage=

cleanup() {
    rm -rf -- "$workspace"

    if [[ -n "$export_stage" ]]; then
        rm -rf -- "$export_stage"
    fi
}

trap cleanup EXIT

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

git_source ls-files -z -- \
    firmware/RP2354/CMakeLists.txt \
    firmware/RP2354/pico_sdk_import.cmake \
    firmware/RP2354/src \
    firmware/RP2354/include \
    firmware/RP2354/pio > "$workspace/inputs"

[[ -s "$workspace/inputs" ]] || fail 'No RP sources found.'

tar -C /src --null -T "$workspace/inputs" -cf - \
    | tar -C "$workspace/project" --strip-components=2 -xf -

cd "$workspace/project"
find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > "$workspace/input-sha256.txt"
input_digest=$(sha256sum "$workspace/input-sha256.txt" | cut -d ' ' -f 1)

export CCACHE_BASEDIR="$workspace/project"

if [[ -d /cache ]]; then
    [[ -w /cache ]] || fail '/cache must be writable by the container user.'
    export CCACHE_DIR=/cache/rp2350-arm-s-pico2-sdk2.2.0-gcc14.2-release
else
    export CCACHE_DIR="$workspace/ccache"
fi

cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DPICO_BOARD=pico2 \
    -DPICO_PLATFORM=rp2350-arm-s \
    -DENABLE_HELIOS_USB=OFF \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON

cmake --build build --parallel 4

bundle="$workspace/artifacts"
mkdir -p "$bundle"

for extension in uf2 elf bin elf.map; do
    file="ILDAWaveX16V2-RP2350.$extension"
    [[ -s "build/$file" ]] || fail "Missing artifact: $file"
    cp "build/$file" "$bundle/"
done

read -r flash_origin flash_size < <(awk '$1 == "FLASH" && $2 ~ /^0x/ {print $2, $3; exit}' build/ILDAWaveX16V2-RP2350.elf.map) \
    || fail 'Cannot read linked FLASH bounds from the map file.'
node /usr/local/lib/inspect-uf2.cjs "$bundle/ILDAWaveX16V2-RP2350.uf2" "$flash_origin" "$flash_size" > "$bundle/uf2-info.json"
picotool info -a "$bundle/ILDAWaveX16V2-RP2350.uf2" > "$bundle/picotool-info.txt"

cp build/CMakeCache.txt "$bundle/"
cp "$workspace/input-sha256.txt" "$bundle/"

{
    printf 'source_commit=%s\nsource_state=%s\ninput_digest=%s\n' "$source_commit" "$source_state" "$input_digest"
    printf 'board=pico2\nplatform=rp2350-arm-s\nconfiguration=Release\nhelios=OFF\n'
    printf 'sdk_commit=a1438dff1d38bd9c65dbd693f0e5db4b9ae91779\n'
    printf 'picotool_commit=25aa087b2c517b4901874a99536e869d4d27b678\n'
    arm-none-eabi-gcc --version
    picotool version
    cmake --version
    ninja --version
    sha256sum /usr/local/bin/build-firmware /usr/local/lib/inspect-uf2.cjs
} > "$bundle/build-info.txt"

(
    cd "$bundle"
    find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > "$workspace/SHA256SUMS"
)

cp "$workspace/SHA256SUMS" "$bundle/"
export_stage=$(mktemp -d /out/.export.XXXXXX)
cp -R "$bundle/." "$export_stage/"
chmod 755 "$export_stage"
mv -T "$export_stage" /out/artifacts
export_stage=

printf 'Exported RP artifacts to /out/artifacts\n'
