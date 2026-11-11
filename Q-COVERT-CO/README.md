# Q-COVERT-CO and IVF-RaBitQ baselines

For the CSV-driven experiment entry points, see the repository
[quick start](../QUICKSTART.md).

Install and build using [QUICKSTART.md](../QUICKSTART.md). Run the following
commands from this directory using the shared environment and root build.

## Prepare and build an index

```bash
../.venv/bin/python ivf-rabitq/preprocess.py all --dataset sift
../build/Q-COVERT-CO/build_ivf_rabitq_index --dataset sift
```

The stages `ivf` and `rabitq` may be run separately. Raw inputs follow the shared
PS profiles. `config.json` selects the dataset, raw filenames and experiment
settings; relative `source` and `train` paths are resolved against that file.
Explicit CLI paths are relative to the launch directory. The `files` entries
are relative to `source`. Generated CO indexes remain in their dataset directory.

| Dataset | Index/search executable suffix | Vector dimension / code bits |
| --- | --- | --- |
| sift, siftsmall, sift10M, sift100M, sift1B | none | 128 / 128 |
| deep1M | `_d96` | 96 / 128 |
| msmarco1m | `_d768` | 768 / 768 |

For example, build DEEP1M with `build_ivf_rabitq_index_d96`. Use the same suffix
for the CO and local search executables. The encrypted baseline selects its
vector dimension at runtime. Reranking uses original coordinates and exact squared L2 distance.

FuORAM stores SIFT coordinates as lossless `uint8`, and DEEP1M/MSMARCO1M coordinates
as `float32`. `--exact-payload raw-float32` can retain float32 storage explicitly.
Raw uint8 conversion rejects fractional or out-of-range values. New raw payload
cache names keep previous quantized caches separate.

## Q-COVERT-CO

```bash
../build/Q-COVERT-CO/test_ivf_rabitq_double_oram \
  --dataset sift --nprobes 20,50,100 --k2 60,150,250 --nqueries 100
```

`--nprobes` and `--k2` are comma-separated lists with one reranking budget per
probe count. A sweep reuses one ORAM initialization. Results are written under
`results/` relative to the launch directory.
QuORAM keeps each cluster's actual block count. Queries add dummy paths so that
each query accesses exactly `nprobe * nb` paths, where `nb = ceil(tau / 32)`.
`tau` follows the preprocessing `size_max` setting; for a custom index built
with a command-line `--size-max`, pass the same value as `--tau` when querying.
When `size_max` is zero, the same default capacity formula as preprocessing is
used automatically. Both ORAM layers default to `(Z, S, A) = (32, 58, 46)`.

## Baselines

```bash
# Local IVF-RaBitQ
../build/Q-COVERT-CO/search_local --dataset sift --nprobes 100 --k2 250 --nqueries 100
# Encrypted baseline
../build/Q-COVERT-CO/test_ivf_rabitq_encrypted_baseline \
  --dataset sift --nprobes 100 --k2 250 --nqueries 100 --path-batch-clusters 100
```

## Options

| Stage | Options |
| --- | --- |
| Input/config | `--dataset`, `--source`, `--config`, `--no-config`; index builder: `--clusters`, `--output` |
| Preprocessing | `--clusters`, `--train`, `--train-sample-ratio`, `--bits`, `--seed`, `--size-max`, `--chunk-size` |
| Clustering | `--assign-index exact/hnsw`, `--kmeans-assign-index exact/hnsw`, `--kmeans-niter`, `--kmeans-nredo`, `--hnsw-m`, `--hnsw-ef-construction`, `--hnsw-ef-search` |
| CPU control | Preprocessing: `--cpu-threads`, `--faiss-threads`; local search: `--threads` |
| Query | `--k`, `--nprobe`, `--nprobes`, `--k2`, `--nqueries` (`0`: all queries), `--tau` (preprocessing cluster cap) |
| ORAM | `--path-batch-clusters`, `--ring-z`, `--ring-s`, `--ring-a`, `--seed` |
| Network model | `--net-sim none/fast/slow/custom`, `--net-bandwidth-mbps`, `--net-rtt-ms`, `--net-no-sleep` |
| Accounting/payload | `--metadata-storage client/server`, `--exact-payload auto/raw-uint8/raw-float32` |
| Baseline output | Local search and encrypted baseline: `--results-dir` |

Use `--help` on each executable for its accepted options. Command-line values
override JSON settings. QuORAM/FuORAM parameters and exact payload settings are
independent of raw file selection.

## Shared dataset preparation

Raw download, SIFT subset generation, MSMARCO1M sampling and format conversion
are documented in the shared [data guide](../data/README.md).
