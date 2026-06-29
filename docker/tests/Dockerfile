FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    cmake \
    ninja-build \
    python3 \
    python3-pytest \
    python3-numpy \
    python3-h5py \
    python3-yaml \
    python3-serial \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
