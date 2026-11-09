# Q-COVERT Vector Database

Q-COVERT is a research vector database that separates a trusted query service from an untrusted storage service. It is integrated with the Q-COVERT vector database provider in the parent AnythingLLM project. It supports 384-dimensional vector search, persistent collections, online updates, resumable bulk initialization, and generation-based indexing.

```text
AnythingLLM / API client
    → qcovert_query (trusted: vectors, metadata, keys, ORAM client state)
    → qcovert_storage (untrusted: encrypted ORAM trees)
```

The storage service holds AES-256-GCM authenticated encrypted blocks. ClusterORAM and RingORAM obscure logical access patterns from the storage side. The query service sees plaintext and must be trusted; this is not an end-to-end zero-knowledge system. This implementation is a research prototype, not a production security certification.

## Repository and data

| Path | Purpose |
| --- | --- |
| `src/`, `include/`, `core/` | Services, persistence, search, and ORAM/ANN implementation |
| `tests/` | Collection, recovery, security, and ORAM tests |
| `scripts/` | PDF preparation, embedding, summaries, and import scripts |
| `benchmarks/scidocs/` | Standalone SCIDOCS benchmark code |
| `../server/utils/vectorDbProviders/qcovert/` | AnythingLLM adapter |
| `qcovert-data/` | Trusted runtime state, including plaintext WAL and keys; ignored by Git |
| `storage/` | Encrypted server-side trees; ignored by Git |
| `pdf-corpus-v1/`, `rebuild-backups/` | Local corpus and backups; ignored by Git |

This vector database is bundled with the AnythingLLM application under
`anything-llm/Q-COVERT-VectorDB/`. Datasets, databases, virtual environments,
and build artifacts stay local and are ignored by Git. Some optional historical
corpus scripts still contain machine-specific paths and require adjustment on
other hosts. Third-party code in `core/` retains its documentation and licenses.

## Requirements and build

Use Linux on an AVX2-capable x86-64 CPU, a C++17 compiler, CMake 3.20+, Python 3, OpenSSL development headers, and Node.js/Yarn for AnythingLLM and the JavaScript scripts. On Ubuntu/Debian:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libssl-dev python3 python3-venv python3-pip curl poppler-utils
```

From this directory:

```bash
python3 -m venv .venv
./.venv/bin/python -m pip install numpy faiss-cpu
./.venv/bin/python -c 'import faiss, numpy; print("Dependencies available")'
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target qcovert_storage qcovert_query -j"$(nproc)"
```

To build and run the C++ tests:

```bash
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## Run with AnythingLLM

The parent [AnythingLLM_Q-COVERT.md](../AnythingLLM_Q-COVERT.md) contains full setup and troubleshooting instructions. For development, configure `../server/.env.development` with your own credentials:

```dotenv
VECTOR_DB="qcovert"
QCOVERT_ENDPOINT="http://127.0.0.1:8080"
QCOVERT_API_KEY="your-qcovert-api-key"
QCOVERT_COLLECTION_PREFIX="anythingllm_"
QCOVERT_CANDIDATE_FACTOR="1"
EMBEDDING_ENGINE="native"
EMBEDDING_MODEL_PREF="MintplexLabs/multilingual-e5-small"
```

Set a chat model and its credentials separately. The query service and AnythingLLM must use the same Q-COVERT API key. Never commit credentials. `server/.env.development` is Git-ignored.

Start storage, then query, then AnythingLLM in separate terminals:

```bash
cd /path/to/Q-COVERT/anything-llm/Q-COVERT-VectorDB
./build/qcovert_storage --port 19091 --root ./storage --worker-threads 16
```

```bash
cd /path/to/Q-COVERT/anything-llm/Q-COVERT-VectorDB
set -a
source ../server/.env.development
set +a
./build/qcovert_query --port 8080 --root ./qcovert-data \
  --storage-host 127.0.0.1 --storage-port 19091 --worker-threads 16 \
  --python ./.venv/bin/python --faiss-threads 0 --kmeans-niter 25 --kmeans-nredo 2
```

```bash
cd /path/to/Q-COVERT/anything-llm
yarn dev
```

Open `http://localhost:3000`. Replace `/path/to/Q-COVERT` with your checkout
location. These commands use new local data directories. To reuse an existing
database, explicitly provide its matching storage and query roots and the
corresponding AnythingLLM application database. Use only one query writer for a
given trusted root.

Check the authenticated API from a shell with `QCOVERT_API_KEY` set:

```bash
curl --noproxy '*' -sS -H "Authorization: Bearer ${QCOVERT_API_KEY}" http://127.0.0.1:8080/v1/health
```

The response reports `status: ok` and dimension `384`. Stop AnythingLLM first, then interrupt `qcovert_query` and wait for its graceful shutdown message, then interrupt `qcovert_storage`. Query shutdown checkpoints the active ORAM runtime.

## Collections and search

- Collection names contain 1–95 ASCII letters, digits, `_` or `-`. Vector IDs support UTF-8, are 1–512 bytes, and cannot contain control characters.
- Vectors contain exactly 384 finite values and are stored as float32. Search reports squared L2 distance and metadata. Reusing an ID updates its current value.
- AnythingLLM maps a workspace slug to `QCOVERT_COLLECTION_PREFIX + slug`; for example, `papers` maps to `anythingllm_papers` with the prefix above. The adapter expects retrievable text in `metadata.text`.
- Embedding model, normalization, pooling, and document/query prefixes must match between ingestion and queries. The provided E5 model uses `passage: ` and `query: ` respectively.
- `top_k` defaults to 10 and accepts 1–1000. `candidate_factor` defaults to 0.5 and accepts `(0, 100]`. The service computes `nprobe = min(cluster_count, 2 * top_k + floor(cluster_count / 25))` and `k2 = ceil(2 * top_k + top_k * nprobe * candidate_factor)`.
- Fewer than 10,000 vectors use one cluster; otherwise the generation uses `ceil(sqrt(N))` clusters. Online upserts can trigger generation at 10,000-vector boundaries. Bulk initialization creates one generation after the full upload.
- Deleting individual vectors is currently a **no-op**. Deleting a collection removes its data and associated state; do not confuse a successful no-op response with deletion.

## API and bulk initialization

Except when explicitly started in unauthenticated local-debug mode, the HTTP API requires `Authorization: Bearer <API_KEY>`. Core endpoints are:

| Method and path | Purpose |
| --- | --- |
| `GET /v1/health`, `GET /v1/stats` | Health and global statistics |
| `PUT /v1/collections/{name}` | Create or obtain a compatible collection |
| `GET /v1/collections/{name}` | Collection manifest |
| `GET /v1/collections/{name}/stats` | Generation and capacity statistics |
| `POST /v1/collections/{name}/vectors` | Online batch upsert |
| `POST /v1/collections/{name}/search` | Search an active generation |
| `POST /v1/collections/{name}/generations` | Start an asynchronous generation |
| `GET /v1/collections/{name}/jobs/{job_id}` | Generation status |
| `POST /v1/collections/{name}/initializations` | Create or resume bulk initialization of an empty collection |
| `GET /v1/collections/{name}/initializations/{id}` | Initialization progress |
| `PUT /v1/collections/{name}/initializations/{id}/batches/{index}` | Upload a numbered batch with `X-QCovert-Content-SHA256` |
| `POST /v1/collections/{name}/initializations/{id}/finalize` | Validate the upload and build a generation |
| `DELETE /v1/collections/{name}/initializations/{id}` | Abort an uploading initialization |
| `POST/GET /v1/collections/{name}/backups` | Create/list named backups |
| `POST /v1/collections/{name}/restore` | Restore a backup |
| `DELETE /v1/collections/{name}` | Delete a complete collection |

Bulk initialization accepts only an empty collection. Batches are ordered and idempotent; the server records received counts and checksums so uploads can resume. Finalization builds one generation after all vectors are uploaded. Wait for `succeeded` and an active generation before searching. A building job must finish; a failed finalization can be retried. Existing active generations remain searchable during a separate generation rebuild.

Trusted persistence uses a checksummed append-only vector WAL, collection manifests, snapshots, keys, and ORAM client state. Recovery discards incomplete trailing batches and reports committed-data checksum failures. The storage root contains encrypted server-side trees. Both roots are needed to restore a database.

## Preparing and importing PDF papers

For a new text corpus, put PDFs in `pdf-corpus-v1/source/` and run:

```bash
python3 scripts/prepare_pdf_corpus.py --source pdf-corpus-v1/source --output pdf-corpus-v1
node scripts/embed_pdf_corpus.mjs --output pdf-corpus-v1
```

The preparation script extracts pages with Poppler and creates stable IDs based on the source hash and processing version. Its default target is 700 characters with 100 characters of overlap. It records extraction problems for review; it does not automatically OCR every problematic page. Check reading order, formulas, tables, image-only pages, and replacement characters before importing.

| Output | Contents |
| --- | --- |
| `documents.jsonl` | Paper IDs, titles, source paths, hashes, and document metadata |
| `pages.jsonl` | Extracted text and original PDF page number per page |
| `chunks.jsonl` | Stable chunk IDs, original text, embedding text, and citation metadata |
| `vectors.ids`, `vectors.f32` | Row-aligned IDs and little-endian 384-dimensional float32 vectors |
| `manifest.json` | Counts, hashes, processing version, embedding configuration, and fingerprint |
| `rejected.jsonl` and progress files | Problems requiring review and resumable processing status |

Validate hashes, counts, vector dimensions, finite values, title/path metadata, and representative pages before loading. The PDF loader is configured for the prepared SP2026 corpus and refuses incompatible manifests or an unexplained nonempty collection. It can accept the specific recorded replacement-character warnings with `--allow-warnings`; inspect the report first.

```bash
node scripts/load_pdf_qcovert.mjs --data /path/to/pdf-corpus-v1 --validate-only --allow-warnings
node scripts/load_pdf_qcovert.mjs --data /path/to/pdf-corpus-v1 --allow-warnings
```

The loader uses resumable bulk initialization and records progress. It does not delete an existing collection. A matching AnythingLLM workspace slug and collection prefix are required. The historical SP2026 run contained 235 PDFs, 4,536 pages and 42,157 original-text chunks; these are dataset-specific figures, not general defaults. The original files, local corpus and generated vectors are excluded from Git.

The optional `summarize_pdf_corpus.mjs` and `prepare_summary_index.py` scripts build a separate machine-generated paper-level index; `embed_pdf_corpus.mjs` embeds it locally and `load_pdf_lance.mjs --summary` imports the comparison table. Summaries must be checked against the source PDF and can omit or misstate information. The current summary retrieval experiment produced irrelevant results for some questions, so it should not be treated as validated ground truth. Local PDF reading through the AnythingLLM agent uses `pdf-corpus-v1/source` in this directory by default. Set `QCOVERT_PDF_SOURCE_DIR` in `../server/.env.development` to an absolute path when reusing an existing corpus. It extracts text rather than interpreting figures.

## SCIDOCS benchmark

The standalone scripts in `benchmarks/scidocs/` use the same embeddings for LanceDB and Q-COVERT. They rely on AnythingLLM's installed Node dependencies and local embedding model. Set `QCOVERT_API_KEY` in the environment; no fixed benchmark key is required. Default paths are machine-specific and may need adjustment.

```bash
cd benchmarks/scidocs
node prepare_embeddings.mjs
../../.venv/bin/python prepare_ground_truth.py
node load_lancedb.mjs
node load_qcovert.mjs
node benchmark.mjs --database both
```

`prepare_ground_truth.py` uses Faiss `IndexFlatL2` for exact top-10 reference results. Embedding preparation and Q-COVERT bulk loading are resumable. `--recreate` on a loader deletes its benchmark collection/table and should only be used when that data can be discarded. Benchmark caches, datasets and generated results are Git-ignored.

One sequential SCIDOCS run on 2026-08-24 used 25,657 document vectors, 1,000 pre-embedded queries, top-10 search and no similarity filter. It bypassed AnythingLLM. Its recorded results were:

| Metric | LanceDB exact baseline | Q-COVERT |
| --- | ---: | ---: |
| Timed queries successful | 3,000 / 3,000 | 3,000 / 3,000 |
| Recall@10 against exact top-10 | 100% | 95.11% |
| nDCG@10 against exact ranking | 100% | 96.81% |
| Top-1 agreement | 100% | 96.50% |
| Complete top-10 set agreement | 100% | 68.30% |
| Mean / p95 / p99 database latency | 38.79 / 41.08 / 46.14 ms | 191.13 / 210.96 / 220.07 ms |

These numbers describe that run, not a production SLA or semantic relevance benchmark. They exclude embedding, reranking, model generation, concurrency, long-term operation and end-to-end RAG quality. Define an acceptable latency before testing; `benchmark.mjs` supports `--acceptable-p95-ms` for an explicit threshold.

## Operations and limitations

- Keep the query service, its WAL, metadata, keys, and backups in a trusted environment. Restrict access to the storage service as well.
- The HTTP service does not provide built-in TLS. Use appropriate network isolation and TLS/mTLS when crossing hosts. `--allow-unauthenticated` is for isolated local debugging only.
- No cross-process writer lease or key rotation is implemented. Do not run multiple query writers against the same root. Treat exposed generation keys as a data exposure and rebuild the affected collection.
- Storage protocol version 3 uses AES-256-GCM; older protocol-v2/CBC storage is incompatible and requires rebuilding test state.
- If the API returns `401`, check the configured key. A `409 generation_not_ready` means the collection has no active searchable generation. If search is empty, inspect collection statistics, embedding compatibility, candidate settings, and application similarity filtering.
- A source PDF path in retrieved metadata is a local path, not a portable file download. Page numbers and extracted text require review, especially for scanned or complex PDFs.
- Production use requires further work on transport security, authorization, quotas, auditing, fault injection, concurrency and end-to-end retrieval evaluation. Confirm the license of project and third-party code before redistribution.
