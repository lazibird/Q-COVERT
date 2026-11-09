#!/bin/bash
# Compass-TEE sgx-hw ef sweep: one invocation per dataset, sharing one persisted initial ORAM snapshot.
# Parameters come from results/compass_ef_params.csv (1000 plaintext queries); use 100 queries per ef here.
set -e
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
PY="${QCOVERT_PYTHON:-../.venv/bin/python}"
COMMON="--ef-spec 4 --ef-n 12 --k 10 --queries 100 --threads 32 --mode sgx-hw"

run() { # $1=dataset $2=build-dir $3=ef-list $4=extra-options
  echo "=== $(date '+%F %T') sgx-hw $1 ==="
  $PY compass/benchmark.py --dataset "$1" --build-dir "$2" $COMMON --ef $3 $4
}

run sift     build/compass-sift-sgx-hw     "20 24 28 36 40 44 48 52 60 64 68 72 80 84 88"
run deep1m   build/compass-deep1m-sgx-hw   "20 28 32 40 48 52 60 68 72 80 84 92 100 104 112"
run msmarco1m build/compass-msmarco-sgx-hw "132 168 204 240 280 316 352 388 424 460 496 536 668 812 956"
run sift10m  build/qcovert-sgx-hw          "28 32 36 40 44 48 52 56 60 64 68 72 76 80 84" \
  --enclave build/qcovert-sgx-hw/apps/sgx/ringdoram_enclave_h196608m.signed.so
echo "ALL_SGX_HW_DONE $(date '+%F %T')"
