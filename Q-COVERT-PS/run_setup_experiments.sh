#!/bin/bash
# Compare setup methods: BulkOR / Sonic sequential (inside the SGX enclave) + plain (native).
# Run SIFT before SIFT10M. Parameters come from config/dataset_defaults.json
# and data/prepared/*/manifest.json (qu_capacity).
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

SGX_HOST=build/qcovert-sgx-hw/apps/sgx/ringdoram_sgx_host
ENCLAVE=build/qcovert-sgx-hw/apps/sgx/ringdoram_enclave.signed.so
NATIVE=build/qcovert-release/ringdoram_setup_benchmark
mkdir -p results

run_dataset () {
  local ds="$1" fu_n="$2" fu_r="$3" qu_n="$4" qu_r="$5"
  echo "===== ${ds}: SGX bulk,sequential ====="
  "$SGX_HOST" "$ENCLAVE" --setup \
    --z 32 --s 58 --a 46 --evict-batch 1 --threads 32 \
    --paths bulk,sequential --csv "results/setup-sonic-hw-${ds}.csv" \
    "${ds}-Fu,${fu_n},128,${fu_r}" "${ds}-Qu,${qu_n},1280,${qu_r}"
  echo "===== ${ds}: native plain Fu ====="
  "$NATIVE" --paths plain \
    --z 32 --s 58 --a 46 --evict-batch 1 --threads 32 \
    --routing-depth "${fu_r}" --block-bytes 128 --label "${ds}-Fu" \
    --repetitions 1 --csv "results/setup-plain-${ds}.csv" "${fu_n}"
  echo "===== ${ds}: native plain Qu ====="
  "$NATIVE" --paths plain \
    --z 32 --s 58 --a 46 --evict-batch 1 --threads 32 \
    --routing-depth "${qu_r}" --block-bytes 1280 --label "${ds}-Qu" \
    --repetitions 1 --csv "results/setup-plain-${ds}.csv" "${qu_n}"
  echo "===== ${ds}: done ====="
}

# SIFT:    Fu N=1,000,000  R=3; Qu N=48,510   R=3
run_dataset SIFT    1000000  3 48510  3
# SIFT10M: Fu N=10,000,000 R=3; Qu N=451,950  R=4
run_dataset SIFT10M 10000000 3 451950 4

echo "===== all setup experiments finished ====="
