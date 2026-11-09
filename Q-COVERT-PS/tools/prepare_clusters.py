#!/usr/bin/env python3
"""Offline clustering and portable C++ quantizer input; no privacy claim."""
import argparse
import struct
from pathlib import Path
import numpy as np
from clustering import cluster_vectors, split_clusters
from vector_io import read_labels_vecs, read_vector_bin, read_vecs


def load_vectors(input_path):
    """Load an .npy matrix or map a base .bin file ([uint32 n][uint32 dim][vectors])."""
    path = Path(input_path)
    if path.suffix == '.npy':
        return np.load(path, allow_pickle=False)
    return read_vector_bin(path)


def prepare(input_path, output_path, tau, public_clusters=None, initial_clusters=None, seed=42,
            niter=20, precomputed_labels=None, precomputed_centroids=None):
    vectors = load_vectors(input_path)
    if vectors.dtype not in (np.dtype('uint8'), np.dtype('float32')):
        raise ValueError('input must be a float32 or uint8 N x D .npy/.bin array')
    if precomputed_labels is not None:
        # Existing unbounded assignment (e.g. external k-means): only split oversized groups.
        labels = read_labels_vecs(precomputed_labels)
        centroid_count = None
        if precomputed_centroids is not None:
            centroids = read_vecs(precomputed_centroids, 'float')
            centroid_count = centroids.shape[0]
        result = split_clusters(vectors, labels, tau, centroid_count)
    else:
        result = cluster_vectors(vectors, tau, initial_clusters, niter=niter, seed=seed)
    n, dim = vectors.shape
    nc = len(result.centroids)
    if public_clusters is None:
        public_clusters = nc
    if public_clusters < nc:
        raise ValueError(f'public cluster capacity {public_clusters} is smaller than actual count {nc}')
    with Path(output_path).open('wb') as out:
        # magic, dimension, scalar tag, N, actual nc, tau, padded public nc
        out.write(struct.pack('<7Q', 0x31534C4356435151, dim,
                              2 if vectors.dtype == np.uint8 else 1, n, nc, tau, public_clusters))
        out.write(np.asarray(result.centroids, dtype='<f4').tobytes(order='C'))
        out.write(np.asarray(result.labels, dtype='<u8').tobytes(order='C'))
        scalar = np.dtype('u1') if vectors.dtype == np.uint8 else np.dtype('<f4')
        step = max(1, (1 << 26) // (dim * scalar.itemsize))
        for start in range(0, n, step):
            block = np.ascontiguousarray(vectors[start:start + step], dtype=scalar)
            out.write(block.tobytes(order='C'))
    return n, dim, nc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', help='float32 or uint8 .npy matrix, or base .bin file')
    parser.add_argument('output', help='clustered intermediate binary for qcovert_prepare')
    parser.add_argument('--tau', type=int, required=True)
    parser.add_argument('--public-clusters', type=int,
                        help='public padded cluster capacity, selected independently of secret '
                             'assignments (default: actual cluster count)')
    parser.add_argument('--initial-clusters', type=int)
    parser.add_argument('--precomputed-labels',
                        help='ivecs file with one existing cluster id per base vector; '
                             'skips k-means and only splits oversized clusters')
    parser.add_argument('--precomputed-centroids',
                        help='fvecs file of the existing unbounded centroids (count validation)')
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--niter', type=int, default=20)
    args = parser.parse_args()
    if args.precomputed_centroids and not args.precomputed_labels:
        parser.error('--precomputed-centroids requires --precomputed-labels')
    n, dim, nc = prepare(args.input, args.output, args.tau, args.public_clusters,
                         args.initial_clusters, args.seed, args.niter,
                         args.precomputed_labels, args.precomputed_centroids)
    print(f'Offline clustered {n} vectors, dimension {dim}, actual clusters {nc}.')


if __name__ == '__main__':
    main()
