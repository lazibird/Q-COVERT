#!/bin/bash
# Compass-TEE SIFT sgx-hw k sweep: one invocation per (k, ef), sharing the same initial ORAM snapshot.
set -e
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
PY="${QCOVERT_PYTHON:-../.venv/bin/python}"
CSV=results/compass_k_sgx-hw.csv
COMMON="--dataset sift --mode sgx-hw --build-dir build/compass-sift-sgx-hw --ef-spec 4 --ef-n 12 --queries 50 --threads 32 --csv $CSV"

run() { # $1=k $2=ef
  echo "=== $(date '+%F %T') sgx-hw sift k=$1 ef=$2 ==="
  $PY compass/benchmark.py $COMMON --k "$1" --ef "$2"
}

run 5   24
run 10  28
run 20  32
run 50  44
run 100 64

echo "ALL_COMPASS_K_DONE $(date '+%F %T')"
