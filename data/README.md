# Shared datasets

Install dependencies using [QUICKSTART.md](../QUICKSTART.md#1-install-dependencies). Run the commands
below from the repository root. The two entry points resolve their default
`data/` directory relative to the scripts, regardless of the launch directory.
Downloaded files and generated datasets are excluded from Git.

| Dataset | Source | Preparation and GT |
| --- | --- | --- |
| sift | [TEXMEX](http://corpus-texmex.irisa.fr/) | Extract original SIFT1M vectors and official GT |
| sift10M/sift100M | SIFT1B/BIGANN from [TEXMEX](http://corpus-texmex.irisa.fr/) | First 10M/100M base vectors; recompute exact L2 GT |
| MSMARCO1M | [Compass](https://github.com/Clive2312/compass) | Uniform 1M passage sample, seed 42; recompute exact L2 GT |
| DEEP1M | [deep1b_gt](https://github.com/matsui528/deep1b_gt#bonus-deep1m) | First 1M DEEP1B base vectors; recompute exact L2 GT |

## Download

```bash
# Download all sources, or select individual datasets
bash data/download_datasets.sh
bash data/download_datasets.sh sift sift1b
bash data/download_datasets.sh msmarco1m deep1m
# Preview requests without downloading or creating directories
bash data/download_datasets.sh --dry-run
```

Options: `--data-root DIR`, `--dry-run`, `--help`. Dataset selectors are `sift`,
`sift1b` (alias `bigann`), `msmarco1m`, `deep1m`, and `all` (default).
Directories are created automatically. Completed nonempty files are skipped;
interrupted downloads resume from `.part` files when the same command is rerun.
The downloader requires `curl`; DEEP link resolution also requires `python3`.

TEXMEX's file links use FTP. SIFT downloads `sift.tar.gz`; BIGANN downloads only
`bigann_base.bvecs.gz` and `bigann_query.bvecs.gz`. MSMARCO downloads Compass's
original `passages.fvecs` and `queries.fvecs`. DEEP downloads `base/base_00`
(about 10.6 GB, larger than the final 1M prefix) and `deep1B_queries.fvecs`
from the public Yandex disk referenced by `deep1b_gt`. No learning shards or
external GT are downloaded for the derived datasets.

## Prepare

```bash
# Prepare all datasets, including decompression and exact L2 GT generation
.venv/bin/python data/prepare_datasets.py
# Or select datasets
.venv/bin/python data/prepare_datasets.py sift
.venv/bin/python data/prepare_datasets.py sift10m sift100m
.venv/bin/python data/prepare_datasets.py msmarco1m deep1m --faiss-threads 8
```

SIFT keeps its original vectors and official GT. All other datasets retain
**all original queries** and recompute exact L2 top-100 GT against the newly
written subset; GT IDs refer to positions in that subset. Published DEEP GT
is ignored. SIFT prefixes remain uint8; MSMARCO and DEEP remain float32, with
DEEP retaining its original base order and 96 dimensions.

Options (`--help` lists defaults):

- `--data-root DIR`: shared input/output root, default `data/` beside the script.
- `--sift10m-count 10M`, `--sift100m-count 100M`, `--msmarco-count 1M`,
  `--deep-count 1M`: subset sizes; positive integers and K/M suffixes are accepted.
- `--seed 42`: MSMARCO uniform sampling seed; sampled source IDs are sorted.
- `--k 100`: GT width for derived datasets, at most the selected base size.
- `--base-batch 100000`, `--query-batch 100`: copy/search block sizes.
- `--faiss-threads 0`: FAISS OpenMP threads; 0 keeps the library default.

`all` is the default; `sift1b`/`bigann` selects both SIFT prefixes. Archives are
expanded automatically and retained. Existing uncompressed BIGANN bvecs and
legacy uint8 `sift1B/base.bin` / `query.bin` are also supported. DEEP may use
`source/deep1M_base.fvecs` instead of `source/base/base_00`; its reader tolerates
an incomplete trailing shard record beyond the requested prefix.

Each rerun regenerates the selected derived datasets and GT. Outputs are staged
before replacing existing files; original downloaded vectors are preserved.
Rebuild scheme indexes after changing a subset. Exact GT uses FAISS IndexFlatL2
on disjoint base blocks and merges their top-k results, bounding index memory
by `--base-batch`. SIFT100M still requires substantial disk space and compute.

## Directory layout

```text
data/
|-- download_datasets.sh
|-- prepare_datasets.py
|-- vector_io.py                    # Shared format helpers
|-- sift/
|   |-- sift.tar.gz
|   |-- sift_base.fvecs
|   |-- sift_query.fvecs
|   `-- sift_groundtruth.ivecs
|-- sift1B/                        # Removable after SIFT10M and SIFT100M preparation
|   |-- bigann_base.bvecs.gz        # Downloaded sources; archives retained
|   |-- bigann_query.bvecs.gz
|   |-- bigann_base.bvecs           # Created during preparation
|   `-- bigann_query.bvecs
|-- sift10M/                       # sift100M/ uses corresponding filenames
|   |-- base.bin                   # uint8 prefix
|   |-- sift10m_query.fvecs
|   `-- sift10m_groundtruth.ivecs   # Recomputed exact L2 GT
|-- msmarco1m/
|   |-- passages.fvecs
|   |-- queries.fvecs
|   |-- base.bin                   # float32 sample
|   |-- query.bin                  # float32 original queries
|   `-- gt.bin                     # Recomputed exact L2 GT, uint32 IDs
`-- deep1M/
    |-- source/                    # Removable after DEEP1M preparation
    |   |-- base/base_00
    |   `-- deep1B_queries.fvecs
    |-- base.bin                   # float32 prefix
    |-- deep1m_query.fvecs
    `-- deep1m_groundtruth.ivecs    # Recomputed exact L2 GT
```

After successful preparation, `sift1B/` can be deleted once both `sift10M/` and
`sift100M/` contain their base, query, and GT files; `deep1M/source/` can be
deleted once the three processed DEEP1M files are complete. Subset experiments
use these processed files. Download the sources again before regenerating or
changing these subsets. 

Preserve directory capitalization. Header-bin files have one uint32
`(count, dimension)` header; vecs files have a dimension header per row.
Optional SIFT training files, `.part` files, and scheme indexes are omitted.
Scheme-specific artifacts remain under `prepared/ps/`, `prepared/compass/`,
and `prepared/bison/`; CO may create indexes and ORAM caches beside datasets.
