#!/usr/bin/env python3
"""Train Q-COVERT IVF centroids and nearest assignments with Python Faiss."""

import argparse
import math
import os
import struct
import sys
import time


SNAPSHOT_MAGIC = 0x31534351
RECORD_MAGIC = 0x31564351
FORMAT_VERSION = 1
DIMENSION = 384
SNAPSHOT_HEADER = struct.Struct("<IIIQQ")
RECORD_HEADER = struct.Struct("<IIIIIQ")


def status(message):
    print(f"[faiss-worker] {message}", flush=True)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--clusters", type=int, required=True)
    parser.add_argument("--niter", type=int, default=25)
    parser.add_argument("--nredo", type=int, default=2)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--threads", type=int, default=0)
    return parser.parse_args()


def read_exact(handle, size, description):
    data = handle.read(size)
    if len(data) != size:
        raise RuntimeError(f"truncated {description}")
    return data


def load_snapshot(path, np):
    with open(path, "rb") as handle:
        magic, version, dimension, vector_count, last_sequence = SNAPSHOT_HEADER.unpack(
            read_exact(handle, SNAPSHOT_HEADER.size, "snapshot header")
        )
        if magic != SNAPSHOT_MAGIC or version != FORMAT_VERSION or dimension != DIMENSION:
            raise RuntimeError("invalid Q-COVERT snapshot header")
        if vector_count == 0:
            raise RuntimeError("cannot cluster an empty generation")
        vectors = np.empty((vector_count, dimension), dtype=np.float32)
        maximum_sequence = 0
        for index in range(vector_count):
            header = RECORD_HEADER.unpack(read_exact(handle, RECORD_HEADER.size, "record header"))
            record_magic, record_version, record_dimension, id_bytes, metadata_bytes, sequence = header
            if (
                record_magic != RECORD_MAGIC
                or record_version != FORMAT_VERSION
                or record_dimension != dimension
                or id_bytes <= 0
                or id_bytes > 512
                or metadata_bytes > 1024 * 1024
                or sequence <= 0
            ):
                raise RuntimeError(f"invalid vector record header at row {index}")
            read_exact(handle, id_bytes, "vector id")
            vector_bytes = read_exact(handle, dimension * 4, "float32 vector")
            vectors[index] = np.frombuffer(vector_bytes, dtype="<f4", count=dimension)
            read_exact(handle, metadata_bytes, "metadata")
            maximum_sequence = max(maximum_sequence, sequence)
        if handle.read(1):
            raise RuntimeError("snapshot contains trailing bytes")
        if maximum_sequence > last_sequence:
            raise RuntimeError("snapshot record sequence exceeds declared boundary")
    if not np.isfinite(vectors).all():
        raise RuntimeError("snapshot contains non-finite vector values")
    return np.ascontiguousarray(vectors)


def write_fvecs(path, values, np, chunk_rows=65536):
    values = np.asarray(values, dtype=np.float32, order="C")
    with open(path, "wb") as handle:
        for start in range(0, values.shape[0], chunk_rows):
            rows = values[start : start + chunk_rows]
            encoded = np.empty((rows.shape[0], rows.shape[1] + 1), dtype=np.float32)
            encoded.view(np.uint32)[:, 0] = rows.shape[1]
            encoded[:, 1:] = rows
            encoded.tofile(handle)


def write_ivecs(path, values, np, chunk_rows=65536):
    values = np.asarray(values, dtype=np.uint32).reshape(-1, 1)
    with open(path, "wb") as handle:
        for start in range(0, values.shape[0], chunk_rows):
            rows = values[start : start + chunk_rows]
            encoded = np.empty((rows.shape[0], 2), dtype=np.uint32)
            encoded[:, 0] = 1
            encoded[:, 1] = rows[:, 0]
            encoded.tofile(handle)


def main():
    args = parse_args()
    try:
        import faiss
        import numpy as np
    except ImportError as error:
        raise RuntimeError(
            "Python Faiss dependencies are unavailable; activate .venv and install numpy faiss-cpu"
        ) from error

    if args.clusters <= 0 or args.niter <= 0 or args.nredo <= 0 or args.threads < 0:
        raise ValueError("clusters, niter and nredo must be positive; threads must be non-negative")
    if args.threads:
        faiss.omp_set_num_threads(args.threads)
    status(f"loading snapshot: {args.snapshot}")
    started = time.monotonic()
    vectors = load_snapshot(args.snapshot, np)
    if args.clusters > vectors.shape[0]:
        raise ValueError("cluster count exceeds vector count")
    status(
        f"training centroids: vectors={vectors.shape[0]:,} dimension={vectors.shape[1]} "
        f"clusters={args.clusters:,} niter={args.niter} nredo={args.nredo} threads={args.threads or 'faiss-default'}"
    )

    max_points_per_centroid = int(math.ceil(vectors.shape[0] / args.clusters))
    kmeans = faiss.Kmeans(
        vectors.shape[1],
        args.clusters,
        niter=args.niter,
        nredo=args.nredo,
        seed=args.seed,
        verbose=False,
        spherical=False,
        min_points_per_centroid=1,
        max_points_per_centroid=max_points_per_centroid,
    )
    kmeans.train(vectors)
    centroids = np.ascontiguousarray(kmeans.centroids, dtype=np.float32)

    status("computing exact nearest-centroid assignments")
    assignment_index = faiss.IndexFlatL2(vectors.shape[1])
    assignment_index.add(centroids)
    squared_distances, assignments = assignment_index.search(vectors, 1)
    distances = np.sqrt(np.maximum(squared_distances, 0.0), dtype=np.float32)

    os.makedirs(args.output_dir, exist_ok=True)
    write_fvecs(os.path.join(args.output_dir, "faiss_centroids.fvecs"), centroids, np)
    write_ivecs(os.path.join(args.output_dir, "faiss_assignments.ivecs"), assignments, np)
    write_fvecs(os.path.join(args.output_dir, "faiss_distances.fvecs"), distances, np)
    status(f"complete in {time.monotonic() - started:.2f}s")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"[faiss-worker] failed: {error}", file=sys.stderr, flush=True)
        raise
