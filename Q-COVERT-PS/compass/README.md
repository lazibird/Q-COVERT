# Compass-TEE

For the CSV-driven experiment entry points, see the repository
[quick start](../../QUICKSTART.md).

Install and compile using [QUICKSTART.md](../../QUICKSTART.md). Commands below run
from `Q-COVERT-PS/`. This is the TEE adaptation; the original Compass and its
low-client-storage path are in `../../Compass/`.

## Prepare

```bash
../.venv/bin/python compass/prepare.py --dataset sift
# Import compatible existing HNSW and IndexPQ files instead of retraining
../.venv/bin/python compass/prepare.py --dataset sift --hnsw /path/to/hnsw.index --pq /path/to/pq.index
```

Raw files follow `config/dataset_defaults.json`. Prepared files default to
`../data/prepared/ps/compass/<DATASET>.full-c.bin`. SIFT/DEEP1M/MSMARCO1M use
float32 stored vectors; SIFT10M/100M use uint8. The binary layout must match the
prepared dimension, degree, PQ subspaces/centroids and scalar; build flags are
listed in the [TEE layout instructions](../../QUICKSTART.md#native-graph-tee-builds-for-other-datasets).

## Search

```bash
../.venv/bin/python compass/benchmark.py --dataset sift --build-dir ../build/Q-COVERT-PS \
  --ef 20 40 60 --ef-spec 4 --ef-n 12 --k 10 --queries 100 --reference
# SGX simulation
../.venv/bin/python compass/benchmark.py --dataset sift --mode sgx-sim \
  --build-dir ../build-sgx-sim/Q-COVERT-PS --ef 40 --queries 100
```

A sweep reuses one two-tree initialization or initial snapshot. CSV output
defaults to `results/compass_full_c_<dataset>_<mode>.csv`. Initial snapshots are
stored next to the prepared file and separated by execution mode.

## Options

Preparation and search default to 32 worker threads; `--threads` overrides this.

| Stage | Options |
| --- | --- |
| Shared input | `--dataset`, `--config`, `--data-root` (the `data/` directory), `--base`, `--query`, `--groundtruth` as accepted by the stage |
| Formats | `--base-format`, `--query-format`, `--gt-format`; `--base-dtype`, `--query-dtype` for header-bin payloads |
| Preparation | `--output`, `--scalar`, `--metric l2/ip`, `--hnsw`, `--pq`, `--convert-prepared` |
| Training | `--hnsw-m`, `--ef-construction`, `--pq-subspaces`, `--pq-bits`, `--pq-iterations`, `--training-count`, `--threads`, `--seed`, `--batch-size`, `--limit` |
| Search | `--prepared`, `--ef` (space-separated sweep), `--ef-spec`, `--ef-n`, `--k` |
| Runner | `--mode native/sgx-sim/sgx-hw`, `--build-dir`, `--enclave` |
| Measurement | `--queries`, `--warmup`, `--repeats`, `--threads`, `--csv`, `--reference` |
| ORAM | `--z`, `--s`, `--a`, `--evict-batch`, `--stash` |
| Snapshots | `--oram-snapshot`, `--rebuild-oram`, `--no-oram-snapshot` |

`--reference` compares outputs with the plaintext oracle; it still runs the
ORAM path. `--upper-steps` is retained for compatibility and only accepts 1.
Use `--help` on `prepare.py` and `benchmark.py` for stage-specific options.
