# BiSON: plaintext, original ORAM and low-client-storage ORAM

For the CSV-driven experiment entry points, see the repository
[quick start](../QUICKSTART.md).

Install and compile using [QUICKSTART.md](../QUICKSTART.md). Run commands from
this directory with the shared environment. The TEE adaptation is separately
documented in `../Q-COVERT-PS/bison/README.md`.

```bash
BISON_BIN="$PWD/../build/bison/bin"
```

## Prepare data and build DiskANN

```bash
../.venv/bin/python scripts/benchmark.py sift prepare
../.venv/bin/python scripts/benchmark.py sift build --build-dir "$BISON_BIN"
```

Datasets: `sift`, `sift10m`, `deep1m`, `msmarco1m`. Raw filenames follow PS
profiles, including the shared `sift10M` and `deep1M` directory capitalization.
SIFT/DEEP1M/MSMARCO1M use float32, SIFT10M uses uint8; all current profiles use L2.
Preparation writes native header-bin copies, indexes, states and results under
`../data/prepared/bison/<dataset>/`. It never modifies the raw inputs. SIFT10M
float queries are converted only if their values are exactly representable as
uint8. Generated files are not interchangeable with BiSON-TEE prepared files.

`prepare` updates the chosen configuration (`config/bench_<dataset>.json` by
default). `--source` replaces the raw directory; `--base`, `--query`, and
`--groundtruth` override individual files. `--data-root` selects the generated
artifact root. Relative paths inside configs resolve against the outer root;
absolute custom paths are supported.

Changing the base prefix/sample requires matching ground truth:

```bash
../.venv/bin/python scripts/benchmark.py sift groundtruth --build-dir "$BISON_BIN"
```

## Plaintext

```bash
../.venv/bin/python scripts/benchmark.py sift plain --build-dir "$BISON_BIN" \
  --t 5,6 --w 20,16 --queries 100 --k 10
```

This invokes DiskANN `search_disk_index` without ORAM client/server processes.
Plaintext T maps to the I/O limit, while ORAM T is the iteration count; they are
not identical search budgets. Sweeps pair T/W positionally unless
`--pairing product` is selected.

## Original ORAM

```bash
../.venv/bin/python scripts/benchmark.py sift oram-init --build-dir "$BISON_BIN"
# Terminal A
../.venv/bin/python scripts/benchmark.py sift server --build-dir "$BISON_BIN" --port 8000 --memory
# Terminal B
../.venv/bin/python scripts/benchmark.py sift client --build-dir "$BISON_BIN" \
  --port 8000 --t 5,6 --w 20,16 --queries 100 --k 10
```

## Low client storage

Prepare/build the main DiskANN index first, then initialize the low-storage
configuration. Initialization updates the derived auxiliary block counts/tree
levels in that configuration. This command builds the graph ORAM and the required auxiliary
PQ/position-map ORAMs:

```bash
../.venv/bin/python scripts/benchmark.py sift oram-init --build-dir "$BISON_BIN" \
  --config config/bench_low_sift.json
# Terminal A: graph server, also using port+1
../.venv/bin/python scripts/benchmark.py sift server --build-dir "$BISON_BIN" \
  --config config/bench_low_sift.json --port 8000 --memory
# Terminal B: PQ server
../.venv/bin/python scripts/benchmark.py sift server --build-dir "$BISON_BIN" \
  --config config/bench_low_sift.json --role pq --port 8002 --memory
# Terminal C: client
../.venv/bin/python scripts/benchmark.py sift client --build-dir "$BISON_BIN" \
  --config config/bench_low_sift.json --port 8000 --t 5,6 --w 20,16 --queries 100
```

When `recursive_posmap=true`, add the position-map server at port+3, using the
same dataset/config (SIFT10M enables it by default):

```bash
../.venv/bin/python scripts/benchmark.py sift10m server --build-dir "$BISON_BIN" \
  --config config/bench_low_sift10m.json --role posmap --port 8003 --memory
```

Low storage forces zero graph-cache nodes and zero tree-top cache levels.
Other PQ/position-map settings retain their configured values. Keep pristine
initial states when comparing paths, and do not share mutable run state across
simultaneous experiments.

## Options

| Purpose | Options |
| --- | --- |
| Paths | `--source`, `--base`, `--query`, `--groundtruth`, `--data-root`, `--config`, `--build-dir`, `--csv` |
| Preparation | `--base-count`; raw fvecs/bvecs and header-bin inputs are supported |
| Index construction | `--pq-bytes`, `--ram-gb`, `--build-l` |
| Search | `--t`, `--w` (comma-separated lists), `--pairing zip/product`, `--l`, `--k` |
| Measurement | `--queries`, `--threads` |
| Client/server | `--server-ip`, `--port`, `--memory`, `--io-events`, `--role graph/pq/posmap`, `--integrity` |
| Original client cache | `--cache-nodes`, `--cached-levels` (low storage overrides both to zero) |

Server `--memory` loads buckets into RAM; otherwise it uses the existing disk
backend. Auxiliary roles require explicit matching ports. CSV modes `plain`,
`oram`, and `oram_low` share the dataset CSV without replacing each other's
parameter rows. Logs and private client configs use separate `results/oram/`
and `results/oram_low/` directories. The underlying binaries accept `name=value`
arguments; prefer this driver for consistent path handling and summaries.
