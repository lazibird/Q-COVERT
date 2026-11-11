# Quick start

Run these commands from the repository root. This guide covers dependencies,
native builds, and CSV-driven experiments for the bundled research
implementations. The three TEE implementations run without an enclave in
native mode; [SGX SIM/HW builds](#sgx-build) are described below.
AnythingLLM has its own [run guide](anything-llm/AnythingLLM_Q-COVERT.md).

## 1. Install dependencies

The supported installation below targets x86-64 Ubuntu 24.04, GCC 13, Python
3.12 and an AVX2-capable CPU. CMake >= 3.23.1 is required; PS `--preset` commands
require CMake >= 3.25. C++17 is used by CO/Compass and C++20 by PS/BiSON.

| Dependency | Used by |
| --- | --- |
| CMake, C/C++ toolchain, OpenSSL development files, OpenMP | Native implementations |
| NumPy, Faiss Python | CO/PS preprocessing, Compass index preparation and Python integration tests |
| Intel MKL headers/libraries and Intel OpenMP | Original BiSON/DiskANN; DiskANN index construction for BiSON-TEE |
| Boost program_options, libaio, liburing, gperftools/tcmalloc | Original BiSON/DiskANN |
| BLAS/LAPACK | Optional original Compass HE/cluster baselines |
| Intel SGX SDK | Optional PS/Compass-TEE/BiSON-TEE SIM/HW builds |
| Google Cloud SDK Python packages, Paramiko, gdown, pandas, matplotlib, rich, tcconfig | Optional historical cloud/download/plotting scripts; not needed by local experiments |

### System packages

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config git curl python3-dev python3-venv \
  libssl-dev libomp-dev libboost-program-options-dev libaio-dev liburing-dev \
  libgoogle-perftools-dev libunwind-dev libblas-dev liblapack-dev
```

### Python and MKL environment

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
source scripts/environment.sh
```

`requirements.txt` pins NumPy, Faiss and `mkl-devel`. MKL development files and
Intel OpenMP are installed in `.venv/include` and `.venv/lib`.
`requirements.lock.txt` records the complete tested Python resolution. CMake detects
this checkout-local installation automatically. For a system MKL installation,
pass `-DMKL_PATH=/path/to/lib -DMKL_INCLUDE_PATH=/path/to/include
-DOMP_PATH=/path/to/intel-openmp/lib` instead. These are library/header directory
paths, not filenames. Source `scripts/environment.sh` in each new shell to select
the checkout's Python environment and native library paths.

### Without sudo: local Ubuntu development libraries

On Ubuntu 24.04, when the compiler, Python, OpenSSL headers and Boost headers
are already installed, missing libraries can be extracted without changing the
system package database:

```bash
bash scripts/install-local-deps.sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
source scripts/environment.sh
```

Packages are stored under `.deps/`; CMake uses `.deps/usr` for package discovery,
headers, library search and build RPATHs. The helper also repairs Debian
alternatives links and relocates pkg-config metadata. This is an alternative to
the system-package command, not a replacement for the compiler/OpenSSL/Boost
headers. `.deps/` and `.venv/` are excluded from Git.

### Optional legacy Python tools

Only when using upstream cloud orchestration or plotting:

```bash
.venv/bin/python -m pip install \
  google-cloud-storage google-cloud-compute paramiko gdown pandas matplotlib rich tcconfig
```

Cloud account configuration and machine/network privileges are outside the
local experiment commands. Vendored Eigen, DiskANN, Faiss and SEAL sources
retain their licenses. Compass upstream dependency revisions are recorded in
`Compass/third_party/dependencies.json`.

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
runners need the [matching layout](#native-graph-tee-builds-for-other-datasets)
described below. For enclave builds, see [SGX build](#sgx-build).

The default build includes CO, PS, Compass and BiSON adapter checks. The explicit
`BISON_BUILD_NATIVE=ON` above enables the original BiSON programs; the separate
`RINGDORAM_BUILD_BISON=ON` enables BiSON-TEE. The two options are independent.
Reduce parallelism if DiskANN template compilation exceeds available memory.
See [Validate the installation](#validate-the-installation) for small-dataset checks.

| Build output | Implementations |
| --- | --- |
| `build/Q-COVERT-CO/` | Q-COVERT-CO and IVF-RaBitQ baselines |
| `build/Q-COVERT-PS/` | Q-COVERT-PS, with graph TEE runners under `compass/` and `bison/` |
| `build/Compass/` | Compass and Compass-ls |
| `build/bison/bin/` | BiSON and BiSON-ls, including the DiskANN builder |

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

Compass-TEE and BiSON-TEE use compile-time layouts that must match their prepared
dimension, scalar, degree, and PQ settings. Defaults are SIFT layouts:

| Setting | CMake options |
| --- | --- |
| Compass-TEE | `COMPASS_DIM=128`, `COMPASS_DEGREE=128`, `COMPASS_PQ_SUBSPACES=8`, `COMPASS_PQ_CENTROIDS=256`, `COMPASS_SCALAR=float32` |
| BiSON-TEE | `BISON_DIM=128`, `BISON_DEGREE=128`, `BISON_PQ_BYTES=16`, `BISON_SCALAR=float32` |
| Multiple BiSON-TEE layouts | `RINGDORAM_BISON_LAYOUTS="96:128:16:float32;128:128:32:uint8"` |

BiSON-TEE uses 32 PQ bytes for SIFT prefixes. Configure a separate PS build for
other layouts and pass it with `--build-dir`, for example:

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

## Additional build options

### Standalone builds

To build only one scheme, use the same dependency environment:

```bash
cmake -S Q-COVERT-CO -B Q-COVERT-CO/build -DCMAKE_BUILD_TYPE=Release
cmake --build Q-COVERT-CO/build --parallel 2

cmake -S Q-COVERT-PS -B Q-COVERT-PS/build/native-release \
  -DCMAKE_BUILD_TYPE=Release -DRINGDORAM_BUILD_BISON=ON
cmake --build Q-COVERT-PS/build/native-release --parallel 2

cmake -S Compass -B Compass/build -DCMAKE_BUILD_TYPE=Release
cmake --build Compass/build --parallel 2

cmake -S bison -B bison/build -DCMAKE_BUILD_TYPE=Release -DBISON_BUILD_NATIVE=ON
cmake --build bison/build --parallel 2
```

Run guides use root-build locations; override their `--build-dir` or executable
paths when using standalone builds. Root options `QCOVERT_BUILD_PS`,
`QCOVERT_BUILD_COMPASS`, and `QCOVERT_BUILD_BISON` can disable those directories.
CO dimension-specific binaries (`_d96`, `_d384`, `_d768`) are built automatically.

### DiskANN builder for BiSON-TEE

The full original BiSON build already provides
`build/bison/bin/build_disk_index`; pass it to BiSON-TEE preparation with
`--builder`. Alternatively build the trimmed vendored builder separately:

```bash
cmake -S Q-COVERT-PS/vendor/diskann -B Q-COVERT-PS/build/diskann-builder \
  -DCMAKE_BUILD_TYPE=Release
cmake --build Q-COVERT-PS/build/diskann-builder --target build_disk_index --parallel 2
```

### Optional original Compass HE/lexical tools

These are upstream extras, separate from the two Compass ORAM paths:

```bash
cmake -S Compass -B build-compass-extras -DCMAKE_BUILD_TYPE=Release \
  -DCOMPASS_BUILD_LEGACY_BASELINES=ON
cmake --build build-compass-extras --parallel 2
```

Missing vendored optional sources can be restored with
`bash Compass/script/setup_third_party.sh` at the pinned revisions, without
creating nested Git repositories.

## SGX build

An Intel SGX SDK is required in SIM and HW modes. If it is not installed,
obtain the Intel Linux x64 SDK installer for your distribution following the
[official Intel SGX installation guide](https://cc-enabling.trustedservices.intel.com/intel-sgx-sw-installation-guide-linux/02/installation_instructions/) and install it:

```bash
SGX_INSTALLER=/path/to/sgx_linux_x64_sdk_installer.bin
bash "$SGX_INSTALLER" --prefix "$HOME/intel"
export SGX_SDK_DIR="$HOME/intel/sgxsdk"
# Or use an existing SDK, for example:
# export SGX_SDK_DIR=/opt/intel/sgxsdk
```

Build all three PS-family SIM runners:

```bash
cmake -S . -B build-sgx-sim -DCMAKE_BUILD_TYPE=Release \
  -DQCOVERT_BUILD_COMPASS=OFF -DQCOVERT_BUILD_BISON=OFF \
  -DRINGDORAM_ENABLE_SGX=ON -DRINGDORAM_SGX_MODE=SIM \
  -DSGX_SDK_DIR="${SGX_SDK_DIR:-/opt/intel/sgxsdk}" \
  -DRINGDORAM_BUILD_BISON=ON \
  -DRINGDORAM_BENCHMARK_PYTHON="$PWD/.venv/bin/python"
cmake --build build-sgx-sim --parallel 2
ctest --test-dir build-sgx-sim --output-on-failure --parallel 2
```

For HW, use a separate `build-sgx-hw` directory and
`-DRINGDORAM_SGX_MODE=HW`. HW requires an SGX-capable host, working enclave device
access and appropriate platform setup. SIM is functional validation, not hardware
security validation. Set `RINGDORAM_SGX_HEAP_SIZE` for larger indexes; optional
`RINGDORAM_SGX_HEAP_VARIANTS` compiles additional heap images.
`RINGDORAM_SGX_DATASET` reads a profile heap size in SIM mode. The Compass/BiSON
layout options above also apply to SGX builds.

## Validate the installation

After the native build, run the small-dataset test suite:

```bash
ctest --test-dir build --output-on-failure --parallel 2
```

These checks do not reproduce all paper figures.

For small real client/server checks of the original Compass and BiSON paths:

```bash
.venv/bin/python scripts/smoke-native-baselines.py --only all
```

This uses 512 synthetic vectors, isolated configurations and local ports. Logs
stay under `build/smoke-logs/`; shared raw datasets are untouched.
