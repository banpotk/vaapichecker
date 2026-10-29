FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config curl ca-certificates \
        file patchelf squashfs-tools python3 desktop-file-utils \
        qt6-base-dev qt6-base-dev-tools qt6-wayland libqt6svg6-dev \
        libva-dev fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /source
ENTRYPOINT ["bash", "/source/packaging/appimage/build.sh"]
