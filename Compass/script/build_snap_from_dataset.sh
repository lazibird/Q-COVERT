#!/usr/bin/env bash
# Generate Compass client/server snapshots from an existing HNSW index and base vectors (buckets, position map, graph cache).
# Usage (from the Compass directory):
#   ./script/build_snap_from_dataset.sh [dataset, default: sift]
#
# Prerequisites, with paths from the dataset entry in tests/config/config_ring.json:
#   - index_path: a Faiss-readable IndexHNSW graph (this repository implements read_index only).
#   - base_path: base.fvecs or BigANN uint8 base.bin, matching the index count and dimensions.
#   - pq_path: a compatible PQ file is required by test_compass_ring and is not generated here.
#
# compass_init lays out graph and vector data in Ring ORAM and writes encrypted snapshots; it does not train HNSW/PQ.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${COMPASS_BUILD_DIR:-${REPO_ROOT}/build}"
DATASET="${1:-sift}"

if [[ ! -d "$BUILD_DIR" ]]; then
  echo "Error: build directory not found; run: mkdir build && cd build && cmake .. && make"
  exit 1
fi

if [[ ! -x "$BUILD_DIR/compass_init" ]]; then
  echo "Error: $BUILD_DIR/compass_init not found; run cmake .. && make in the build directory."
  exit 1
fi

echo ">>> Running in $BUILD_DIR: ./compass_init d=$DATASET"
echo ">>> Ensure index_path / base_path for \"$DATASET\" in config_ring.json point to your data."
cd "$BUILD_DIR"
exec ./compass_init "d=$DATASET"
