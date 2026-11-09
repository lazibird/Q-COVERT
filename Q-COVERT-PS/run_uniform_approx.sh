#!/bin/bash
# Uniform-quota approximate selection: each probed cluster uses ceil(alpha*k2/nprobe).
# Compare with quotas calibrated on simulated queries to measure calibration benefits.
# Read nprobe/k2/alpha row by row from the existing calibrated approximate-result CSV.
# Use 1000 plaintext queries for recall and 20 sgx-hw queries for timing. Run SIFT before SIFT10M.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
PY="${QCOVERT_PYTHON:-../.venv/bin/python}"
[ -x "$PY" ] || PY=python3

run_ds() {
  local ds="$1"
  local ref="results/qcovert_${ds}_sgx-hw_approx.csv"
  local nprobes k2s alpha
  nprobes=$(tail -n +2 "$ref" | cut -d, -f1 | tr '\n' ' ')
  k2s=$(tail -n +2 "$ref" | cut -d, -f2 | tr '\n' ' ')
  alpha=$(tail -n +2 "$ref" | cut -d, -f3 | sort -u | tr '\n' ' ')
  alpha=$(echo $alpha)  # Normalize whitespace; require one unique value.
  [ "$(echo "$alpha" | wc -w)" = 1 ] || { echo "alpha not unique in $ref: $alpha"; exit 1; }
  echo "===== ${ds}: plaintext uniform quotas, 1000 queries ====="
  echo "nprobe=[${nprobes}] k2=[${k2s}] alpha=${alpha}"
  OMP_NUM_THREADS=32 "$PY" tools/plaintext_recall.py --dataset "${ds}" \
    --candidate-selection approx --uniform-quotas --alpha "${alpha}" \
    --nprobe ${nprobes} --k2 ${k2s} --queries 1000 \
    --csv "results/plaintext_${ds}_approx_uniform.csv"
  echo "===== ${ds}: sgx-hw uniform quotas, 20 queries ====="
  "$PY" tools/benchmark.py --mode sgx-hw --sgx-build-dir build/qcovert-sgx-hw \
    --dataset "${ds}" --candidate-selection approx --uniform-quotas --alpha "${alpha}" \
    --nprobe ${nprobes} --k2 ${k2s} --queries 20 \
    --csv results/qcovert-uniform.csv
  echo "===== ${ds}: done ====="
}

run_ds SIFT
run_ds SIFT10M
echo "===== all uniform-quota runs finished ====="
