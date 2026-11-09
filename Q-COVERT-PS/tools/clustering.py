"""FAISS k-means followed by bounded farthest-pair cluster splitting."""

from dataclasses import dataclass
from collections import deque

import numpy as np


def _matrix(x):
    x = np.ascontiguousarray(x, dtype=np.float32)
    if x.ndim != 2 or not x.shape[0] or not x.shape[1] or not np.isfinite(x).all():
        raise ValueError("expected a nonempty, finite N x D matrix")
    return x


def _farthest_pair(data, block_size=256):
    norms = np.einsum("ij,ij->i", data, data)
    best = (-1.0, 0, 1)
    for start in range(0, len(data), block_size):
        block = data[start:start + block_size]
        d2 = np.maximum(0, norms[start:start + len(block), None] + norms - 2 * block @ data.T)
        rows = np.arange(start, start + len(block))
        d2[np.arange(len(block)), rows] = -np.inf
        flat = int(np.argmax(d2))
        value = float(d2.flat[flat])
        if value > best[0]:
            i, j = divmod(flat, len(data))
            best = (value, start + i, j)
    return best


@dataclass
class ClusterResult:
    centroids: np.ndarray
    labels: np.ndarray
    distances: np.ndarray  # Euclidean distance to assigned centroid


def _split_mask(data):
    """Bisect one oversized group along its farthest pair, with a balanced fallback."""
    diameter, i, j = _farthest_pair(data)
    if diameter > 0:
        da = np.einsum("ij,ij->i", data - data[i], data - data[i])
        db = np.einsum("ij,ij->i", data - data[j], data - data[j])
        left = da <= db
    else:
        left = np.zeros(len(data), dtype=bool)
    if left.all() or not left.any():
        axis = data[j] - data[i]
        order = np.argsort(data @ axis, kind="stable") if diameter > 0 else np.arange(len(data))
        left = np.zeros(len(data), dtype=bool)
        left[order[:len(data) // 2]] = True
    return left


def _bounded_groups(vectors, memberships, max_cluster_size):
    """Recursively split each membership until every group fits the size bound."""
    pending = deque(memberships)
    groups = []
    while pending:
        members = pending.popleft()
        if not len(members):
            continue
        if len(members) <= max_cluster_size:
            groups.append(members)
            continue
        data = np.asarray(vectors[members], dtype=np.float32)
        left = _split_mask(data)
        pending.extend((members[left], members[~left]))
    return groups


def _assemble(vectors, groups, n):
    """Recompute centroids, labels and distances for the final bounded groups."""
    centroids = np.empty((len(groups), vectors.shape[1]), dtype=np.float32)
    final_labels = np.empty(n, dtype=np.int64)
    distances = np.empty(n, dtype=np.float32)
    for c, members in enumerate(groups):
        data = np.asarray(vectors[members], dtype=np.float32)
        centroids[c] = data.mean(axis=0, dtype=np.float64)
        final_labels[members] = c
        distances[members] = np.linalg.norm(data - centroids[c], axis=1)
    return ClusterResult(centroids, final_labels, distances)


def split_clusters(vectors, labels, max_cluster_size, centroid_count=None):
    """Bound cluster sizes for an existing assignment, skipping k-means training.

    vectors may be a memory-mapped uint8/float32 N x D matrix; only the members
    of the group currently being split are materialized as float32.
    """
    labels = np.asanyarray(labels)
    if labels.ndim != 1 or not np.issubdtype(labels.dtype, np.integer):
        raise ValueError("labels must be a one-dimensional integer array")
    if not isinstance(max_cluster_size, (int, np.integer)) or max_cluster_size < 1:
        raise ValueError("max_cluster_size must be a positive integer")
    vectors = np.asanyarray(vectors)
    if vectors.ndim != 2 or vectors.shape[0] != labels.shape[0] or not labels.shape[0]:
        raise ValueError("expected an N x D matrix with one label per row")
    if vectors.dtype not in (np.dtype('uint8'), np.dtype('float32')):
        raise ValueError("vectors must be a uint8 or float32 matrix")
    if vectors.dtype == np.float32 and not np.isfinite(vectors).all():
        raise ValueError("expected a finite N x D matrix")
    if labels.min() < 0:
        raise ValueError("labels must be nonnegative")
    if centroid_count is None:
        centroid_count = int(labels.max()) + 1
    if not isinstance(centroid_count, (int, np.integer)) or not 1 <= centroid_count or labels.max() >= centroid_count:
        raise ValueError("centroid_count must exceed every label")

    counts = np.bincount(labels.astype(np.int64, copy=False), minlength=int(centroid_count))
    order = np.argsort(labels, kind="stable")
    ends = np.cumsum(counts)
    memberships = [order[end - count:end] for end, count in zip(ends, counts)]
    groups = _bounded_groups(vectors, memberships, max_cluster_size)
    return _assemble(vectors, groups, len(labels))


def cluster_vectors(vectors, max_cluster_size, initial_clusters=None, niter=20, seed=42):
    """FAISS k-means, then recursively split oversized groups by their farthest pair."""
    x = _matrix(vectors)
    n = len(x)
    if not isinstance(max_cluster_size, (int, np.integer)) or max_cluster_size < 1:
        raise ValueError("max_cluster_size must be a positive integer")
    if initial_clusters is None:
        initial_clusters = (n + max_cluster_size - 1) // max_cluster_size
    if not isinstance(initial_clusters, (int, np.integer)) or not 1 <= initial_clusters <= n:
        raise ValueError("initial_clusters must be between 1 and N")
    if not isinstance(niter, int) or niter < 1:
        raise ValueError("niter must be positive")

    try:
        import faiss
    except ImportError as exc:
        raise ImportError("cluster_vectors requires faiss-cpu (or another FAISS Python build)") from exc

    dim = x.shape[1]
    if initial_clusters == n:
        centers = x.copy()
    else:
        training_index = faiss.IndexFlatL2(dim)
        kmeans = faiss.Clustering(dim, int(initial_clusters))
        kmeans.seed = int(seed)
        kmeans.niter = niter
        kmeans.nredo = 1
        kmeans.min_points_per_centroid = 1
        kmeans.max_points_per_centroid = (n + initial_clusters - 1) // initial_clusters
        kmeans.train(x, training_index)
        centers = faiss.vector_to_array(kmeans.centroids).reshape(initial_clusters, dim).copy()

    assignment_index = faiss.IndexFlatL2(dim)
    assignment_index.add(centers)
    labels = np.empty(n, dtype=np.int64)
    for start in range(0, n, 4096):
        chunk = x[start:start + 4096]
        _, assigned = assignment_index.search(chunk, 1)
        labels[start:start + len(chunk)] = assigned[:, 0]
    memberships = [np.flatnonzero(labels == c) for c in range(initial_clusters)]
    groups = _bounded_groups(x, memberships, max_cluster_size)
    return _assemble(x, groups, n)
