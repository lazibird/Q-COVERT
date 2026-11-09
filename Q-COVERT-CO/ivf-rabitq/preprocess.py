#!/usr/bin/env python3

import argparse
from collections import deque
import glob
import json
import math
import os
import struct
import sys
import time

np = None


def format_duration(seconds):
    if seconds is None or seconds < 0 or seconds == float("inf"):
        return "--"
    seconds = int(seconds + 0.5)
    hours = seconds // 3600
    minutes = (seconds % 3600) // 60
    secs = seconds % 60
    if hours:
        return f"{hours}h{minutes:02d}m{secs:02d}s"
    if minutes:
        return f"{minutes}m{secs:02d}s"
    return f"{secs}s"


def log_status(message):
    print(f"[status] {message}", flush=True)


def apply_cpu_thread_limit(args):
    threads = int(args.cpu_threads)
    if threads <= 0:
        return

    thread_text = str(threads)
    for name in (
        "OMP_NUM_THREADS",
        "OMP_THREAD_LIMIT",
        "OPENBLAS_NUM_THREADS",
        "MKL_NUM_THREADS",
        "NUMEXPR_NUM_THREADS",
        "VECLIB_MAXIMUM_THREADS",
        "BLIS_NUM_THREADS",
    ):
        os.environ[name] = thread_text
    os.environ["OMP_DYNAMIC"] = "FALSE"
    os.environ["MKL_DYNAMIC"] = "FALSE"
    log_status(f"cpu thread limit: {threads}")


class ProgressReporter:
    def __init__(self, label, total, min_interval=2.0):
        self.label = label
        self.total = max(0, int(total))
        self.min_interval = min_interval
        self.start_time = time.time()
        self.last_time = 0.0
        self.last_width = 0

    def update(self, done, force=False):
        done = min(max(0, int(done)), self.total)
        now = time.time()
        if not force and done < self.total and now - self.last_time < self.min_interval:
            return

        elapsed = now - self.start_time
        rate = done / elapsed if elapsed > 0 and done > 0 else 0.0
        eta = (self.total - done) / rate if rate > 0 and done < self.total else 0.0
        percent = (100.0 * done / self.total) if self.total else 100.0
        rate_text = f"{rate:,.0f} vec/s" if rate > 0 else "-- vec/s"
        line = (
            f"[status] {self.label}: {done:,} / {self.total:,} "
            f"({percent:5.1f}%) elapsed {format_duration(elapsed)} "
            f"eta {format_duration(eta)} rate {rate_text}"
        )
        padding = " " * max(0, self.last_width - len(line))
        sys.stdout.write("\r" + line + padding)
        sys.stdout.flush()
        self.last_width = len(line)
        self.last_time = now

    def done(self):
        self.update(self.total, force=True)
        sys.stdout.write("\n")
        sys.stdout.flush()


def require_numpy():
    global np
    if np is None:
        try:
            import numpy as _np
        except ImportError as exc:
            raise RuntimeError("numpy is required for preprocessing") from exc
        np = _np
    return np


def read_fvecs(filename, c_contiguous=True):
    np = require_numpy()
    fv = np.fromfile(filename, dtype=np.float32)
    if fv.size == 0:
        return np.zeros((0, 0), dtype=np.float32)
    dim = fv.view(np.int32)[0]
    if dim <= 0:
        raise ValueError(f"invalid fvecs dimension in {filename}")
    fv = fv.reshape(-1, 1 + dim)
    if not np.all(fv.view(np.int32)[:, 0] == dim):
        raise IOError("non-uniform vector sizes in " + filename)
    fv = fv[:, 1:]
    return fv.copy() if c_contiguous else fv


def read_ivecs(filename, c_contiguous=True):
    np = require_numpy()
    iv = np.fromfile(filename, dtype=np.int32)
    if iv.size == 0:
        return np.zeros((0, 0), dtype=np.int32)
    dim = iv[0]
    if dim <= 0:
        raise ValueError(f"invalid ivecs dimension in {filename}")
    iv = iv.reshape(-1, 1 + dim)
    if not np.all(iv[:, 0] == dim):
        raise IOError("non-uniform vector sizes in " + filename)
    iv = iv[:, 1:]
    return iv.copy() if c_contiguous else iv


def fvecs_shape(filename):
    np = require_numpy()
    raw = np.memmap(filename, dtype=np.float32, mode="r")
    if raw.size == 0:
        return 0, 0
    dim = int(raw.view(np.int32)[0])
    if dim <= 0:
        raise ValueError(f"invalid fvecs dimension in {filename}")
    record_words = 1 + dim
    if raw.size % record_words != 0:
        raise IOError("non-uniform vector sizes in " + filename)
    return raw.size // record_words, dim


def bvecs_shape(filename):
    np = require_numpy()
    raw = np.memmap(filename, dtype=np.uint8, mode="r")
    if raw.size == 0:
        return 0, 0
    dim = int(np.frombuffer(bytes(raw[:4]), dtype="<i4")[0])
    if dim <= 0:
        raise ValueError(f"invalid bvecs dimension in {filename}")
    record_bytes = 4 + dim
    if raw.size % record_bytes != 0:
        raise IOError("non-uniform vector sizes in " + filename)
    return raw.size // record_bytes, dim


def read_bvecs(filename, c_contiguous=True):
    np = require_numpy()
    bv = np.memmap(filename, dtype=np.uint8, mode="r")
    if bv.size == 0:
        return np.zeros((0, 0), dtype=np.float32)
    dim = int(np.frombuffer(bytes(bv[:4]), dtype="<i4")[0])
    if dim <= 0:
        raise ValueError(f"invalid bvecs dimension in {filename}")
    record_bytes = 4 + dim
    if bv.size % record_bytes != 0:
        raise IOError("non-uniform vector sizes in " + filename)
    records = bv.reshape(-1, record_bytes)
    data = records[:, 4:].astype(np.float32)
    return data.copy() if c_contiguous else data


def vector_file_shape(filename):
    if filename.endswith(".fvecs"):
        return fvecs_shape(filename)
    if filename.endswith(".bvecs"):
        return bvecs_shape(filename)
    if filename.endswith(".bin"):
        n, dim, _ = header_bin_shape(filename)
        return n, dim
    raise ValueError(f"unsupported vector file format: {filename}")


def sample_row_indices(n, sample_size, seed):
    np = require_numpy()
    if sample_size <= 0 or sample_size >= n:
        return np.arange(n, dtype=np.int64)
    rng = np.random.default_rng(seed)
    return np.sort(rng.choice(n, size=sample_size, replace=False).astype(np.int64))


def read_fvecs_rows(filename, indices):
    np = require_numpy()
    n, dim = fvecs_shape(filename)
    raw = np.memmap(filename, dtype=np.float32, mode="r")
    records = raw.reshape(int(n), 1 + int(dim))
    return np.asarray(records[indices, 1:], dtype=np.float32)


def read_bvecs_rows(filename, indices, as_float=True):
    np = require_numpy()
    n, dim = bvecs_shape(filename)
    raw = np.memmap(filename, dtype=np.uint8, mode="r")
    records = raw.reshape(int(n), 4 + int(dim))
    dtype = np.float32 if as_float else np.uint8
    return np.asarray(records[indices, 4:], dtype=dtype)


def read_bigann_u8bin_rows(filename, indices, as_float=True):
    np = require_numpy()
    n, dim = bigann_u8bin_shape(filename)
    data = np.memmap(filename, dtype=np.uint8, mode="r", offset=8, shape=(int(n), int(dim)))
    dtype = np.float32 if as_float else np.uint8
    return np.asarray(data[indices], dtype=dtype)


def read_header_bin_rows(filename, indices, as_float=True):
    np = require_numpy()
    n, dim, kind = header_bin_shape(filename)
    if kind == "uint8":
        return read_bigann_u8bin_rows(filename, indices, as_float=as_float)
    data = np.memmap(filename, dtype=np.float32, mode="r", offset=8, shape=(int(n), int(dim)))
    return np.asarray(data[indices], dtype=np.float32)


def read_vector_sample(filename, sample_count, seed):
    n, dim = vector_file_shape(filename)
    if n == 0:
        return read_vectors_as_float(filename)
    sample_count = int(sample_count)
    actual = int(n) if sample_count <= 0 else min(sample_count, int(n))
    indices = sample_row_indices(int(n), actual, seed)
    if filename.endswith(".fvecs"):
        return read_fvecs_rows(filename, indices)
    if filename.endswith(".bvecs"):
        return read_bvecs_rows(filename, indices, as_float=False)
    if filename.endswith(".bin"):
        return read_header_bin_rows(filename, indices, as_float=False)
    raise ValueError(f"unsupported vector file format: {filename}")


def read_vector_sample_as_float(filename, sample_count, seed):
    data = read_vector_sample(filename, sample_count, seed)
    np = require_numpy()
    return np.asarray(data, dtype=np.float32)


def read_bigann_u8bin(filename, c_contiguous=True):
    np = require_numpy()
    with open(filename, "rb") as fp:
        header = fp.read(8)
    if len(header) != 8:
        raise ValueError(f"invalid BigANN uint8 bin header in {filename}")
    n, dim = struct.unpack("<II", header)
    if n == 0 or dim == 0:
        raise ValueError(f"invalid BigANN uint8 bin shape in {filename}")
    expected = 8 + int(n) * int(dim)
    actual = os.path.getsize(filename)
    if actual != expected:
        raise IOError(f"BigANN uint8 bin size mismatch in {filename}: expected {expected}, got {actual}")
    data = np.memmap(filename, dtype=np.uint8, mode="r", offset=8, shape=(int(n), int(dim)))
    data = data.astype(np.float32)
    return data.copy() if c_contiguous else data


def read_header_bin_as_float(filename, c_contiguous=True):
    np = require_numpy()
    n, dim, kind = header_bin_shape(filename)
    if kind == "uint8":
        return read_bigann_u8bin(filename, c_contiguous=c_contiguous)
    data = np.memmap(filename, dtype=np.float32, mode="r", offset=8, shape=(int(n), int(dim)))
    return np.asarray(data, dtype=np.float32).copy() if c_contiguous else data


def read_vectors_as_float(filename, c_contiguous=True):
    if filename.endswith(".fvecs"):
        return read_fvecs(filename, c_contiguous=c_contiguous)
    if filename.endswith(".bvecs"):
        return read_bvecs(filename, c_contiguous=c_contiguous)
    if filename.endswith(".bin"):
        return read_header_bin_as_float(filename, c_contiguous=c_contiguous)
    raise ValueError(f"unsupported vector file format: {filename}")


def header_bin_shape(filename):
    with open(filename, "rb") as fp:
        header = fp.read(8)
    if len(header) != 8:
        raise ValueError(f"invalid header bin file in {filename}")
    n, dim = struct.unpack("<II", header)
    if n == 0 or dim == 0:
        raise ValueError(f"invalid header bin shape in {filename}: {n} x {dim}")
    actual = os.path.getsize(filename)
    expected_u8 = 8 + int(n) * int(dim)
    expected_f32 = 8 + int(n) * int(dim) * 4
    if actual == expected_u8:
        return int(n), int(dim), "uint8"
    if actual == expected_f32:
        return int(n), int(dim), "float32"
    raise IOError(
        f"header bin size mismatch in {filename}: "
        f"expected {expected_u8} for uint8 or {expected_f32} for float32, got {actual}"
    )


def bigann_u8bin_shape(filename):
    n, dim, kind = header_bin_shape(filename)
    if kind != "uint8":
        raise IOError(f"expected BigANN uint8 bin, got {kind}: {filename}")
    return n, dim


def iter_header_bin_chunks(filename, chunk_size):
    np = require_numpy()
    n, dim, kind = header_bin_shape(filename)
    dtype = np.uint8 if kind == "uint8" else np.float32
    data = np.memmap(filename, dtype=dtype, mode="r", offset=8, shape=(int(n), int(dim)))
    for start in range(0, int(n), chunk_size):
        end = min(start + chunk_size, int(n))
        yield start, np.asarray(data[start:end], dtype=dtype)


def iter_ivecs_chunks(filename, chunk_size):
    np = require_numpy()
    raw = np.memmap(filename, dtype=np.uint32, mode="r")
    if raw.size == 0:
        return
    dim = int(raw[0])
    if dim <= 0:
        raise ValueError(f"invalid ivecs dimension in {filename}")
    record_words = 1 + dim
    if raw.size % record_words != 0:
        raise IOError("non-uniform vector sizes in " + filename)
    records = raw.reshape(-1, record_words)
    for start in range(0, records.shape[0], chunk_size):
        end = min(start + chunk_size, records.shape[0])
        yield start, np.asarray(records[start:end, 1:], dtype=np.uint32)


def ivecs_shape(filename):
    np = require_numpy()
    raw = np.memmap(filename, dtype=np.uint32, mode="r")
    if raw.size == 0:
        return 0, 0
    dim = int(raw[0])
    if dim <= 0:
        raise ValueError(f"invalid ivecs dimension in {filename}")
    record_words = 1 + dim
    if raw.size % record_words != 0:
        raise IOError("non-uniform vector sizes in " + filename)
    return raw.size // record_words, dim


def append_fvecs(fp, data):
    np = require_numpy()
    data = np.asarray(data, dtype=np.float32)
    if data.ndim == 1:
        data = data.reshape(-1, 1)
    out = np.empty((data.shape[0], data.shape[1] + 1), dtype=np.float32)
    out.view(np.uint32)[:, 0] = data.shape[1]
    out[:, 1:] = data
    fp.write(out.tobytes())


def append_ivecs(fp, data):
    np = require_numpy()
    data = np.asarray(data, dtype=np.uint32)
    if data.ndim == 1:
        data = data.reshape(-1, 1)
    out = np.empty((data.shape[0], data.shape[1] + 1), dtype=np.uint32)
    out[:, 0] = data.shape[1]
    out[:, 1:] = data
    fp.write(out.tobytes())


def append_Ivecs(fp, data):
    np = require_numpy()
    data = np.asarray(data, dtype=np.uint64)
    if data.ndim == 1:
        data = data.reshape(-1, 1)
    record_dtype = np.dtype([("dim", "<u4"), ("values", "<u8", (data.shape[1],))])
    out = np.empty(data.shape[0], dtype=record_dtype)
    out["dim"] = data.shape[1]
    out["values"] = data
    fp.write(out.tobytes())


def write_fvecs(filename, data):
    np = require_numpy()
    os.makedirs(os.path.dirname(filename) or ".", exist_ok=True)
    data = np.asarray(data, dtype=np.float32)
    with open(filename, "wb") as fp:
        append_fvecs(fp, data)


def write_ivecs(filename, data):
    np = require_numpy()
    os.makedirs(os.path.dirname(filename) or ".", exist_ok=True)
    data = np.asarray(data, dtype=np.uint32)
    with open(filename, "wb") as fp:
        append_ivecs(fp, data)


def write_Ivecs(filename, data):
    np = require_numpy()
    os.makedirs(os.path.dirname(filename) or ".", exist_ok=True)
    data = np.asarray(data, dtype=np.uint64)
    with open(filename, "wb") as fp:
        append_Ivecs(fp, data)


def orthogonal(dim, seed):
    np = require_numpy()
    rng = np.random.default_rng(seed)
    g = rng.standard_normal((dim, dim), dtype=np.float32)
    q, _ = np.linalg.qr(g)
    return q.astype(np.float32)


def default_split_size_max(n, clusters):
    base = (n + clusters - 1) // clusters
    scaled = (base * 12 + 9) // 10
    return ((scaled + 31) // 32) * 32


def default_clusters(dataset):
    key = dataset.lower()
    if key in ("sift1b", "sift100m"):
        return 1 << 16
    if key == "sift10m":
        return 4096
    if key == "deep1m":
        return 4096
    if key == "msmarco1m":
        return 4096
    if key == "siftsmall":
        return 48
    return 4096


def is_bigann_uint8_dataset(dataset):
    return dataset.lower() in ("sift1b", "sift10m", "sift100m")


def default_assign_index(dataset):
    return "hnsw" if is_bigann_uint8_dataset(dataset) else "exact"


def default_kmeans_assign_index(dataset):
    return "hnsw" if is_bigann_uint8_dataset(dataset) else "exact"


def default_kmeans_niter(dataset):
    return 8 if is_bigann_uint8_dataset(dataset) else 25


def first_existing_path(paths):
    for path in paths:
        if path and os.path.exists(path):
            return path
    return None


def base_vector_path(source, dataset, files=None):
    if files and files.get("base"):
        return os.path.join(source, files["base"])
    key = dataset.lower()
    if key in ("sift", "sift10m", "sift100m", "deep1m", "msmarco1m"):
        return os.path.join(source, "sift_base.fvecs" if key == "sift" else "base.bin")
    return first_existing_path([
        os.path.join(source, f"{dataset}_base.fvecs"),
        os.path.join(source, f"{dataset}_base.bvecs"),
        os.path.join(source, f"{dataset}_base.bin"),
        os.path.join(source, "base.bin"),
    ])


def training_vector_path(source, dataset, explicit_train, files=None):
    if explicit_train:
        return explicit_train
    return first_existing_path([
        os.path.join(source, "bigann_learn.bvecs"),
        os.path.join(source, f"{dataset}_learn.bvecs"),
        os.path.join(source, f"{dataset}_learn.fvecs"),
        base_vector_path(source, dataset, files),
    ])


def output_paths(source, dataset, clusters):
    return {
        "centroid": os.path.join(source, f"{dataset}_centroid_{clusters}.fvecs"),
        "dist": os.path.join(source, f"{dataset}_dist_to_centroid_{clusters}.fvecs"),
        "cluster": os.path.join(source, f"{dataset}_cluster_id_{clusters}.ivecs"),
    }


def assignment_manifest_path(source, dataset, requested_clusters):
    return os.path.join(source, f"{dataset}_assignment_{requested_clusters}.txt")


def write_assignment_manifest(source, dataset, requested_clusters, actual_clusters, paths):
    path = assignment_manifest_path(source, dataset, requested_clusters)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as fp:
        fp.write("assignment split\n")
        fp.write(f"requested_clusters {int(requested_clusters)}\n")
        fp.write(f"actual_clusters {int(actual_clusters)}\n")
        for key in ("centroid", "dist", "cluster"):
            fp.write(f"{key} {paths[key]}\n")


def read_assignment_manifest_clusters(source, dataset, requested_clusters):
    path = assignment_manifest_path(source, dataset, requested_clusters)
    if not os.path.exists(path):
        return None
    actual = None
    with open(path, "r", encoding="utf-8") as fp:
        for line in fp:
            parts = line.strip().split()
            if len(parts) == 2 and parts[0] == "actual_clusters":
                actual = int(parts[1])
                break
    return actual


def resolve_assignment_clusters(source, dataset, requested_clusters):
    actual = read_assignment_manifest_clusters(source, dataset, requested_clusters)
    if actual:
        return actual

    direct = output_paths(source, dataset, requested_clusters)
    if all(os.path.exists(direct[key]) for key in ("centroid", "dist", "cluster")):
        return requested_clusters

    pattern = os.path.join(source, f"{dataset}_centroid_*.fvecs")
    candidates = []
    prefix = f"{dataset}_centroid_"
    suffix = ".fvecs"
    for path in glob.glob(pattern):
        name = os.path.basename(path)
        if not (name.startswith(prefix) and name.endswith(suffix)):
            continue
        cluster_text = name[len(prefix):-len(suffix)]
        if not cluster_text.isdigit():
            continue
        clusters = int(cluster_text)
        paths = output_paths(source, dataset, clusters)
        if all(os.path.exists(paths[key]) for key in ("centroid", "dist", "cluster")):
            candidates.append(clusters)

    candidates = sorted(set(candidates))
    if len(candidates) == 1:
        return candidates[0]
    if candidates:
        raise RuntimeError(
            f"multiple cluster outputs found for requested {requested_clusters}: {candidates}; "
            "specify the actual --clusters value"
        )
    raise RuntimeError(f"cannot find cluster outputs with requested clusters={requested_clusters}")


def make_assignment_index(faiss, centroids, args):
    np = require_numpy()
    dim = centroids.shape[1]
    centroids = np.ascontiguousarray(centroids.astype(np.float32, copy=False))
    t0 = time.time()
    log_status(
        f"building {args.assign_index} centroid assignment index "
        f"for {centroids.shape[0]:,} centroids"
    )

    if args.assign_index == "exact":
        assignment_index = faiss.IndexFlatL2(dim)
    elif args.assign_index == "hnsw":
        assignment_index = faiss.IndexHNSWFlat(dim, args.hnsw_m)
        assignment_index.hnsw.efConstruction = args.hnsw_ef_construction
        assignment_index.hnsw.efSearch = max(args.hnsw_ef_search, 1)
    else:
        raise ValueError(f"unknown assignment index: {args.assign_index}")

    assignment_index.add(centroids)
    log_status(
        f"centroid assignment index ready in {format_duration(time.time() - t0)}"
    )
    return assignment_index


class BaseVectorRows:
    def __init__(self, base_path):
        np = require_numpy()
        self.base_path = base_path
        self.is_bin = base_path.endswith(".bin")
        if self.is_bin:
            n, dim, kind = header_bin_shape(base_path)
            self.n = int(n)
            self.dim = int(dim)
            self.kind = kind
            self.data = np.memmap(
                base_path,
                dtype=np.uint8 if kind == "uint8" else np.float32,
                mode="r",
                offset=8,
                shape=(self.n, self.dim),
            )
        else:
            data = read_vectors_as_float(base_path)
            self.n = int(data.shape[0])
            self.dim = int(data.shape[1])
            self.data = data

    def iter_chunks(self, chunk_size):
        chunk_size = max(1, int(chunk_size))
        for start in range(0, self.n, chunk_size):
            end = min(start + chunk_size, self.n)
            yield start, np.asarray(self.data[start:end])

    def rows_float(self, indices):
        return np.asarray(self.data[indices], dtype=np.float32)


def assign_vector_chunk(chunk, assignment_index):
    np = require_numpy()
    search_chunk = np.ascontiguousarray(chunk.astype(np.float32, copy=False))
    dist2, cluster_id = assignment_index.search(search_chunk, 1)
    return dist2.reshape(-1).astype(np.float32, copy=False), cluster_id.reshape(-1).astype(np.uint32)


def write_assignment_arrays(paths, dist2, cluster_id, chunk_size):
    np = require_numpy()
    chunk_size = max(1, int(chunk_size))
    n = int(cluster_id.shape[0])
    progress = ProgressReporter("writing assignment arrays", n)
    with open(paths["dist"], "wb") as dist_fp, open(paths["cluster"], "wb") as cluster_fp:
        for start in range(0, n, chunk_size):
            end = min(start + chunk_size, n)
            append_fvecs(dist_fp, np.sqrt(dist2[start:end]).reshape(-1, 1).astype(np.float32))
            append_ivecs(cluster_fp, cluster_id[start:end].reshape(-1, 1))
            progress.update(end)
    progress.done()


def nearest_assign_all(accessor, assignment_index, chunk_size):
    np = require_numpy()
    cluster_id = np.empty(accessor.n, dtype=np.uint32)
    dist2 = np.empty(accessor.n, dtype=np.float32)
    progress = ProgressReporter("initial nearest assignment for split", accessor.n)
    for start, chunk in accessor.iter_chunks(chunk_size):
        assigned_dist2, assigned_cluster = assign_vector_chunk(chunk, assignment_index)
        end = start + chunk.shape[0]
        dist2[start:end] = assigned_dist2
        cluster_id[start:end] = assigned_cluster
        progress.update(end)
    progress.done()
    return dist2, cluster_id


def member_groups_from_cluster_id(cluster_id, clusters):
    np = require_numpy()
    counts = np.bincount(cluster_id, minlength=clusters)
    order = np.argsort(cluster_id, kind="stable")
    offsets = np.concatenate(([0], np.cumsum(counts, dtype=np.int64)))
    return [order[offsets[c]:offsets[c + 1]].copy() for c in range(clusters)]


def farthest_pair_exact(data, block_size=512):
    np = require_numpy()
    data = np.ascontiguousarray(data.astype(np.float32, copy=False))
    n = data.shape[0]
    if n < 2:
        return 0, 0, 0.0
    norms = np.einsum("ij,ij->i", data, data)
    best_i = 0
    best_j = 1
    best_dist = -1.0
    block_size = max(1, int(block_size))
    for start in range(0, n, block_size):
        end = min(start + block_size, n)
        dist = norms[start:end, None] + norms[None, :] - 2.0 * np.dot(data[start:end], data.T)
        rows = np.arange(start, end)
        dist[np.arange(end - start), rows] = -np.inf
        flat = int(np.argmax(dist))
        value = float(dist.reshape(-1)[flat])
        if value > best_dist:
            local_i, j = divmod(flat, n)
            best_i = start + local_i
            best_j = j
            best_dist = value
    return best_i, best_j, max(0.0, best_dist)


def split_members_by_farthest_pair(accessor, members):
    np = require_numpy()
    data = accessor.rows_float(members)
    a, b, farthest_dist = farthest_pair_exact(data)
    if farthest_dist <= 0.0:
        mid = members.shape[0] // 2
        return members[:mid].copy(), members[mid:].copy()

    anchor_a = data[a]
    anchor_b = data[b]
    dist_a = np.einsum("ij,ij->i", data - anchor_a, data - anchor_a)
    dist_b = np.einsum("ij,ij->i", data - anchor_b, data - anchor_b)
    left_mask = dist_a <= dist_b
    left_count = int(np.count_nonzero(left_mask))
    if left_count == 0 or left_count == members.shape[0]:
        axis = anchor_b - anchor_a
        if float(np.dot(axis, axis)) <= 0.0:
            order = np.arange(members.shape[0])
        else:
            order = np.argsort(np.dot(data, axis), kind="stable")
        mid = members.shape[0] // 2
        return members[order[:mid]].copy(), members[order[mid:]].copy()
    return members[left_mask].copy(), members[~left_mask].copy()


def assign_split_vectors(assignment_index, base_path, initial_centroids, args):
    np = require_numpy()
    t0 = time.time()
    accessor = BaseVectorRows(base_path)
    if accessor.n == 0:
        raise RuntimeError(f"empty dataset: {base_path}")
    if accessor.dim != initial_centroids.shape[1]:
        raise RuntimeError("base vector dimension and trained centroid dimension do not match")

    max_size = args.size_max if args.size_max > 0 else default_split_size_max(accessor.n, args.clusters)
    if max_size <= 0:
        raise ValueError("--size-max must be positive")
    log_status(
        f"assignment split start: requested_clusters={args.clusters:,} "
        f"max_size={max_size:,}"
    )

    _, initial_cluster_id = nearest_assign_all(accessor, assignment_index, args.chunk_size)
    queue = deque(member_groups_from_cluster_id(initial_cluster_id, args.clusters))
    final_members = []
    split_count = 0
    max_seen = 0
    while queue:
        members = queue.popleft()
        size = int(members.shape[0])
        if size == 0:
            continue
        max_seen = max(max_seen, size)
        if size <= max_size:
            final_members.append(members)
            continue
        left, right = split_members_by_farthest_pair(accessor, members)
        if left.shape[0] == 0 or right.shape[0] == 0:
            raise RuntimeError("split assignment produced an empty child cluster")
        queue.append(left)
        queue.append(right)
        split_count += 1
        if split_count == 1 or split_count % 50 == 0:
            log_status(
                f"split progress: splits={split_count:,} pending={len(queue):,} "
                f"final={len(final_members):,} last_split={size:,}"
            )

    actual_clusters = len(final_members)
    paths = output_paths(normalized_source(args.source), args.dataset, actual_clusters)
    log_status(
        f"split assignment clusters ready: actual_clusters={actual_clusters:,} "
        f"largest_seen={max_seen:,}; writing {paths['cluster']}"
    )

    centroids = np.empty((actual_clusters, accessor.dim), dtype=np.float32)
    final_cluster_id = np.empty(accessor.n, dtype=np.uint32)
    final_dist2 = np.empty(accessor.n, dtype=np.float32)
    progress = ProgressReporter("computing split centroids", accessor.n)
    done = 0
    for cid, members in enumerate(final_members):
        data = accessor.rows_float(members)
        centroid = data.mean(axis=0, dtype=np.float64).astype(np.float32)
        centroids[cid] = centroid
        diff = data - centroid
        final_dist2[members] = np.einsum("ij,ij->i", diff, diff).astype(np.float32)
        final_cluster_id[members] = cid
        done += int(members.shape[0])
        progress.update(done)
    progress.done()

    write_fvecs(paths["centroid"], centroids)
    write_assignment_arrays(paths, final_dist2, final_cluster_id, args.chunk_size)

    counts = np.bincount(final_cluster_id, minlength=actual_clusters)
    if counts.max() > max_size:
        raise RuntimeError("split assignment produced cluster sizes outside requested max")
    write_assignment_manifest(
        normalized_source(args.source),
        args.dataset,
        args.clusters,
        actual_clusters,
        paths,
    )
    log_status(
        f"assignment split complete in {format_duration(time.time() - t0)}: "
        f"actual_clusters={actual_clusters:,} min_size={int(counts.min()):,} "
        f"max_size={int(counts.max()):,} mean_size={float(counts.mean()):.2f}"
    )
    return actual_clusters


def run_ivf(args):
    try:
        import faiss
    except ImportError as exc:
        raise RuntimeError("faiss is required for IVF clustering") from exc

    np = require_numpy()
    source = normalized_source(args.source)
    base_path = base_vector_path(source, args.dataset, args.files)
    train_path = training_vector_path(source, args.dataset, args.train, args.files)
    if not base_path:
        raise RuntimeError(f"cannot find base vectors under {source}")
    if not train_path:
        raise RuntimeError(f"cannot find training vectors under {source}")

    train_n, train_dim = vector_file_shape(train_path)
    if train_n == 0 or train_dim == 0:
        raise RuntimeError(f"empty training dataset: {train_path}")

    log_status(f"base vectors: {base_path}")
    log_status(
        f"training vectors: {train_path} "
        f"shape={int(train_n):,} x {int(train_dim):,}"
    )

    train_sample_count = int(math.ceil(float(train_n) * args.train_sample_ratio))
    train_sample_count = min(max(1, train_sample_count), int(train_n))
    if train_sample_count < args.clusters:
        raise ValueError("effective training sample count must be >= --clusters")

    log_status(f"loading training vectors: {train_path}")
    t0 = time.time()
    if train_sample_count < int(train_n):
        log_status(
            f"using {train_sample_count:,} training vectors for k-means "
            f"from {int(train_n):,} available, ratio={args.train_sample_ratio:g}"
        )
        train_x = read_vector_sample(train_path, train_sample_count, args.seed)
    else:
        log_status(f"using all {int(train_n):,} training vectors for k-means")
        train_x = read_vector_sample(train_path, 0, args.seed)
    if train_x.shape[0] == 0:
        raise RuntimeError(f"empty training dataset: {train_path}")
    if train_x.shape[0] < args.clusters:
        raise ValueError("k-means training vector count must be >= --clusters")
    log_status(
        f"loaded training vectors: {train_x.shape[0]:,} x {train_x.shape[1]} "
        f"in {format_duration(time.time() - t0)}"
    )

    if args.faiss_threads > 0 and hasattr(faiss, "omp_set_num_threads"):
        faiss.omp_set_num_threads(args.faiss_threads)
        log_status(f"faiss threads: {args.faiss_threads}")

    log_status(
        f"training IVF centroids with k-means: clusters={args.clusters:,} "
        f"niter={args.kmeans_niter} nredo={args.kmeans_nredo} "
        f"assign_index={args.kmeans_assign_index}"
    )
    t0 = time.time()
    train_x_float = np.ascontiguousarray(train_x.astype(np.float32, copy=False))
    del train_x
    if args.kmeans_assign_index == "exact":
        quantizer = faiss.IndexFlatL2(train_x_float.shape[1])
    elif args.kmeans_assign_index == "hnsw":
        if not hasattr(faiss, "IndexHNSWFlat"):
            raise RuntimeError("this faiss build does not provide IndexHNSWFlat")
        quantizer = faiss.IndexHNSWFlat(train_x_float.shape[1], args.hnsw_m)
        quantizer.hnsw.efConstruction = args.hnsw_ef_construction
        quantizer.hnsw.efSearch = args.hnsw_ef_search
    else:
        raise ValueError(f"unknown k-means assignment index: {args.kmeans_assign_index}")
    clustering = faiss.Clustering(train_x_float.shape[1], args.clusters)
    clustering.verbose = bool(args.verbose)
    clustering.seed = int(args.seed)
    clustering.niter = int(args.kmeans_niter)
    clustering.nredo = int(args.kmeans_nredo)
    clustering.max_points_per_centroid = int(
        math.ceil(float(train_x_float.shape[0]) / float(args.clusters)))
    clustering.train(train_x_float, quantizer)
    centroids = faiss.vector_to_array(clustering.centroids).reshape(args.clusters, train_x_float.shape[1])
    centroids = np.ascontiguousarray(centroids.astype(np.float32, copy=False))
    log_status(f"trained IVF centroids in {format_duration(time.time() - t0)}")

    assignment_index = make_assignment_index(faiss, centroids, args)

    log_status("assignment stage: split (bounded clustering)")
    assign_split_vectors(assignment_index, base_path, centroids, args)


def run_rabitq_one(args):
    np = require_numpy()
    source = normalized_source(args.source)
    clusters = resolve_assignment_clusters(source, args.dataset, args.clusters)
    base_path = base_vector_path(source, args.dataset, args.files)
    if not base_path:
        raise RuntimeError(f"cannot find base vectors under {source}")
    log_status(
        f"loading RabitQ inputs for dataset={args.dataset} clusters={clusters:,}"
    )
    centroids = read_fvecs(os.path.join(source, f"{args.dataset}_centroid_{clusters}.fvecs"))
    cluster_path = os.path.join(source, f"{args.dataset}_cluster_id_{clusters}.ivecs")

    if base_path.endswith(".bin"):
        n, dim, _ = header_bin_shape(base_path)
        cluster_n, cluster_dim = ivecs_shape(cluster_path)
        if cluster_dim != 1:
            raise RuntimeError("cluster_id file must have dimension 1")
        cluster_id = None
    else:
        x = read_vectors_as_float(base_path)
        n, dim = x.shape
        cluster_id = np.squeeze(read_ivecs(cluster_path))
        cluster_n = cluster_id.shape[0]
    if n != cluster_n:
        raise RuntimeError("base vector count and cluster_id count do not match")
    log_status(f"RabitQ input vectors: {int(n):,} x {int(dim)}")

    bits = args.bits or ((dim + 63) // 64 * 64)
    if bits % 64 != 0 or bits < dim:
        raise ValueError("--bits must be a multiple of 64 and >= dataset dimension")

    padded_dim = max(dim, bits)
    centroids_pad = np.pad(centroids, ((0, 0), (0, padded_dim - dim)), "constant")

    log_status(f"building random projection and randomized centroids: bits={bits}")
    t0 = time.time()
    projection = orthogonal(padded_dim, args.seed).T
    cp = np.dot(centroids_pad, projection)
    log_status(f"randomized centroids ready in {format_duration(time.time() - t0)}")

    write_fvecs(os.path.join(source, f"P_C{clusters}_B{bits}.fvecs"), projection)
    write_fvecs(os.path.join(source, f"RandCentroid_C{clusters}_B{bits}.fvecs"), cp)

    def compute_and_write_chunk(x_chunk, cluster_chunk, x0_fp, binary_fp):
        x_float = np.asarray(x_chunk, dtype=np.float32)
        if x_float.shape[1] != padded_dim:
            x_pad = np.pad(x_float, ((0, 0), (0, padded_dim - x_float.shape[1])), "constant")
        else:
            x_pad = x_float
        xp = np.dot(x_pad, projection)
        cluster_chunk = np.asarray(cluster_chunk, dtype=np.uint32).reshape(-1)
        xp = xp - cp[cluster_chunk]

        bin_xp = xp > 0
        norm = np.linalg.norm(xp, axis=1, keepdims=True)
        x0 = np.sum(
            xp[:, :bits] * (2 * bin_xp[:, :bits] - 1) / np.sqrt(bits),
            axis=1,
            keepdims=True,
        ) / norm
        x0[~np.isfinite(x0)] = args.zero_norm_x0
        x0 = x0.astype(np.float32)

        flat_bits = bin_xp[:, :bits].reshape(-1)
        uint64_xp = np.packbits(flat_bits.reshape(-1, 8, 8)[:, ::-1]).view(np.uint64)
        uint64_xp = uint64_xp.reshape(-1, bits >> 6)
        append_fvecs(x0_fp, x0)
        append_Ivecs(binary_fp, uint64_xp)

    x0_path = os.path.join(source, f"x0_C{clusters}_B{bits}.fvecs")
    binary_path = os.path.join(source, f"RandNet_C{clusters}_B{bits}.Ivecs")
    progress = ProgressReporter("generating RabitQ vectors", n)
    with open(x0_path, "wb") as x0_fp, open(binary_path, "wb") as binary_fp:
        if base_path.endswith(".bin"):
            cluster_iter = iter_ivecs_chunks(cluster_path, max(1, args.chunk_size))
            for (start, x_chunk), (cluster_start, cluster_chunk) in zip(
                    iter_header_bin_chunks(base_path, max(1, args.chunk_size)),
                    cluster_iter):
                if start != cluster_start or x_chunk.shape[0] != cluster_chunk.shape[0]:
                    raise RuntimeError("base vector chunk and cluster_id chunk do not match")
                compute_and_write_chunk(x_chunk, cluster_chunk, x0_fp, binary_fp)
                progress.update(start + x_chunk.shape[0])
        else:
            chunk_size = max(1, args.chunk_size)
            for start in range(0, n, chunk_size):
                end = min(start + chunk_size, n)
                compute_and_write_chunk(x[start:end], cluster_id[start:end], x0_fp, binary_fp)
                progress.update(end)
    progress.done()


def run_rabitq(args):
    run_rabitq_one(args)


def normalized_source(source):
    return source if source.endswith(os.sep) else source + os.sep


CONFIG_ARG_TYPES = {
    "source": str,
    "clusters": int,
    "train": str,
    "train_sample_ratio": float,
    "kmeans_niter": int,
    "kmeans_nredo": int,
    "kmeans_assign_index": str,
    "faiss_threads": int,
    "cpu_threads": int,
    "chunk_size": int,
    "assign_index": str,
    "hnsw_m": int,
    "hnsw_ef_construction": int,
    "hnsw_ef_search": int,
    "bits": int,
    "size_max": int,
    "seed": int,
    "zero_norm_x0": float,
    "verbose": bool,
}


def normalize_config_key(key):
    return str(key).replace("-", "_")


def cli_provided_options(argv):
    provided = set()
    for token in argv:
        if not token.startswith("--"):
            continue
        name = token[2:].split("=", 1)[0]
        provided.add(normalize_config_key(name))
    return provided


def load_config_file(path, explicit):
    if not path:
        return {}
    if not os.path.exists(path):
        if explicit:
            raise FileNotFoundError(f"config file does not exist: {path}")
        return {}
    with open(path, "r", encoding="utf-8") as fp:
        data = json.load(fp)
    if not isinstance(data, dict):
        raise ValueError("config root must be a JSON object")
    return data


def config_value_as(value, converter, path):
    if converter is bool:
        if not isinstance(value, bool):
            raise ValueError(f"config value must be a bool: {path}")
        return value
    if converter is int:
        if isinstance(value, bool) or int(value) != value:
            raise ValueError(f"config value must be an integer: {path}")
        return int(value)
    if converter is float:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError(f"config value must be a number: {path}")
        return float(value)
    if converter is str:
        if not isinstance(value, str):
            raise ValueError(f"config value must be a string: {path}")
        return value
    return value


def apply_preprocess_config_section(args, section, provided, path):
    if section is None:
        return
    if not isinstance(section, dict):
        raise ValueError(f"config section must be an object: {path}")
    for raw_key, value in section.items():
        key = normalize_config_key(raw_key)
        if key not in CONFIG_ARG_TYPES or key in provided:
            continue
        setattr(args, key, config_value_as(value, CONFIG_ARG_TYPES[key], f"{path}.{raw_key}"))


def apply_preprocess_config(args, provided):
    if args.no_config:
        return
    explicit_config = "config" in provided
    config = load_config_file(args.config, explicit_config)
    if not config:
        return

    if "dataset" not in provided and not args.dataset:
        default_dataset = config.get("default_dataset")
        if default_dataset is not None:
            if not isinstance(default_dataset, str):
                raise ValueError("config value must be a string: default_dataset")
            args.dataset = default_dataset

    defaults = config.get("defaults", {})
    if defaults is not None and not isinstance(defaults, dict):
        raise ValueError("config section must be an object: defaults")
    apply_preprocess_config_section(
        args,
        defaults.get("preprocess") if isinstance(defaults, dict) else None,
        provided,
        "defaults.preprocess",
    )

    datasets = config.get("datasets", {})
    if datasets is not None and not isinstance(datasets, dict):
        raise ValueError("config section must be an object: datasets")
    dataset_section = datasets.get(args.dataset) if isinstance(datasets, dict) else None
    if dataset_section is not None:
        files = dataset_section.get("files", {})
        if not isinstance(files, dict) or any(not isinstance(v, str) or not v for v in files.values()):
            raise ValueError("dataset files must contain nonempty string paths")
        args.files = files
        apply_preprocess_config_section(args, dataset_section, provided, f"datasets.{args.dataset}")
        apply_preprocess_config_section(
            args,
            dataset_section.get("preprocess") if isinstance(dataset_section, dict) else None,
            provided,
            f"datasets.{args.dataset}.preprocess",
        )


def parse_args():
    parser = argparse.ArgumentParser(description="IVF-RaBitQ preprocessing")
    parser.add_argument("stage", choices=["ivf", "rabitq", "all"], help="preprocessing stage to run")
    parser.add_argument("--dataset", default="", help="dataset prefix, e.g. sift")
    parser.add_argument("--source", default="", help="directory containing vector files")
    parser.add_argument("--config", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "config.json"), help="JSON config file used for defaults")
    parser.add_argument("--no-config", action="store_true", help="disable config.json defaults")
    parser.add_argument(
        "--clusters",
        type=int,
        default=0,
        help="number of IVF clusters; default 65536 for sift1B/sift100M, 48 for siftsmall, otherwise 4096",
    )
    parser.add_argument(
        "--train",
        default="",
        help="training vector file for IVF centroid training; sift1B defaults to <source>/bigann_learn.bvecs",
    )
    parser.add_argument(
        "--train-sample-ratio",
        type=float,
        default=1.0,
        help="fraction of training vectors loaded for k-means, in (0, 1]; default 1 uses the whole training file",
    )
    parser.add_argument(
        "--kmeans-niter",
        type=int,
        default=0,
        help="FAISS k-means iterations; default 8 for sift1B/sift10M/sift100M and 25 otherwise",
    )
    parser.add_argument(
        "--kmeans-nredo",
        type=int,
        default=1,
        help="FAISS k-means restarts",
    )
    parser.add_argument(
        "--kmeans-assign-index",
        choices=["exact", "hnsw"],
        default="",
        help="assignment index used inside FAISS k-means; default hnsw for sift1B/sift10M/sift100M and exact otherwise",
    )
    parser.add_argument(
        "--faiss-threads",
        type=int,
        default=0,
        help="FAISS OpenMP thread count; default 0 uses --cpu-threads when set, otherwise keeps FAISS default",
    )
    parser.add_argument(
        "--cpu-threads",
        type=int,
        default=0,
        help="thread limit for FAISS/OpenMP/BLAS libraries; default 0 leaves library defaults unchanged",
    )
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=65536,
        help="base-vector chunk size used for BigANN .bin assignment and RabitQ generation",
    )
    parser.add_argument(
        "--assign-index",
        choices=["exact", "hnsw"],
        default="",
        help="centroid assignment search index; default hnsw for sift1B/sift10M/sift100M and exact otherwise",
    )
    parser.add_argument(
        "--hnsw-m",
        type=int,
        default=32,
        help="HNSW neighbor count for --assign-index hnsw",
    )
    parser.add_argument(
        "--hnsw-ef-construction",
        type=int,
        default=200,
        help="HNSW efConstruction for --assign-index hnsw",
    )
    parser.add_argument(
        "--hnsw-ef-search",
        type=int,
        default=64,
        help="HNSW efSearch for --assign-index hnsw",
    )
    parser.add_argument("--bits", type=int, default=0, help="RaBitQ padded bit dimension, default ceil(D/64)*64")
    parser.add_argument(
        "--size-max",
        type=int,
        default=0,
        help="maximum cluster size for bounded split clustering; default ceil(ceil(N/clusters)*1.2) rounded up to a multiple of 32",
    )
    parser.add_argument("--seed", type=int, default=0, help="random seed for orthogonal projection")
    parser.add_argument("--zero-norm-x0", type=float, default=0.8, help="fallback x0 for zero residual vectors")
    parser.add_argument("--verbose", action="store_true", help="enable verbose faiss training output")
    args = parser.parse_args()
    args.files = {}
    provided = cli_provided_options(sys.argv[1:])
    apply_preprocess_config(args, provided)
    if not args.no_config:
        for key in ("source", "train"):
            value = getattr(args, key, "")
            if key not in provided and value:
                setattr(args, key, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(args.config)), value)))
    if not args.dataset or not args.source:
        parser.error("--dataset and --source are required unless provided by config.json")
    if args.clusters <= 0:
        args.clusters = default_clusters(args.dataset)
    if not args.assign_index:
        args.assign_index = default_assign_index(args.dataset)
    if not args.kmeans_assign_index:
        args.kmeans_assign_index = default_kmeans_assign_index(args.dataset)
    if args.kmeans_niter <= 0:
        args.kmeans_niter = default_kmeans_niter(args.dataset)
    if args.chunk_size <= 0:
        raise ValueError("--chunk-size must be positive")
    if args.train_sample_ratio <= 0.0 or args.train_sample_ratio > 1.0:
        raise ValueError("--train-sample-ratio must be in (0, 1]")
    if args.kmeans_niter <= 0:
        raise ValueError("--kmeans-niter must be positive")
    if args.kmeans_nredo <= 0:
        raise ValueError("--kmeans-nredo must be positive")
    if args.cpu_threads < 0:
        raise ValueError("--cpu-threads must be non-negative")
    if args.faiss_threads < 0:
        raise ValueError("--faiss-threads must be non-negative")
    if args.cpu_threads > 0 and args.faiss_threads == 0:
        args.faiss_threads = args.cpu_threads
    if args.hnsw_m <= 0 or args.hnsw_ef_construction <= 0 or args.hnsw_ef_search <= 0:
        raise ValueError("hnsw-m, hnsw-ef-construction, and hnsw-ef-search must be positive")
    return args


def main():
    args = parse_args()
    apply_cpu_thread_limit(args)
    total_t0 = time.time()
    log_status(
        f"preprocess start: stage={args.stage} dataset={args.dataset} "
        f"clusters={args.clusters:,} "
        f"assign_index={args.assign_index} train_sample_ratio={args.train_sample_ratio:g} "
        f"kmeans_niter={args.kmeans_niter} kmeans_nredo={args.kmeans_nredo} "
        f"kmeans_assign_index={args.kmeans_assign_index} "
        f"cpu_threads={args.cpu_threads} faiss_threads={args.faiss_threads} "
        f"chunk_size={args.chunk_size:,}"
    )
    if args.stage in ("ivf", "all"):
        run_ivf(args)
    if args.stage in ("rabitq", "all"):
        run_rabitq(args)
    log_status(f"preprocess complete in {format_duration(time.time() - total_t0)}")


if __name__ == "__main__":
    main()
