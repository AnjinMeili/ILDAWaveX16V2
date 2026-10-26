# ESP-IDF v6.0, linux/amd64 manifest. The SDK and compiler come from this image.
FROM espressif/idf:v6.0@sha256:c41bed8b8b28815526b70eac307452fb432c7904a9157fbaaa04784ac4840ad7

USER root

RUN apt-get update \
    && apt-get install --yes --no-install-recommends jq \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --gid 10001 builder \
    && useradd --uid 10001 --gid builder --create-home builder

COPY --chmod=755 docker/esp32/build.sh /usr/local/bin/build-firmware

USER builder
WORKDIR /tmp

ENTRYPOINT ["/usr/local/bin/build-firmware"]
