# BiSON-TEE

For the CSV-driven experiment entry points, see the repository
[quick start](../../QUICKSTART.md).

Install and compile using [QUICKSTART.md](../../QUICKSTART.md). Commands run from
`Q-COVERT-PS/`. The original BiSON three-path baseline is in `../../bison/`.

## Build a dataset index

```bash
../.venv/bin/python bison/prepare.py --dataset SIFT --degree 128 --build-l 125 \
  --pq-bytes 16 --build-ram-gb 4 --builder ../build/bison/bin/build_disk_index
```

The [DiskANN builder instructions](../../QUICKSTART.md#diskann-builder-for-bison-tee)
cover installation. Preparation builds or
reuses Vamana/PQ files, then imports them into
`../data/prepared/ps/bison/<DATASET>.bin`. SIFT/DEEP1M/MSMARCO1M use float32;
SIFT10M/100M use uint8. `--cache-depth` is fixed at 0. The runner must match the
prepared dimension, degree, PQ bytes and scalar; see the
[TEE layout instructions](../../QUICKSTART.md#native-graph-tee-builds-for-other-datasets).

## Search and plaintext reference

```bash
../.venv/bin/python bison/benchmark.py --dataset SIFT --build-dir ../build/Q-COVERT-PS \
  --iterations 5 7 --beamwidth 8 13 --l-search 125 --queries 100 --reference
# Plaintext reference without constructing ORAM
../.venv/bin/python bison/benchmark.py --dataset SIFT --reference-only \
  --iterations 7 --beamwidth 13 --l-search 125 --queries 100
# SGX simulation
../.venv/bin/python bison/benchmark.py --dataset SIFT --mode sgx-sim \
  --build-dir ../build-sgx-sim/Q-COVERT-PS --iterations 7 --beamwidth 13 --queries 100
```

T/W/L combinations are swept independently of Q-COVERT nprobe/k2. CSVs default
to `results/bison_appendix_f_<dataset>_<mode>.csv`. Initial ORAM images are stored
next to the prepared file and separated by mode.

## Options

| Stage | Options |
| --- | --- |
| Input | `--dataset`, `--config`, `--data-root` (the `data/` directory), `--base`, `--query`, `--groundtruth` as accepted by the stage |
| Formats | `--base-format`, `--query-format`, `--gt-format`, `--base-dtype`, `--query-dtype`, `--scalar` |
| Preparation | `--output`, `--builder`, `--index-prefix`, `--degree`, `--build-l`, `--pq-bytes`, `--build-ram-gb`, `--pq-budget-gb`, `--alpha`, `--rebuild` |
| Search | `--prepared`, `--iterations`, `--beamwidth`, `--l-search` (space-separated lists), `--k` |
| Runner | `--mode native/sgx-sim/sgx-hw`, `--build-dir`, `--enclave` |
| Measurement | `--queries`, `--warmup`, `--repeats`, `--threads`, `--csv`, `--reference`, `--reference-only` |
| ORAM | `--z`, `--s`, `--a`, `--evict-batch`, `--stash` |
| Reuse | `--oram-snapshot`, `--rebuild-oram`, `--no-oram-snapshot`, `--job-isolation`, `--resume` |

`--job-isolation` uses a separate runner invocation per T/W/L combination.
`--resume` skips completed combinations. Use `--help` for the exact accepted
options. Full prepared/index files are scheme-specific and cannot be replaced
with the original BiSON or Compass files.
