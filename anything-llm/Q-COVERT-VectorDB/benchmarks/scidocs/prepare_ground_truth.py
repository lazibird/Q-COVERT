#!/usr/bin/env python3
"""Generate exact squared-L2 Top-K ground truth with Faiss IndexFlatL2."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import tempfile

import faiss
import numpy as np


DIMENSION = 384
TOP_K = 10


def parse_args() -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cache",
        type=Path,
        default=script_dir / "cache",
        help="Embedding cache directory (default: ./cache)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Ground-truth JSON (default: CACHE/faiss-ground-truth.json)",
    )
    parser.add_argument("--query-batch-size", type=int, default=100)
    parser.add_argument(
        "--faiss-threads",
        type=int,
        default=0,
        help="Faiss OpenMP threads; 0 keeps the Faiss default",
    )
    return parser.parse_args()


def read_ids(path: Path) -> list[str]:
    return [line for line in path.read_text(encoding="utf-8").splitlines() if line]


def load_matrix(path: Path, rows: int, dimension: int) -> np.memmap:
    expected_bytes = rows * dimension * np.dtype("<f4").itemsize
    actual_bytes = path.stat().st_size
    if actual_bytes != expected_bytes:
        raise ValueError(f"{path} is {actual_bytes} bytes; expected {expected_bytes}")
    return np.memmap(path, mode="r", dtype="<f4", shape=(rows, dimension))


def write_json_atomic(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    file_descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    try:
        with os.fdopen(file_descriptor, "w", encoding="utf-8") as output:
            json.dump(value, output, ensure_ascii=False, indent=2)
            output.write("\n")
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def main() -> None:
    args = parse_args()
    if args.query_batch_size <= 0:
        raise ValueError("--query-batch-size must be positive")
    if args.faiss_threads < 0:
        raise ValueError("--faiss-threads must be non-negative")

    cache = args.cache.resolve()
    output_path = (args.output or cache / "faiss-ground-truth.json").resolve()
    manifest = json.loads((cache / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("status") != "complete" or manifest.get("dimension") != DIMENSION:
        raise ValueError("Embedding cache is incomplete or is not 384-dimensional")

    corpus_ids = read_ids(cache / manifest["files"]["corpus_ids"])
    query_ids = read_ids(cache / manifest["files"]["query_ids"])
    if len(corpus_ids) != manifest["corpus_count"]:
        raise ValueError("Corpus ID count differs from manifest")
    if len(query_ids) != manifest["query_count"]:
        raise ValueError("Query ID count differs from manifest")

    corpus = load_matrix(
        cache / manifest["files"]["corpus_vectors"], len(corpus_ids), DIMENSION
    )
    queries = load_matrix(
        cache / manifest["files"]["query_vectors"], len(query_ids), DIMENSION
    )
    if args.faiss_threads > 0:
        faiss.omp_set_num_threads(args.faiss_threads)

    print(f"Building Faiss IndexFlatL2 over {len(corpus_ids)} vectors ...", flush=True)
    index = faiss.IndexFlatL2(DIMENSION)
    index.add(corpus)
    actual_top_k = min(TOP_K, len(corpus_ids))
    results: dict[str, list[dict[str, object]]] = {}

    for begin in range(0, len(query_ids), args.query_batch_size):
        end = min(len(query_ids), begin + args.query_batch_size)
        distances, indices = index.search(queries[begin:end], actual_top_k)
        for offset, query_id in enumerate(query_ids[begin:end]):
            hits = []
            for rank in range(actual_top_k):
                corpus_index = int(indices[offset, rank])
                if corpus_index < 0:
                    continue
                hits.append(
                    {
                        "id": corpus_ids[corpus_index],
                        "distance": float(distances[offset, rank]),
                    }
                )
            results[query_id] = hits
        print(f"Faiss exact search: {end}/{len(query_ids)} queries", flush=True)

    report = {
        "schema_version": 1,
        "cache_fingerprint": manifest["fingerprint"],
        "model": manifest["model"],
        "dimension": DIMENSION,
        "metric": "squared_l2",
        "faiss_index": "IndexFlatL2",
        "top_k": TOP_K,
        "corpus_count": len(corpus_ids),
        "query_count": len(query_ids),
        "results": results,
    }
    write_json_atomic(output_path, report)
    print(f"Faiss ground truth complete: {output_path}", flush=True)


if __name__ == "__main__":
    main()
