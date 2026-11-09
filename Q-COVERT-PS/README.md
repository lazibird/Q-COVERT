# Q-COVERT-PS

For the CSV-driven experiment entry points, see the repository
[quick start](../QUICKSTART.md).

Install and build using the [repository README](../README.md). Commands below
run from this directory with the shared Python environment and root build.
Compass-TEE and BiSON-TEE have separate run guides in their own directories.

## Prepare and search

```bash
# Prepare reusable clustering and RaBitQ artifacts
../.venv/bin/python tools/benchmark.py --dataset SIFT --build-dir ../build/Q-COVERT-PS --prepare-only
# Exact candidate selection
../.venv/bin/python tools/benchmark.py --dataset SIFT --build-dir ../build/Q-COVERT-PS \
  --nprobe 20 50 100 --k2 60 150 250 --k 10 --queries 100 --threads 8
# Approximate selection with automatically calibrated quotas
../.venv/bin/python tools/benchmark.py --dataset SIFT --build-dir ../build/Q-COVERT-PS \
  --candidate-selection approx --alpha 2 --nprobe 100 --k2 250 --queries 100
```

Datasets: `SIFT`, `SIFT10M`, `SIFT100M`, `DEEP1M`, `MSMARCO1M` (case insensitive).
Defaults come from `config/dataset_defaults.json`. PS `--data-root` is the parent
of `data/`, unlike the Compass/BiSON-TEE options, which take `data/` itself.
Raw paths do not depend on the launch directory. Offline artifacts and initial
ORAM images default to `../data/prepared/ps/qcovert/`; CSV files default to
`results/`. A sweep initializes/restores once and appends a summary per pair.

## SGX

After building the SIM or HW targets using the root README:

```bash
../.venv/bin/python tools/benchmark.py --dataset SIFT --mode sgx-sim \
  --build-dir ../build/Q-COVERT-PS --sgx-build-dir ../build-sgx-sim/Q-COVERT-PS \
  --nprobe 100 --k2 250 --queries 100
```

For hardware execution, use `--mode sgx-hw` and the HW build directory. SIM
validates functionality without providing enclave protection. Heap size and
vector/code dimensions are build settings documented in the root README.

## Options

| Purpose | Options |
| --- | --- |
| Inputs | `--dataset`, `--config`, `--data-root`, `--cache-dir` |
| Programs | `--build-dir`, `--sgx-build-dir`, `--mode native/sgx-sim/sgx-hw` |
| Search sweep | `--nprobe` and `--k2` take space-separated lists of matching lengths; `--k` sets final top-k |
| Candidate selection | `--candidate-selection exact/approx`, `--alpha`, `--calibration-queries`, `--use-config-quotas`, `--uniform-quotas` |
| Measurement | `--queries`, `--warmup`, `--repeats`, `--threads`, `--csv` |
| QuORAM forest | `--qu-routing-depth`, `--qu-evict-batch` |
| FuORAM forest | `--fu-routing-depth`, `--fu-evict-batch` |
| Reuse | `--prepare-only`, `--rebuild-oram`, `--resume` |

`--use-config-quotas` reuses configured rank fractions; `--uniform-quotas` selects
uniform approximate allocation. Use these instead of automatic calibration when
an experiment requires fixed quotas. `--rebuild-oram` replaces the saved initial
image; `--resume` skips parameter pairs already present in the result CSV.

For a plaintext recall reference using the same prepared index:

```bash
../.venv/bin/python tools/plaintext_recall.py --help
```
