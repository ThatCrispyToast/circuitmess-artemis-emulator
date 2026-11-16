FROM docker.io/library/ubuntu:22.04@sha256:45e9ddc6895604674618a409ca9f73d301da8fdb75fe330e8e9a27b3b219e4ad

ENV DEBIAN_FRONTEND=noninteractive
RUN sed -n 's/^deb /deb-src /p' /etc/apt/sources.list > /etc/apt/sources.list.d/artemis-sources.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
       ca-certificates git python3-venv python3-tomli build-essential ninja-build \
       pkg-config curl patch patchelf dpkg-dev xz-utils \
       libglib2.0-dev libgcrypt20-dev libpixman-1-dev libslirp-dev zlib1g-dev \
       libegl1 libgl1 libglib2.0-0 libfontconfig1 libxkbcommon-x11-0 libxcb-cursor0 \
       libxcb-icccm4 libxcb-keysyms1 libxcb-shape0 libwayland-cursor0 libpulse0 \
       fonts-dejavu-core xvfb xauth

WORKDIR /work
