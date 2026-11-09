#!/usr/bin/env bash
# Install Compass C++ build dependencies. Run ./script/config.sh from the Compass directory.
#
# If apt update reports Clearsigned / NOSPLIT, check for conflicting package mirrors or proxy requirements.
# Edit /etc/apt/sources.list.d/ to keep one working set of sources, then retry.
set -euo pipefail

sudo apt update

sudo apt install -y \
  build-essential \
  cmake \
  libssl-dev \
  liblapack-dev \
  libblas-dev \
  openssl

echo ""
echo ">>> If CMake reports missing third_party/SEAL or faiss, run ./script/setup_third_party.sh (requires GitHub access or COMPASS_GIT_MIRROR)."
echo ">>> Build: mkdir -p build && cd build && cmake .. && make"
