# syntax=docker/dockerfile:1
FROM ubuntu:24.04 AS builder
ARG RUST_VERSION=1.98.0
ENV DEBIAN_FRONTEND=noninteractive VCPKG_DISABLE_METRICS=1 VCPKG_MAX_CONCURRENCY=2 \
    CMAKE_BUILD_PARALLEL_LEVEL=2 CARGO_BUILD_JOBS=2 VCPKG_ROOT=/opt/vcpkg \
    PATH=/root/.cargo/bin:/opt/python/bin:$PATH
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates git build-essential g++-14 ninja-build pkg-config curl zip unzip tar \
    python3 python3-venv autoconf autoconf-archive automake libtool \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev \
    libwayland-dev libgl-dev libvulkan-dev && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/python && pip install --no-cache-dir cmake==3.31.6
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/rustup-init.sh && \
    sh /tmp/rustup-init.sh -y --profile minimal --default-toolchain ${RUST_VERSION} && \
    rm /tmp/rustup-init.sh
WORKDIR /source
COPY vcpkg.json ./
COPY scripts/netplay/build.py /opt/build.py
# Use the same dependency revision as native builds, not vcpkg's moving HEAD.
RUN python3 /opt/build.py && \
    /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
COPY CMakeLists.txt CMakePresets.json VERSION ./
COPY cmake cmake
COPY src src
COPY tests tests
COPY tools tools
COPY shaders shaders
COPY data data
RUN cmake --preset linux-ninja-release -B /source/build/probe \
    -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DCMAKE_BUILD_TYPE=Release -DGDL_ENABLE_NETPLAY=ON -DGDL_BUILD_TESTS=ON \
    -DGDL_EMBED_DISC_ICON=OFF -DGDL_ASSET_DIR=/assets -DGDL_DATA_DIR=/opt/probe/data && \
    cmake --build /source/build/probe --target netplayprobe --parallel 2

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 iproute2 util-linux ca-certificates libstdc++6 \
    libx11-6 libxrandr2 libxinerama1 libxcursor1 libxi6 libxkbcommon0 \
    libwayland-client0 libwayland-cursor0 libwayland-egl1 libgl1 libvulkan1 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=builder /source/build/probe/bin/netplayprobe /opt/probe/netplayprobe
COPY data /opt/probe/data
COPY VERSION /opt/probe/VERSION
COPY scripts/netplay_netem.py scripts/netplay/container.py /opt/probe/
WORKDIR /opt/probe
ENTRYPOINT ["python3", "/opt/probe/container.py"]
