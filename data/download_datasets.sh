#!/usr/bin/env bash
# Download shared dataset sources; extraction and GT generation are handled by prepare_datasets.py.
set -euo pipefail
DATA_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRY_RUN=0
DATASETS=()
usage() {
  cat <<'HELP'
Usage: download_datasets.sh [DATASET ...] [--data-root DIR] [--dry-run]
Datasets: sift, sift1b (alias: bigann), msmarco1m, deep1m, all (default).
Create dataset directories under data/ beside this script, from any launch directory.
Skip completed nonempty files and resume interrupted downloads from *.part.
Download only base/query sources, plus SIFT's official GT in its archive.
Run prepare_datasets.py afterwards to extract files, create subsets, and compute GT.
HELP
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --data-root)
      if [[ $# -lt 2 || -z "$2" ]]; then echo "--data-root requires a directory" >&2; exit 2; fi
      DATA_ROOT="$2"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    sift|sift1b|bigann|msmarco1m|deep1m|all) DATASETS+=("$1"); shift ;;
    *) echo "Unknown option or dataset: $1" >&2; usage >&2; exit 2 ;;
  esac
done
if [[ ${#DATASETS[@]} -eq 0 || " ${DATASETS[*]} " == *" all "* ]]; then
  DATASETS=(sift sift1b msmarco1m deep1m)
fi
if [[ "$DRY_RUN" -eq 0 ]]; then
  command -v curl >/dev/null || { echo "curl is required; see the root README" >&2; exit 1; }
fi
TEXMEX="ftp://ftp.irisa.fr/local/texmex/corpus"
MSMARCO="https://storage.googleapis.com/compass_osdi/dataset/msmarco_bert"
PUBLIC_KEY="https://yadi.sk/d/11eDCm7Dsn9GA"
YANDEX_API="https://cloud-api.yandex.net/v1/disk/public/resources/download"

download() {
  local source_url="$1" destination="$2" yandex_path="${3:-}" download_url
  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '%s -> %s\n' "$source_url" "$destination"
    return
  fi
  if [[ -s "$destination" ]]; then
    printf 'Already present: %s\n' "$destination"
    return
  fi
  mkdir -p "$(dirname "$destination")"
  download_url="$source_url"
  if [[ -n "$yandex_path" ]]; then
    command -v python3 >/dev/null || { echo "python3 is required; see the root README" >&2; exit 1; }
    # Signed links expire, so resolve a new one before resuming a Yandex download.
    download_url="$(curl --fail --silent --show-error --location --retry 3 --get \
      --data-urlencode "public_key=$PUBLIC_KEY" --data-urlencode "path=/$yandex_path" "$YANDEX_API" |
      python3 -c 'import json, sys
value = json.load(sys.stdin).get("href")
if not isinstance(value, str) or not value.startswith("https://"):
    sys.exit("Yandex did not return an HTTPS download URL")
print(value)')"
  fi
  printf 'Downloading: %s\n' "$destination"
  curl --fail --location --retry 3 --connect-timeout 30 --continue-at - \
    --output "$destination.part" "$download_url"
  [[ -s "$destination.part" ]] || { echo "Empty download: $destination" >&2; return 1; }
  mv -- "$destination.part" "$destination"
}

declare -A SEEN=()
for dataset in "${DATASETS[@]}"; do
  [[ "$dataset" == bigann ]] && dataset=sift1b
  [[ -n "${SEEN[$dataset]:-}" ]] && continue
  SEEN[$dataset]=1
  case "$dataset" in
    sift)
      if [[ "$DRY_RUN" -eq 0 && -s "$DATA_ROOT/sift/sift_base.fvecs" &&
            -s "$DATA_ROOT/sift/sift_query.fvecs" && -s "$DATA_ROOT/sift/sift_groundtruth.ivecs" ]]; then
        printf 'Already present: %s\n' "$DATA_ROOT/sift (extracted)"
      else
        download "$TEXMEX/sift.tar.gz" "$DATA_ROOT/sift/sift.tar.gz"
      fi ;;
    sift1b)
      for name in bigann_base.bvecs bigann_query.bvecs; do
        if [[ "$DRY_RUN" -eq 0 && -s "$DATA_ROOT/sift1B/$name" ]]; then
          printf 'Already present: %s\n' "$DATA_ROOT/sift1B/$name"
        else
          download "$TEXMEX/$name.gz" "$DATA_ROOT/sift1B/$name.gz"
        fi
      done ;;
    msmarco1m)
      for name in passages.fvecs queries.fvecs; do
        download "$MSMARCO/$name" "$DATA_ROOT/msmarco1m/$name"
      done ;;
    deep1m)
      for name in base/base_00 deep1B_queries.fvecs; do
        download "$PUBLIC_KEY:/$name" "$DATA_ROOT/deep1M/source/$name" "$name"
      done ;;
  esac
done
