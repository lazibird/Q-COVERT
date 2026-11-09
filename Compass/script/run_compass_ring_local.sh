#!/usr/bin/env bash
# Run test_compass_ring locally, starting the server before the client. No tc network emulation by default.
#
# Usage (from the Compass directory):
#   ./script/run_compass_ring_local.sh [dataset] [port] [query count n] [optional ef_search]
# Examples:
#   ./script/run_compass_ring_local.sh sift 8000 100
#   ./script/run_compass_ring_local.sh sift 8000 100 64   # Argument 4 applies only when EF_SEARCH is unset.
# Alternatively: EF_SEARCH=64 ./script/run_compass_ring_local.sh sift 8000 100
#
# ---------------------------------------------------------------------------
# Parameter configuration: choose either method below.
#
# Method A: persistent defaults.
#   Edit tests/config/config_ring.json.
#   Set ef_spec, ef_neighbor, ef_search, and ef_upper_steps for the dataset.
#   The script reads these JSON settings in section 1 and merges overrides in section 2.
#
# Method B: temporary overrides without changing JSON.
#   Export variables in the shell before running, for example:
#     EFN=24 EF_SEARCH=30 ./script/run_compass_ring_local.sh sift 8000 100
#   Alternatively, uncomment the exports in section 3 for local defaults.
#
# Environment variables: EF_SPEC, EFN, EF_SEARCH, EF_UPPER_STEPS, TOPK, USE_BATCHING, USE_LAZY, SERVER_WAIT, NET_PROFILE, RESULT_CSV, PQ_ORAM, PQ_ORAM_PATH, PQ_CODES_PER_BLOCK, PQ_ORAM_STATE_PATH, PQ_METADATA_CLIENT, PM_ORAM, PM_ENTRIES_PER_BLOCK, PM_ORAM_STATE_PATH, SERVER_METADATA, AUTO_BUILD_SNAP
# See the option descriptions below or the README.
# ---------------------------------------------------------------------------
#
# Search defaults come from ef_spec, ef_neighbor, ef_search, and ef_upper_steps
# in tests/config/config_ring.json. Environment overrides do not rebuild HNSW or graph_cache:
#   EF_SPEC           Override efspec (JSON ef_spec by default).
#   EFN               Override efn (JSON ef_neighbor by default).
#   EF_SEARCH         Override ef_search (from JSON by default).
#   EF_UPPER_STEPS    Override ef_upper_steps (from JSON by default).
#   TOPK              Override topk, taking precedence over DEF_TOPK.
#   DEF_TOPK          Script-only default; the binary uses k=10 if topk is omitted.
# Optional ORAM behavior; both endpoints receive the same settings:
#   USE_BATCHING=0    Pass batch=0 to disable batching.
#   USE_LAZY=0        Pass lazy=0 to disable lazy eviction.
# Optional latency estimates based on traffic and RTT counts; no tc configuration:
#   NET_PROFILE=none  No additional network delay (default; alias: 1).
#   NET_PROFILE=fast  3 Gbps, 1 ms RTT (alias: 2).
#   NET_PROFILE=slow  400 Mbps, 80 ms RTT (alias: 3).
# Optional result CSV:
#   RESULT_CSV=...    Override the default result/top${TOPK}.csv path in the dataset vector directory.
# Optional PQ code ORAM and metadata accounting:
#   PQ_ORAM=0         Disable the second RingORAM for PQ codes (enabled by default).
#   PQ_ORAM_PATH=...  Override the PQ index; defaults to JSON pq_path (pq_full_8.index for SIFT).
#   PQ_CODES_PER_BLOCK=... Override PQ codes per block; defaults to block payload bytes / PQ code_size.
#   PQ_ORAM_STATE_PATH=... Override the server snapshot path derived from the PQ file and ORAM parameters.
#   PQ_METADATA_CLIENT=1 Account for PQ metadata at the client (default: 0, server); accounting only.
#   PM_ORAM=1         Enable one recursive graph position-map ORAM layer (default for SIFT10M/100M).
#   PM_ENTRIES_PER_BLOCK=32 Override graph position-map entries per PM ORAM block.
#   PM_ORAM_STATE_PATH=... Override the PM server snapshot path derived from dataset and ORAM parameters.
#   SERVER_METADATA=0 Disable additional RTT accounting for remote metadata; local metadata logic is unchanged.
# Optional snapshot initialization:
#   AUTO_BUILD_SNAP=0 Fail if graph ORAM snapshots are missing; otherwise run compass_init and persist them.
#
# Input paths must match tests/config/config_ring.json, relative to the outer repository data directory.
# Increase SERVER_WAIT when loading large bucket files.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${COMPASS_BUILD_DIR:-${REPO_ROOT}/build}"

# ========== Positional argument defaults ==========
DATASET="${1:-sift}"   # Default dataset for an omitted $1; must match a config_ring.json key.
PORT="${2:-8000}"      # $2: port.
N="${3:-100}"          # $3: query count n.
ARG_EF_SEARCH="${4:-}" # $4: temporary ef_search override; EF_SEARCH takes precedence.


# ========== Section 3: optional local overrides, equivalent to method B ==========
# export EF_SPEC=4
# export EFN=12
# export EF_SEARCH=20
# export EF_UPPER_STEPS=1
# export TOPK=10
# export USE_BATCHING=0
# export USE_LAZY=0

SERVER_WAIT="${SERVER_WAIT:-5}"

if [[ ! -x "$BUILD_DIR/test_compass_ring" ]]; then
  echo "Error: $BUILD_DIR/test_compass_ring not found; build it first."
  exit 1
fi

export REPO_ROOT
export BUILD_DIR
export DATASET

# ========== Section 1: read dataset search defaults from JSON ==========
# Validate input files and read search defaults from config_ring.json.
EVAL_OUT="$(python3 << 'PY'
import json, os, sys
repo = os.environ["REPO_ROOT"]
build = os.environ["BUILD_DIR"]
ds = os.environ["DATASET"]
cfg_path = os.path.join(repo, "tests/config/config_ring.json")
with open(cfg_path) as f:
    j = json.load(f)
if ds not in j:
    print(f"ERR:no_dataset:{ds}", file=sys.stderr)
    sys.exit(1)
c = j[ds]
keys = [
    "index_path", "pq_path", "base_path", "query_path", "gt_path",
]
snap_keys = [
    "buckets_path", "graph_cache_path", "metadata_path", "pos_map_path", "block_map_path",
]
if c.get("integrity"):
    snap_keys.append("hash_path")
missing = []
for k in keys:
    rel = c[k]
    path = os.path.normpath(os.path.join(repo, "..", rel))
    if not os.path.isfile(path):
        missing.append((k, path))
if missing:
    print("ERR:missing_files", file=sys.stderr)
    for k, p in missing:
        print(f"  [{k}] {p}", file=sys.stderr)
    sys.exit(1)
missing_snap = []
for k in snap_keys:
    rel = c[k]
    path = os.path.normpath(os.path.join(repo, "..", rel))
    if not os.path.isfile(path):
        missing_snap.append((k, path))
if missing_snap:
    print("WARN:missing_snap_files", file=sys.stderr)
    for k, p in missing_snap:
        print(f"  [{k}] {p}", file=sys.stderr)
# Match test_compass_ring arguments: efspec, efn, ef_search, ef_upper_steps.
base_path = os.path.normpath(os.path.join(repo, "..", c["base_path"]))
result_dir = os.path.join(repo, "results", ds)
pq_path = os.path.normpath(os.path.join(repo, "..", c["pq_path"]))
print(f"OK {c['ef_spec']} {c['ef_neighbor']} {c['ef_search']} {c['ef_upper_steps']} {result_dir} {pq_path} {1 if missing_snap else 0}")
PY
)"

if [[ "${EVAL_OUT}" == ERR:* ]] || [[ -z "${EVAL_OUT}" ]]; then
  echo "Error: data validation or configuration loading failed; see the Python output above."
  exit 1
fi

read -r _CHECK DEF_EF_SPEC DEF_EF_NEIGHBOR DEF_EF_SEARCH DEF_EF_UPPER DEF_RESULT_DIR DEF_PQ_PATH DEF_SNAP_MISSING <<< "${EVAL_OUT}"
if [[ "${_CHECK}" != "OK" ]]; then
  echo "Error: unexpected configuration output: ${EVAL_OUT}"
  exit 1
fi

if [[ "${DEF_SNAP_MISSING}" == "1" ]]; then
  if [[ "${AUTO_BUILD_SNAP:-1}" == "0" ]]; then
    echo "Error: initial graph ORAM snapshot missing. Run ./script/build_snap_from_dataset.sh ${DATASET}, or unset AUTO_BUILD_SNAP=0 to enable automatic initialization."
    exit 1
  fi
  if [[ ! -x "$BUILD_DIR/compass_init" ]]; then
    echo "Error: $BUILD_DIR/compass_init missing; rebuild it to enable automatic graph ORAM snapshot initialization."
    exit 1
  fi
  echo ">>> Graph ORAM snapshot missing; running compass_init to generate and persist it..."
  (cd "$BUILD_DIR" && ./compass_init "d=$DATASET")
  python3 << 'PY'
import json, os, sys
repo = os.environ["REPO_ROOT"]
build = os.environ["BUILD_DIR"]
ds = os.environ["DATASET"]
cfg_path = os.path.join(repo, "tests/config/config_ring.json")
with open(cfg_path) as f:
    c = json.load(f)[ds]
keys = ["buckets_path", "graph_cache_path", "metadata_path", "pos_map_path", "block_map_path"]
if c.get("integrity"):
    keys.append("hash_path")
missing = []
for k in keys:
    path = os.path.normpath(os.path.join(repo, "..", c[k]))
    if not os.path.isfile(path):
        missing.append((k, path))
if missing:
    print("ERR: compass_init completed but snap files are still missing:", file=sys.stderr)
    for k, p in missing:
        print(f"  [{k}] {p}", file=sys.stderr)
    sys.exit(1)
PY
fi

# ========== Section 2: environment overrides take precedence over JSON DEF_* values ==========
EF_SPEC_VAL="${EF_SPEC:-$DEF_EF_SPEC}"
EFN_VAL="${EFN:-$DEF_EF_NEIGHBOR}"
EF_SEARCH_VAL="${EF_SEARCH:-${ARG_EF_SEARCH:-$DEF_EF_SEARCH}}"
EF_UPPER_STEPS_VAL="${EF_UPPER_STEPS:-$DEF_EF_UPPER}"
TOPK_VAL="${TOPK:-${DEF_TOPK:-}}"

# ========== Section 4: build test_compass_ring arguments ==========
RING_EXTRA_ARGS=(
  "efspec=${EF_SPEC_VAL}"
  "efn=${EFN_VAL}"
  "ef_search=${EF_SEARCH_VAL}"
  "ef_upper_steps=${EF_UPPER_STEPS_VAL}"
)
if [[ "${USE_BATCHING:-}" == "0" ]]; then
  RING_EXTRA_ARGS+=(batch=0)
fi
if [[ "${USE_LAZY:-}" == "0" ]]; then
  RING_EXTRA_ARGS+=(lazy=0)
fi
if [[ -n "${TOPK_VAL}" ]]; then
  RING_EXTRA_ARGS+=("topk=${TOPK_VAL}")
fi
NET_PROFILE_VAL="${NET_PROFILE:-${NETWORK_PROFILE:-none}}"
RING_EXTRA_ARGS+=("net=${NET_PROFILE_VAL}")
TOPK_FILE_VAL="${TOPK_VAL:-10}"
RESULT_CSV_VAL="${RESULT_CSV:-${CSV_RESULT:-${DEF_RESULT_DIR}/top${TOPK_FILE_VAL}.csv}}"
RING_EXTRA_ARGS+=("f_csv=${RESULT_CSV_VAL}")
PQ_ORAM_VAL="${PQ_ORAM:-${USE_PQ_ORAM:-1}}"
SERVER_METADATA_VAL="${SERVER_METADATA:-1}"
PQ_ORAM_PATH_VAL="${PQ_ORAM_PATH:-${DEF_PQ_PATH}}"
PQ_CODES_PER_BLOCK_VAL="${PQ_CODES_PER_BLOCK:-${PQ_ORAM_CODES_PER_BLOCK:--1}}"
PQ_METADATA_CLIENT_VAL="${PQ_METADATA_CLIENT:-0}"
PQ_ORAM_STATE_PATH_VAL="${PQ_ORAM_STATE_PATH:-}"
if [[ -n "${PM_ORAM:-}" ]]; then
  PM_ORAM_VAL="${PM_ORAM}"
else
  case "${DATASET}" in
    sift10M|sift100M) PM_ORAM_VAL=1 ;;
    *) PM_ORAM_VAL=0 ;;
  esac
fi
PM_ENTRIES_PER_BLOCK_VAL="${PM_ENTRIES_PER_BLOCK:-32}"
PM_ORAM_STATE_PATH_VAL="${PM_ORAM_STATE_PATH:-}"
if [[ "${PQ_ORAM_VAL}" == "0" ]]; then
  RING_EXTRA_ARGS+=("pq_oram=0" "server_metadata=${SERVER_METADATA_VAL}")
else
  if [[ ! -f "${PQ_ORAM_PATH_VAL}" ]]; then
    echo "Error: PQ_ORAM=1 but PQ_ORAM_PATH does not exist: ${PQ_ORAM_PATH_VAL}"
    exit 1
  fi
  RING_EXTRA_ARGS+=("pq_oram=1" "server_metadata=${SERVER_METADATA_VAL}" "pq_oram_path=${PQ_ORAM_PATH_VAL}" "pq_codes_per_block=${PQ_CODES_PER_BLOCK_VAL}" "pq_metadata_client=${PQ_METADATA_CLIENT_VAL}")
  if [[ -n "${PQ_ORAM_STATE_PATH_VAL}" ]]; then
    RING_EXTRA_ARGS+=("pq_oram_state_path=${PQ_ORAM_STATE_PATH_VAL}")
  fi
fi
RING_EXTRA_ARGS+=("pm_oram=${PM_ORAM_VAL}" "pm_entries_per_block=${PM_ENTRIES_PER_BLOCK_VAL}")
if [[ -n "${PM_ORAM_STATE_PATH_VAL}" ]]; then
  RING_EXTRA_ARGS+=("pm_oram_state_path=${PM_ORAM_STATE_PATH_VAL}")
fi

echo ">>> Search parameters (JSON defaults with environment overrides):"
echo "    efspec=${EF_SPEC_VAL}  efn=${EFN_VAL}  ef_search=${EF_SEARCH_VAL}  ef_upper_steps=${EF_UPPER_STEPS_VAL}${TOPK_VAL:+  topk=${TOPK_VAL}}"
echo "    net=${NET_PROFILE_VAL} (none/1: no added delay; fast/2: 3Gbps+1ms RTT; slow/3: 400Mbps+80ms RTT)"
echo "    csv=${RESULT_CSV_VAL}"
if [[ "${PQ_ORAM_VAL}" == "0" ]]; then
  echo "    pq_oram=0  server_metadata=${SERVER_METADATA_VAL}"
else
  echo "    pq_oram=1  pq_oram_path=${PQ_ORAM_PATH_VAL}  pq_codes_per_block=${PQ_CODES_PER_BLOCK_VAL}  server_metadata=${SERVER_METADATA_VAL}  pq_metadata_client=${PQ_METADATA_CLIENT_VAL}${PQ_ORAM_STATE_PATH_VAL:+  pq_oram_state_path=${PQ_ORAM_STATE_PATH_VAL}}"
fi
echo "    pm_oram=${PM_ORAM_VAL}  pm_entries_per_block=${PM_ENTRIES_PER_BLOCK_VAL}${PM_ORAM_STATE_PATH_VAL:+  pm_oram_state_path=${PM_ORAM_STATE_PATH_VAL}}"
if [[ "${USE_BATCHING:-}" == "0" ]] || [[ "${USE_LAZY:-}" == "0" ]]; then
  echo "    batch=${USE_BATCHING:-1} lazy=${USE_LAZY:-1} (0 passes batch=0 / lazy=0)"
fi

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

cd "$BUILD_DIR"

# ========== Section 5: launch both endpoints using RING_EXTRA_ARGS from section 4 ==========
echo ">>> Starting server (r=1) ..."
./test_compass_ring "r=1" "d=$DATASET" "p=$PORT" "${RING_EXTRA_ARGS[@]}" &
SERVER_PID=$!

echo ">>> Waiting for server initialization (${SERVER_WAIT}s); adjust SERVER_WAIT as needed."
sleep "$SERVER_WAIT"

echo ">>> Starting client (r=2), n=$N ..."
./test_compass_ring "r=2" "d=$DATASET" "p=$PORT" "n=$N" "${RING_EXTRA_ARGS[@]}"

echo ">>> Done."
