# Quick start

Run these commands from the repository root after installing the system
dependencies shown in [README.md](README.md#system-packages). This guide covers the native
research implementations. The three TEE implementations run without an enclave
in native mode; see [README.md](README.md#sgx-build) for SGX SIM/HW compilation.
AnythingLLM has its own [run guide](anything-llm/AnythingLLM_Q-COVERT.md).

## 1. Configure the Python environment

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
source scripts/environment.sh
```

This installs the shared Python dependencies, MKL and Intel OpenMP locally.
Source `scripts/environment.sh` in each new shell to select the checkout's
Python environment and native library paths. `.venv/` is excluded from Git.

## 2. Download and prepare data

Start with SIFT:

```bash
bash data/download_datasets.sh sift
.venv/bin/python data/prepare_datasets.py sift
```

For the other datasets:

```bash
bash data/download_datasets.sh sift1b msmarco1m deep1m
.venv/bin/python data/prepare_datasets.py sift10m sift100m msmarco1m deep1m
```

Omit dataset selectors to download/prepare everything. Downloads create the
required directories and resume `.part` files when rerun. SIFT retains official
GT; every derived dataset recomputes exact L2 top-100 GT against its own base
and all original queries. BIGANN/SIFT100M require substantial disk space and GT
computation time. See [data/README.md](data/README.md) for options and removable
source directories.

## 3. Compile native programs

```bash
source scripts/environment.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBISON_BUILD_NATIVE=ON \
  -DRINGDORAM_BUILD_BISON=ON \
  -DRINGDORAM_ENABLE_SGX=OFF \
  -DQCOVERT_CO_TEST_PYTHON="$PWD/.venv/bin/python" \
  -DRINGDORAM_BENCHMARK_PYTHON="$PWD/.venv/bin/python"
cmake --build build --parallel 2
```

This builds CO, encrypted IVF-RaBitQ, PS, original Compass/BiSON, and the three
TEE native runners. The default Compass-TEE/BiSON-TEE layouts match SIFT.
CO and PS support the other dataset dimensions in the same build. Graph TEE
runners need the matching layout described below. SGX build commands remain in
the [root README](README.md#sgx-build).

## 4. Run experiments

Each entry point reads search parameters from `data/search_params/`. These
examples run the first CSV data row on 1,000 SIFT queries, returning top-10.
Remove `--rows 1` to run every compatible parameter row.

```bash
.venv/bin/python scripts/run_qcovert_co.py --dataset sift --queries 1000 --k 10 --rows 1
.venv/bin/python scripts/run_encrypted_ivf_rabitq.py --dataset sift --queries 1000 --k 10 --rows 1
.venv/bin/python scripts/run_qcovert_ps.py --dataset sift --queries 1000 --k 10 --rows 1
.venv/bin/python scripts/run_compass_tee.py --dataset sift --queries 1000 --k 10 --rows 1
.venv/bin/python scripts/run_bison_tee.py --dataset sift --queries 1000 --k 10 --rows 1
.venv/bin/python scripts/run_compass.py --dataset sift --queries 1000 --k 10 --rows 1
.venv/bin/python scripts/run_bison.py --dataset sift --queries 1000 --k 10 --rows 1
```

The default stage prepares missing indexes, reuses existing artifacts, and runs
the selected CSV rows. Construction uses each scheme's existing default
parameters. Search settings are paired **row by row**, preserving file order:

| Entry point | CSV pattern | Search columns |
| --- | --- | --- |
| CO and encrypted IVF-RaBitQ | `QCOVERT_CO_<DATASET>.csv` | `nprobe,k2` |
| PS | `QCOVERT_PS_<DATASET>.csv` | `nprobe,k2` |
| Compass and Compass-TEE | `Compass_<DATASET>.csv` | `ef,ef_spec,ef_n` |
| BiSON and BiSON-TEE | `bison_<DATASET>.csv` | `T,W,L` |

The TEE and original versions of Compass/BiSON use the same parameter files.
BiSON T is an I/O limit in its plaintext path and an iteration count in its
ORAM paths. The adapters map columns to each implementation's own arguments;
they do not form Cartesian products. Rows with `k2 < k` or `L < k` are reported
and skipped without changing their budgets. No compatible rows is an error.

### Other original Compass/BiSON paths

```bash
.venv/bin/python scripts/run_compass.py --dataset sift --path low-storage --rows 1
.venv/bin/python scripts/run_compass.py --dataset sift --path reference --rows 1
.venv/bin/python scripts/run_bison.py --dataset sift --path low-storage --rows 1
.venv/bin/python scripts/run_bison.py --dataset sift --path plain --rows 1
```

The default path is `oram`. Compass `reference` is its existing access-leaking
networked reference. Original client/server paths choose local ports, wait for
servers without consuming their connections, and stop their own process groups
on completion or failure. Different paths have separate initial states. BiSON
clones mutable bucket stores for each search group (using reflinks when
available) and removes those scratch clones afterwards.

### Common options

| Option | Meaning / default |
| --- | --- |
| `--dataset` | `sift` (default), `sift10m`, `sift100m`, `deep1m`, `msmarco1m`; case insensitive |
| `--queries N` | Queries per parameter row; default 1000, `0` means all available queries |
| `--k K` | Final top-k and recall@k, from 1 to 100; default 10 |
| `--params FILE` | Override the automatically selected input CSV |
| `--rows 1,3-5` | Select 1-based data rows, excluding the header; default all |
| `--stage all/prepare/search` | Default `all`; prepare offline artifacts or search existing ones separately |
| `--rebuild` | Force reconstruction; requires `all` or `prepare` |
| `--threads N` | Override supported preparation/search thread controls; otherwise keep scheme defaults |
| `--data-root DIR` | Shared dataset root; default repository `data/` |
| `--build-dir DIR` | Root build directory or standalone scheme build; default repository `build/` |
| `--results-dir DIR` | Override result root; default `<scheme>/results/<entry-point>/` |
| `--dry-run` | Print selected rows and commands without changing files or starting processes |
| `--help` | Show entry-point options |

All three TEE runners default to 32 query worker threads. Compass-TEE
preparation also defaults to 32 threads.

The three TEE entry points also accept `--mode native/sgx-sim/sgx-hw` (default
`native`); SGX requires a separately compiled runner. Original Compass accepts
`--path oram/low-storage/reference`; original BiSON accepts
`--path oram/low-storage/plain`.

For example, inspect a top-100 sweep, then prepare and search independently:

```bash
.venv/bin/python scripts/run_qcovert_co.py --dataset sift --k 100 --dry-run
.venv/bin/python scripts/run_qcovert_co.py --dataset sift --stage prepare
.venv/bin/python scripts/run_qcovert_co.py --dataset sift --stage search --queries 1000 --k 100
```

ORAM images for PS and the graph TEE schemes initialize on the first search,
then reusable pristine images are retained. PS falls back to its configured
k-means construction if optional external cluster labels/centroids are absent.
Changing construction parameters or replacing a dataset requires `--rebuild`.

Outputs are separated as `<result-root>/<dataset>/<path-or-mode>/k<K>_q<N>/`.
Each run records its configuration, selected CSV rows and executed commands
in `run.json`, alongside logs and the implementation's result CSVs. CO writes
its CSVs in a nested `results/` directory; other adapters use `summary*.csv`.
Reruns retain each implementation's existing append/update behavior.
Original Compass ORAM CSVs keep their native metric-row/parameter-column layout.

## Native graph TEE builds for other datasets

Compass-TEE and BiSON-TEE must match the prepared dimension, scalar and PQ
layout. Their default construction settings use degree 128, Compass PQ 8
bytes, and BiSON PQ 16 bytes (32 for SIFT prefixes). For example:

```bash
# Choose deep1m, msmarco1m, or sift10m.
tee_dataset=deep1m
case "$tee_dataset" in
  deep1m)    tee_dim=96;  tee_scalar=float32; tee_pq=16 ;;
  msmarco1m) tee_dim=768; tee_scalar=float32; tee_pq=16 ;;
  sift10m)   tee_dim=128; tee_scalar=uint8;   tee_pq=32 ;;
esac
tee_build="build-tee-$tee_dataset"
cmake -S Q-COVERT-PS -B "$tee_build" -DCMAKE_BUILD_TYPE=Release \
  -DRINGDORAM_ENABLE_SGX=OFF -DRINGDORAM_BUILD_BISON=ON \
  -DCOMPASS_DIM="$tee_dim" -DCOMPASS_SCALAR="$tee_scalar" \
  -DBISON_DIM="$tee_dim" -DBISON_SCALAR="$tee_scalar" -DBISON_PQ_BYTES="$tee_pq"
cmake --build "$tee_build" --parallel 2
.venv/bin/python scripts/run_compass_tee.py --dataset "$tee_dataset" --build-dir "$tee_build" --rows 1
.venv/bin/python scripts/run_bison_tee.py --dataset "$tee_dataset" --build-dir "$tee_build" --rows 1
```

BiSON-TEE preparation uses `build/bison/bin/build_disk_index` from the root
native build above. Standalone layouts may reuse that builder. Compass/BiSON
currently have no SIFT100M search CSV; supply a suitable `--params` file before
running their TEE variants or original Compass on it. Original BiSON has no
SIFT100M profile. CO/PS have CSVs for all five shared datasets.
