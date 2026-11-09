# Compass: original and low-client-storage paths

For the CSV-driven experiment entry points, see the repository
[quick start](../QUICKSTART.md).

Install and compile using the [root README](../README.md). Commands below run
from this directory. Executables come from the outer build, `../build/Compass/`.
The TEE adaptation has a separate guide in `../Q-COVERT-PS/compass/README.md`.

## Prepare indexes and graph snapshots

The shared SIFT/DEEP1M/MSMARCO1M/SIFT10M/100M raw paths follow PS settings.
`tests/config/config_ring.json` paths resolve against the outer repository root.
HNSW/PQ indexes are placed in `../data/prepared/compass/indexes/<dataset>/`;
client/server snapshots use `../data/prepared/compass/snap/`.

For example, create SIFT graph/PQ indexes from the configured raw input:

```bash
../.venv/bin/python script/build_bigann_hnsw.py sift --threads 8
../.venv/bin/python script/build_bigann_pq.py sift --threads 8
../build/Compass/compass_init d=sift
```

The builders also support the configured large and float32 datasets. Existing
compatible HNSW/IndexPQ files may be supplied at `index_path` and `pq_path`.
`compass_init` initializes ORAM states; it does not train the graph or PQ model.
Retain a pristine snapshot copy when comparing different paths.

## Original ORAM

Run these in separate terminals with matching switches:

```bash
# Server
../build/Compass/test_compass_ring r=1 d=sift p=8000 pq_oram=0 pm_oram=0 server_metadata=0 net=none
# Client
../build/Compass/test_compass_ring r=2 d=sift p=8000 n=100 pq_oram=0 pm_oram=0 server_metadata=0 net=none
```

## Low client storage

```bash
# Main server (also serves the auxiliary channels)
../build/Compass/test_compass_ring r=1 d=sift p=8000 pq_oram=1 pm_oram=0 server_metadata=1 net=none
# Client
../build/Compass/test_compass_ring r=2 d=sift p=8000 n=100 pq_oram=1 pm_oram=0 server_metadata=1 net=none
```

Use `pm_oram=1` on both parties to enable the recursive graph position-map ORAM.
`pq_oram=1` stores PQ codes remotely. The server uses adjacent ports for the
backup/PQ/position-map channels. `server_metadata` controls existing metadata
accounting; it does not relocate every local metadata structure by itself.

The local wrapper supports both modes:

```bash
COMPASS_BUILD_DIR="$PWD/../build/Compass" PQ_ORAM=0 PM_ORAM=0 SERVER_METADATA=0 bash script/run_compass_ring_local.sh sift
COMPASS_BUILD_DIR="$PWD/../build/Compass" PQ_ORAM=1 PM_ORAM=0 SERVER_METADATA=1 bash script/run_compass_ring_local.sh sift
```

Without an explicit `PM_ORAM`, the wrapper enables it for SIFT10M/100M and
disables it for the other datasets. Defaults are otherwise retained.

## Access-leaking accuracy reference

```bash
# Server
../build/Compass/test_compass_accuracy r=1 d=sift p=8000
# Client
../build/Compass/test_compass_accuracy r=2 d=sift p=8000 n=100 topk=10
```

This path directly fetches target blocks and omits normal ORAM eviction. It
retains networking, encrypted blocks and snapshots; it is not a pure in-memory
HNSW timing baseline. `test_compass_accuracy_ablation` and `test_compass_tp`
retain the upstream ablation and throughput entry points.

## Options

| Entry | Options |
| --- | --- |
| Index builders | Dataset positional argument; `--config`, `--base`, `--output`, `--metric`, `--threads`, `--batch-size`; use `--help` for stage-specific HNSW/PQ parameters |
| Common client/server | `r=1/2`, `d=`, `c=` (configuration JSON), `p=`, `ip=`, `n=` |
| Search | `efspec=`, `efn=`, `ef_search=`, `ef_upper_steps=`, `topk=` |
| Graph ORAM | `batch=`, `lazy=`, `pm_oram=`, `pm_entries_per_block=`, `pm_oram_state_path=` |
| PQ ORAM | `pq_oram=`, `pq_oram_path=`, `pq_codes_per_block=`, `pq_oram_state_path=`, `pq_metadata_client=` |
| Accounting/network | `server_metadata=`, `net=none/fast/slow` (also `1/2/3`) |
| Output | `f_csv=`, `f_latency=`, `f_accuracy=`, `f_comm=` as supported by the entry point |

Wrapper environment variables include `EF_SPEC`, `EFN`, `EF_SEARCH`,
`EF_UPPER_STEPS`, `TOPK`, `NET_PROFILE`, `RESULT_CSV`, `PQ_ORAM`, `PM_ORAM`,
`SERVER_METADATA`, `PQ_CODES_PER_BLOCK`, `PM_ENTRIES_PER_BLOCK`,
`PQ_ORAM_STATE_PATH`, `PM_ORAM_STATE_PATH`, `AUTO_BUILD_SNAP` and
`COMPASS_BUILD_DIR`. Results default to this scheme's `results/<dataset>/`.

TRIP, LAION and full MSMARCO profiles retain the upstream data layout staged
under `../data/compass/upstream/`. Historical GCP/cloud scripts require their
own remote-machine setup and are not part of these local commands.
