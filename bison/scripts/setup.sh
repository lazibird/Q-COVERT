#!/bin/bash
set -e

sudo apt update

# ---------------------------------------------------------
# Build tools
# ---------------------------------------------------------
sudo apt install -y build-essential g++ pkg-config
sudo apt install -y nasm

# CMake (>= 3.22.1 required)
sudo snap install cmake --classic

# ---------------------------------------------------------
# C++ library dependencies
# ---------------------------------------------------------
# OpenSSL (used by third_party/openssl evp)
sudo apt install -y libssl-dev openssl

# BLAS / LAPACK (used by DiskANN)
sudo apt install -y liblapack-dev libblas-dev

# Boost (program_options — required by DiskANN + client/oram_initializer)
sudo apt install -y libboost-all-dev

# OpenMP (required by DiskANN, flagged as FATAL if missing)
sudo apt install -y libomp-dev

# AIO (libaio — linked as `aio` in client / oram_initializer / diskann tools)
sudo apt install -y libaio-dev

# io_uring (liburing — required by server and client via pkg-config)
sudo apt install -y liburing-dev

# Intel oneAPI MKL + OpenMP (required by DiskANN; cmake checks /opt/intel/oneapi)
if ! dpkg -s intel-oneapi-mkl-devel &>/dev/null 2>&1; then
    if [[ ! -f /etc/apt/sources.list.d/oneAPI.list ]]; then
        wget -qO- https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB \
            | sudo gpg --dearmor -o /usr/share/keyrings/oneapi-archive-keyring.gpg
        echo "deb [signed-by=/usr/share/keyrings/oneapi-archive-keyring.gpg] https://apt.repos.intel.com/oneapi all main" \
            | sudo tee /etc/apt/sources.list.d/oneAPI.list
        sudo apt update
    fi
    sudo apt install -y intel-oneapi-mkl-devel intel-oneapi-compiler-dpcpp-cpp-and-cpp-classic
fi

# ---------------------------------------------------------
# Python dependencies
# ---------------------------------------------------------
sudo apt install -y python3-pip
pip3 install \
    google-cloud-storage \
    google-cloud-compute \
    paramiko \
    gdown \
    pandas \
    matplotlib \
    rich \
    tcconfig

# ---------------------------------------------------------
# Build the project
# ---------------------------------------------------------
cd "$(dirname "$0")/.."
cmake -S . -B build -DBISON_BUILD_NATIVE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

