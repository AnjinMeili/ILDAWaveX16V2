# ESP32 Docker builds

The ESP builder compiles V1 and V2 firmware without installing ESP-IDF on the host. Run commands from the repository root. See [RP2354 Docker builds](building-rp2354.md) for the separate RP image and its validation limits.

## Bash quick start

```bash
./tools/build-images.sh                    # Build both builder images only.
./tools/build-firmware.sh esp32            # Build the ESP image and V2 firmware.
./tools/build-firmware.sh esp32 --board-version 1
```

The firmware launcher builds the matching image using Docker's layer cache, then exports to `dist/esp32-v2/artifacts/` or `dist/esp32-v1/artifacts/`. Rerun the same command to rebuild: previous artifacts are replaced only after success and retained if the build fails. These Bash launchers use the invoking UID/GID and work independently of the current directory. Linux is the validated host; native Windows users should use the PowerShell commands below.

To remove generated artifacts without rebuilding:

```bash
./tools/clean-build.sh esp32-v2
./tools/clean-build.sh all
```

Cleanup accepts `esp32-v1`, `esp32-v2`, `rp2354`, or `all`. It removes only their `artifacts/` directories, preserving compiler caches, Docker images, and other files. Build and cleanup commands lock each target to prevent concurrent replacement. A forcibly killed process can leave `dist/<target>/.build-lock`: confirm no build is running before removing it, and recover its `previous/` bundle first if the normal artifacts directory is missing.

Use `./tools/build-images.sh esp32` to build only the ESP image. Files under `docker/*/build.sh` are internal container entrypoints, not host launchers. Compiler-cache mounts remain available through the manual commands below.

## Image

Requires Docker with Linux containers on an x64 host. The base is the official ESP-IDF v6.0 `linux/amd64` image pinned by manifest digest in `docker/esp32/Dockerfile`. Its SDK/compiler are fixed by that image; the added Ubuntu `jq` package is resolved from the package repository at image build time. This is not a claim of bit-for-bit reproducibility.

```sh
docker build --platform linux/amd64 -f docker/esp32/Dockerfile -t ildawave-esp32-builder:local .
```

Record the local image identity alongside your build records:

```sh
docker image inspect ildawave-esp32-builder:local --format '{{.Id}}'
```

The mutable tag is a convenience, not an immutable identity. SDKs and compilers stay inside Docker. Network access is required to create the image and retrieve ESP components.

## Linux

For these manual Docker commands, create an empty output directory per build. The Bash launcher above handles replacement automatically. The container runs with your UID/GID so its exported files belong to you.

```sh
mkdir -p dist/esp32-v2

docker run --rm --platform linux/amd64 \
    --user "$(id -u):$(id -g)" \
    --mount "type=bind,source=$PWD,target=/src,readonly" \
    --mount "type=bind,source=$PWD/dist/esp32-v2,target=/out" \
    ildawave-esp32-builder:local --board-version 2
```

For V1:

```sh
mkdir -p dist/esp32-v1

docker run --rm --platform linux/amd64 \
    --user "$(id -u):$(id -g)" \
    --mount "type=bind,source=$PWD,target=/src,readonly" \
    --mount "type=bind,source=$PWD/dist/esp32-v1,target=/out" \
    ildawave-esp32-builder:local --board-version 1
```

## Windows PowerShell 5.1

These commands target Windows 10 x64 with a working Docker Desktop Linux-container backend. They have not been executed on a Windows host. The image's default user is UID/GID 10001; Docker Desktop must allow it to write the output bind mount.

Build the image with the same one-line `docker build` command above, then:

```powershell
New-Item -ItemType Directory -Force -Path dist/esp32-v2 | Out-Null

$sourcePath = (Get-Location).Path
$outputPath = (Resolve-Path dist/esp32-v2).Path

docker run --rm --platform linux/amd64 `
    --mount "type=bind,source=$sourcePath,target=/src,readonly" `
    --mount "type=bind,source=$outputPath,target=/out" `
    ildawave-esp32-builder:local --board-version 2

if ($LASTEXITCODE -ne 0) {
    throw 'ESP V2 build failed.'
}
```

For V1:

```powershell
New-Item -ItemType Directory -Force -Path dist/esp32-v1 | Out-Null

$sourcePath = (Get-Location).Path
$outputPath = (Resolve-Path dist/esp32-v1).Path

docker run --rm --platform linux/amd64 `
    --mount "type=bind,source=$sourcePath,target=/src,readonly" `
    --mount "type=bind,source=$outputPath,target=/out" `
    ildawave-esp32-builder:local --board-version 1

if ($LASTEXITCODE -ne 0) {
    throw 'ESP V1 build failed.'
}
```

Do not reuse a nonempty output directory. ARM hosts and emulation are not validated.

## Inputs and cache

`/src` must be an accessible Git checkout. Worktrees whose Git metadata lives outside the mount are not supported by these commands. The builder copies tracked files from the ESP `main` directory, CMake project, defaults, and dependency lockfile into a fresh workspace. Modified tracked files are included; untracked files are not. Add new firmware inputs to Git before building them. Generated host configuration, managed components, and build directories are excluded.

ESP32-S3 is selected explicitly. Existing `sdkconfig.defaults` and board definitions remain authoritative. Dependency resolution must leave `dependencies.lock` byte-for-byte unchanged or export fails.

The image layers cache toolchain installation. Each run starts with a fresh CMake build directory. Optional compiler caching uses a writable directory mounted at `/cache`, separated by ESP target, pinned toolchain version, board variant, and defaults digest. Downloaded components are not persisted in this first slice.

For Linux, create `.build-cache/esp32` and add this argument to `docker run`:

```sh
--mount "type=bind,source=$PWD/.build-cache/esp32,target=/cache"
```

Run cached builds under the same UID/GID. On Windows, resolve the cache directory with `Resolve-Path` and pass its absolute path the same way. Without this mount, the compiler cache is disposable. Removing this project's cache directory only removes cached compiler results; no global Docker prune is needed.

## Artifacts and failure behavior

Successful output is published as `/out/artifacts` in a single directory rename. It contains:

- Application ELF, map, and binary; bootloader and partition binaries at their generated relative paths.
- Generated `flasher_args.json` and `flash_args`, preserving build-selected offsets.
- Effective `sdkconfig`, dependency lockfile, and input-file checksum inventory.
- `build-info.txt`: source commit/dirty status, input digest, board, target, base digest, tool versions, and builder-script checksum.
- `SHA256SUMS` covering all other exported files.

The builder verifies referenced flash binaries are nonempty, nonoverlapping, and within the configured flash size. This verifies packaging against the build configuration, not the physical board or settings-preservation behavior. Flashing, browser manifests, merged factory images, and release publication are outside this slice.

Compilation or validation failure returns a nonzero status without publishing `artifacts`. The script cleans its own temporary workspace/staging directory on normal exit or error; forced termination may leave an `.export.*` directory in the output mount. Inspect and remove that incomplete directory before retrying. Existing output is never automatically erased. Do not run concurrent exports to the same directory.

On Linux, verify an export with:

```sh
cd dist/esp32-v2/artifacts
sha256sum --check SHA256SUMS
```

## Earlier builder validation

The earlier builder implementation was validated on Linux x64 using image ID `sha256:0713b519650a44da5b3d43fe7ce0f9a418fe92d8963ec20e4e3995dcaad29735`. This record predates the independent upstream branches:

- V1 and V2 built and exported successfully with unchanged dependency lockfiles and read-only source mounts.
- Exported checksums passed; flash files use generated offsets `0x0`, `0x8000`, and `0x10000` within the configured 4 MB image layout.
- Default UID/GID 10001 and arbitrary UID/GID 12345 produced correctly owned, readable exports. ESP-IDF logs a harmless username lookup warning for a UID without a passwd entry.
- A repeat V2 build reused 810 of 812 cacheable compilations (99.75%). A fresh build also succeeded without a mounted cache.
- An output path containing spaces worked. Invalid board selection and nonempty output were rejected. An intentionally invalid source mounted only inside a test container returned failure and left output empty.
- Bash syntax, ShellCheck, and whitespace checks passed. Firmware sources and configuration defaults were not changed.

These builds used a dirty development checkout; exported input digests and builder-script checksums identify their actual inputs. Windows host behavior and hardware flashing are not tested here.

The pinned image includes ccache 4.9.1, which does not resolve GCC's implicit `picolibc.specs` search path. The builder links that exact toolchain file into the disposable application and bootloader build directories so ccache can hash it. A fixed container-local workspace keeps debug paths stable between builds. Compiler flags, specs contents, and cache correctness checks remain unchanged.

## Independent branch validation (2026-10-24)

The builders were reintroduced directly on `origin/main` (`245c7b6`), without firmware formatting or source changes. Both root commands, `./tools/build-firmware.sh esp32` and `./tools/build-firmware.sh esp32 --board-version 1`, successfully built their image, compiled the original sources, and exported artifacts. Exported checksums passed for both variants. Bash syntax, ShellCheck, and whitespace checks passed. These runs used a checkout with the new builder files staged; no Windows or hardware validation was performed.
