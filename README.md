# Q-COVERT

Code for *One Architecture, Two Scenarios: Q-COVERT for Client-Owned and Provider-Served Oblivious Vector Search*. Install and compile the native research implementations
from this directory. Scheme READMEs contain preparation/search commands and
runtime options. See [QUICKSTART.md](QUICKSTART.md) for dataset download, native
build, and CSV-driven experiment commands.

| Implementation | Run guide | Root-build program directory |
| --- | --- | --- |
| Q-COVERT-CO, local/encrypted IVF-RaBitQ | [Q-COVERT-CO](Q-COVERT-CO/README.md) | `build/Q-COVERT-CO/` |
| Q-COVERT-PS | [Q-COVERT-PS](Q-COVERT-PS/README.md) | `build/Q-COVERT-PS/` |
| Compass-TEE | [Compass-TEE](Q-COVERT-PS/compass/README.md) | `build/Q-COVERT-PS/compass/` |
| BiSON-TEE | [BiSON-TEE](Q-COVERT-PS/bison/README.md) | `build/Q-COVERT-PS/bison/` |
| Original Compass: ORAM/low storage/access-leaking reference | [Compass](Compass/README.md) | `build/Compass/` |
| Original BiSON: plaintext/ORAM/low storage | [BiSON](bison/README.md) | `build/bison/bin/` |

The upstream repositories are [Compass](https://github.com/Clive2312/compass)
and [BiSON](https://github.com/r-dott/bison).
Their lightweight-client and TEE variants are described in
[Baseline Adaptations](BASELINE_ADAPTATIONS.md).

## Panther

Panther is available in the upstream
[OpenPanther repository](https://github.com/AntCPLab/OpenPanther). We made no
modifications to Panther, so its source code is not included here.

## AnythingLLM application

`anything-llm/` integrates Q-COVERT with AnythingLLM for document retrieval and
chat. It has its own dependencies, build outputs and runtime data. Enter that
folder to install, build or run the application; see
[AnythingLLM_Q-COVERT.md](anything-llm/AnythingLLM_Q-COVERT.md). The root CMake,
CTest and dependency-installation commands cover the research implementations
listed above and exclude this application.

## Dependencies

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

### Shared Python and MKL environment

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
source scripts/environment.sh
```

`requirements.txt` pins NumPy, Faiss and `mkl-devel`. MKL development files and
Intel OpenMP are installed in `.venv/include` and `.venv/lib`.
`requirements.lock.txt` records the complete tested Python resolution. CMake detects
this checkout-local installation automatically. For a system MKL installation,
pass `-DMKL_PATH=/path/to/lib -DMKL_INCLUDE_PATH=/path/to/include
-DOMP_PATH=/path/to/intel-openmp/lib` instead. These are library/header directory
paths, not filenames. Source `scripts/environment.sh` in a new shell when using
the shared environment or calling tools directly.

### Without sudo: local Ubuntu development libraries

On Ubuntu 24.04, when the compiler, Python, OpenSSL headers and Boost headers
are already installed, missing libraries can be extracted without changing the
system package database:

```bash
bash scripts/install-local-deps.sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
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

## Build all current native schemes

```bash
source scripts/environment.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBISON_BUILD_NATIVE=ON \
  -DRINGDORAM_BUILD_BISON=ON \
  -DQCOVERT_CO_TEST_PYTHON="$PWD/.venv/bin/python" \
  -DRINGDORAM_BENCHMARK_PYTHON="$PWD/.venv/bin/python"
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 2
```

For small real client/server checks of the original Compass and BiSON paths:

```bash
.venv/bin/python scripts/smoke-native-baselines.py --only all
```

This uses 512 synthetic vectors, isolated configurations and local ports. Logs
stay under `build/smoke-logs/`; shared raw datasets are untouched.

The default build includes CO, PS, Compass and BiSON adapter checks. The explicit
`BISON_BUILD_NATIVE=ON` above enables the original BiSON programs; the separate
`RINGDORAM_BUILD_BISON=ON` enables BiSON-TEE. The two options are independent.
Reduce parallelism if DiskANN template compilation exceeds available memory.
The tests use small datasets; they do not reproduce all paper figures.

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

### TEE baseline layouts

Compass-TEE and BiSON-TEE use compile-time layouts that must match their prepared
files. Defaults are SIFT layouts. Configure a separate PS build for other layouts:

| Setting | CMake options |
| --- | --- |
| Compass-TEE | `COMPASS_DIM=128`, `COMPASS_DEGREE=128`, `COMPASS_PQ_SUBSPACES=8`, `COMPASS_PQ_CENTROIDS=256`, `COMPASS_SCALAR=float32` |
| BiSON-TEE | `BISON_DIM=128`, `BISON_DEGREE=128`, `BISON_PQ_BYTES=16`, `BISON_SCALAR=float32` |
| Multiple BiSON-TEE layouts | `RINGDORAM_BISON_LAYOUTS="96:128:16:float32;128:128:32:uint8"` |

For example, DEEP1M:

```bash
cmake -S Q-COVERT-PS -B build-ps-deep -DCMAKE_BUILD_TYPE=Release \
  -DRINGDORAM_BUILD_BISON=ON -DCOMPASS_DIM=96 -DCOMPASS_SCALAR=float32 \
  -DBISON_DIM=96 -DBISON_SCALAR=float32 -DBISON_PQ_BYTES=16
cmake --build build-ps-deep --parallel 2
```

For SIFT10M/100M, set both scalar options to `uint8` and choose the actual PQ
layout reported by preparation. Point the runtime `--build-dir` to this PS build.

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

## dataset

We evaluate Q-COVERT on five database configurations from SIFT, DEEP, and
MS MARCO, covering local image descriptors, learned image representations,
and text embeddings. Storage types below refer to original coordinates
before index quantization.

| Dataset | Base vectors | Queries | Dimensions | Original storage type |
| --- | --- | --- | --- | --- |
| SIFT1M | 1,000,000 | 10,000 | 128 | float32 |
| SIFT10M | 10,000,000 | 10,000 | 128 | uint8 |
| SIFT100M | 100,000,000 | 10,000 | 128 | uint8 |
| DEEP1M | 1,000,000 | 10,000 | 96 | float32 |
| MS MARCO-1M | 1,000,000 | 6,980 | 768 | float32 |

**SIFT1M.** We use the standard 128-dimensional SIFT benchmark from
[TEXMEX](http://corpus-texmex.irisa.fr/). Its released local image descriptors
are used directly, without extracting features again from source images.

**SIFT10M and SIFT100M.** Following BiSON, we retain the first ten million and
one hundred million vectors of SIFT1B (BIGANN), respectively, preserving their
original order, uint8 coordinates, and query set. These prefixes are distinct
from the standard SIFT1M release.

**DEEP1M.** We retain the first one million vectors of DEEP1B in their original
order and use its original queries. These 96-dimensional float32 descriptors
are learned image representations generated by a deep neural network.

**MS MARCO-1M.** We randomly sample one million passage vectors from the
approximately 8.8 million passages represented in the public
[Compass artifact](https://github.com/Clive2312/compass), using its precomputed
768-dimensional embeddings and all 6,980 dev-set query embeddings. No new
embeddings are generated from raw text.

**Ground truth and recall.** All schemes use Euclidean (L2) distance. Ground
truth contains exact nearest neighbors over each actual evaluation database,
obtained by brute-force search using original coordinates rather than
quantized codes. Each prefix or sampled subset has matching ground truth
for all corresponding queries. Let $R_k(q)$ denote exact nearest-neighbor
IDs, $G_k(q)$ returned IDs, and $Q$ the evaluation query set:

$$
\operatorname{Recall}@k = \frac{1}{|Q|}\sum_{q\in Q}
\frac{|R_k(q)\cap G_k(q)|}{k}.
$$

**Reranking precision.** For float32 configurations, Q-COVERT-CO stores 8-bit
RaBitQ codes in FuORAM for reranking, padded to a multiple of 64 dimensions.
The uint8 configurations retain their original vectors. Ground truth always
uses original coordinates.

Download and preparation commands, filenames, formats, and directory layouts
are documented in [data/README.md](data/README.md).
