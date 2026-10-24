# RP2354 Docker builds

Build the RP firmware in a Linux amd64 container. The image uses Pico SDK 2.2.0, Arm GNU Toolchain 14.2.Rel1, and picotool 2.2.0-a4, matching the recorded project versions. SDK/picotool commits, the Ubuntu base digest, and the compiler archive checksum are pinned in `docker/rp2354/Dockerfile`. Ubuntu package versions remain mutable at image creation time.

This builder preserves the existing `pico2` board selection, Arm secure target, Release configuration, and disabled Helios plugin. Flash size comes from the firmware's CMake configuration; the builder does not override it or reserve settings space.

To install the generated UF2, follow the [RP2354 browser and mounted-drive flashing guide](flashing-rp2354.md).

## Build the image

For Bash, the host launchers provide the short path:

```bash
./tools/build-images.sh rp2354   # Image only; omit the target to build both images.
./tools/build-firmware.sh rp2354 # Build the image and firmware, then export artifacts.
```

The firmware command publishes `dist/rp2354/artifacts/`, replacing the previous bundle only after a successful build. Failed builds preserve the previous bundle. It uses the invoking UID/GID and locates the repository from the script path. `docker/rp2354/build.sh` is the internal container entrypoint, not a host command. Linux is the validated Bash host; native Windows commands are below.

Use `./tools/clean-build.sh rp2354` to remove RP artifacts explicitly, or `./tools/clean-build.sh all` for all three firmware variants. Cleanup preserves compiler caches, Docker images, and unrelated files. See [cleanup and lock recovery](building.md#bash-quick-start) for details.

Run from the repository root:

```sh
docker build --platform linux/amd64 -f docker/rp2354/Dockerfile -t ildawave-rp2354-builder:local .
```

Toolchains remain inside Docker. Record the actual local image identity alongside build records:

```sh
docker image inspect ildawave-rp2354-builder:local --format '{{.Id}}'
```

## Linux build

```sh
mkdir -p dist/rp2354

docker run --rm --platform linux/amd64 \
    --user "$(id -u):$(id -g)" \
    --mount "type=bind,source=$PWD,target=/src,readonly" \
    --mount "type=bind,source=$PWD/dist/rp2354,target=/out" \
    ildawave-rp2354-builder:local
```

These manual Docker commands require an empty output directory; the Bash launcher above handles replacement automatically. Each successful run publishes `dist/rp2354/artifacts/`. The image accepts no build arguments; board/platform/plugin choices are fixed.

## Windows PowerShell 5.1

Target host: Windows 10 x64 with a working Docker Desktop Linux-container backend. These commands remain untested on Windows. Build the image with the one-line command above, then:

```powershell
New-Item -ItemType Directory -Force -Path dist/rp2354 | Out-Null

$sourcePath = (Get-Location).Path
$outputPath = (Resolve-Path dist/rp2354).Path

docker run --rm --platform linux/amd64 `
    --mount "type=bind,source=$sourcePath,target=/src,readonly" `
    --mount "type=bind,source=$outputPath,target=/out" `
    ildawave-rp2354-builder:local

if ($LASTEXITCODE -ne 0) {
    throw 'RP firmware build failed.'
}
```

The default container user is UID/GID 10001. Linux can override it with the host UID/GID as shown above. No USB access or privileged mode is required. ARM hosts and emulation are not validated.

## Inputs, caching, and failures

The builder copies tracked RP CMake files, `src`, `include`, and `pio` into a fresh container workspace. It includes modified tracked files but excludes untracked files, editor configuration, and host build products. Add new source files to Git before building them. The repository is mounted read-only; Git worktrees with metadata outside that mount are not supported by these commands.

SDK dependencies are installed while creating the image. Each firmware run uses a fresh CMake build directory. Optional compiler caching is enabled by mounting a writable directory at `/cache`; the cache namespace includes the target, board, SDK/compiler versions, and Release configuration. Without this mount, cache data is disposable.

For Linux, create `.build-cache/rp2354` and add:

```sh
--mount "type=bind,source=$PWD/.build-cache/rp2354,target=/cache"
```

Use the same UID/GID for repeated cached builds. On Windows resolve that directory to an absolute path with `Resolve-Path`. Both `.build-cache/` and `dist/` are gitignored. Delete only this project's cache directory when clearing it; no global Docker prune is required.

Compilation/export validation failure returns nonzero without publishing an artifact directory. Export is staged and renamed only after success. Normal failure removes the builder's own staging directory; forced termination may leave `.export.*` under the output mount. Inspect and remove that incomplete directory before retrying. Do not run concurrent builds into one output directory. Existing output is never erased automatically.

## Exported files

- `ILDAWaveX16V2-RP2350.uf2`, `.bin`, `.elf`, and `.elf.map` retain the existing CMake project name.
- `CMakeCache.txt` records generated build configuration.
- `uf2-info.json` reports linked flash bounds, application family/address range, and separate RP2350-E10 compatibility blocks.
- `picotool-info.txt` records picotool's inspection of the UF2 file.
- `input-sha256.txt`, `build-info.txt`, and `SHA256SUMS` identify inputs, tool versions, and exported contents.

The UF2 inspector verifies block structure, application family, numbering, order, and application ranges against the linker's FLASH region. Compatibility blocks are reported separately rather than mistaken for application code. These checks do not validate the physical device size, analog output, or settings-sector reservation. No browser package, flashing command, or runtime fix is included.

Verify the export on Linux:

```sh
cd dist/rp2354/artifacts
sha256sum --check SHA256SUMS
```
