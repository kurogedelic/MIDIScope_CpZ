# Build image: Debian 13 (trixie) arm64 has alsa-lib 1.2.14 with the UMP API (bookworm's 1.2.8 does not).
FROM --platform=linux/arm64 debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make pkgconf libasound2-dev dpkg-dev file ca-certificates && rm -rf /var/lib/apt/lists/*
