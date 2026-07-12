FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-arm-none-eabi \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib \
    g++ \
    cmake \
    ninja-build \
    openocd \
    python3 \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --break-system-packages --no-cache-dir clang-tidy==19.1.0.1

WORKDIR /workspace
