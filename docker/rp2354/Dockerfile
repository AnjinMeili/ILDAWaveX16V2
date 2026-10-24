# Linux amd64: the Arm compiler archive below is architecture-specific.
FROM ubuntu:24.04@sha256:4fbb8e6a8395de5a7550b33509421a2bafbc0aab6c06ba2cef9ebffbc7092d90

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential \
        ca-certificates \
        ccache \
        cmake \
        curl \
        git \
        libusb-1.0-0-dev \
        ninja-build \
        nodejs \
        pkg-config \
        python3 \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Validate the compiler download before extracting anything into the image.
RUN <<'INSTALL_COMPILER'
set -eu

curl --fail --location --retry 3 \
    https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi.tar.xz \
    --output /tmp/arm.tar.xz

printf '%s  %s\n' \
    62a63b981fe391a9cbad7ef51b17e49aeaa3e7b0d029b36ca1e9c3b2a9b78823 \
    /tmp/arm.tar.xz | sha256sum --check -

mkdir /opt/arm
tar -xJf /tmp/arm.tar.xz --strip-components=1 -C /opt/arm
rm /tmp/arm.tar.xz
INSTALL_COMPILER

ENV PATH="/opt/arm/bin:/opt/picotool/bin:${PATH}"
ENV PICO_SDK_PATH=/opt/pico-sdk
ENV CMAKE_PREFIX_PATH=/opt/picotool

RUN <<'INSTALL_SDK'
set -eu

git clone --branch 2.2.0 --depth 1 https://github.com/raspberrypi/pico-sdk.git /opt/pico-sdk
test "$(git -C /opt/pico-sdk rev-parse HEAD)" = a1438dff1d38bd9c65dbd693f0e5db4b9ae91779

git -C /opt/pico-sdk submodule update --init --depth 1
INSTALL_SDK

# Match the version already selected by the project's editor configuration.
RUN <<'INSTALL_PICOTOOL'
set -eu

git clone --branch 2.2.0-a4 --depth 1 https://github.com/raspberrypi/picotool.git /opt/picotool-src
test "$(git -C /opt/picotool-src rev-parse HEAD)" = 25aa087b2c517b4901874a99536e869d4d27b678

cmake -S /opt/picotool-src -B /opt/picotool-build -G Ninja \
    -DPICO_SDK_PATH=/opt/pico-sdk \
    -DCMAKE_INSTALL_PREFIX=/opt/picotool

cmake --build /opt/picotool-build --parallel 4
cmake --install /opt/picotool-build
INSTALL_PICOTOOL

RUN groupadd --gid 10001 builder \
    && useradd --uid 10001 --gid builder --create-home builder

COPY --chmod=755 docker/rp2354/build.sh /usr/local/bin/build-firmware
COPY docker/rp2354/inspect-uf2.cjs /usr/local/lib/inspect-uf2.cjs

USER builder
WORKDIR /tmp

ENTRYPOINT ["/usr/local/bin/build-firmware"]
