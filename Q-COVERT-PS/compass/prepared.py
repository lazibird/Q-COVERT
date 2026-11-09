"""Strict mmap reader for Compass layer-specific prepared blocks."""
import struct
import numpy as np

def node_dtype(dim, degree, p, scalar):
    edge = np.dtype([('id', '<u8'), ('pq', 'u1', (p,))])
    return np.dtype([('id', '<u8'), ('embedding', 'u1' if scalar == 2 else '<f4', (dim,)),
                     ('neighbors', edge, (degree,))])

def prepared_full(path, allow_legacy=False):
    data = np.memmap(path, dtype='u1', mode='r')
    if len(data) < 96:
        raise ValueError('truncated prepared hierarchy')
    magic, dim, degree, p, centers, scalar, metric, n, entry = struct.unpack_from('<9Q', data)
    legacy = magic == int.from_bytes(b'CMPDO002', 'little')
    if magic != int.from_bytes(b'CMPDO003', 'little') and not (allow_legacy and legacy):
        raise ValueError('CMPDO003 required; use prepare.py --convert-prepared <CMPDO002> or repack HNSW/PQ')
    if not n or entry >= n or scalar not in (1, 2) or metric not in (0, 1):
        raise ValueError('invalid prepared header')
    if not dim or not p or dim % p or not degree or degree % 2 or not 0 < centers <= 256:
        raise ValueError('invalid prepared dimensions')
    upper_count, scan_count, scan_level = struct.unpack_from('<3Q', data, 72)
    if (upper_count > n or not 0 < scan_count <= n or scan_level > 2 or
            (scan_level == 0 and (upper_count or scan_count != 1)) or (scan_level != 0 and not upper_count)):
        raise ValueError('invalid prepared hierarchy counts')
    bottom_type = node_dtype(dim, degree, p, scalar)
    upper_type = bottom_type if legacy else node_dtype(dim, degree // 2, p, scalar)
    prefix = 96 + dim * centers * 4
    if len(data) != prefix + n * bottom_type.itemsize + (upper_count + scan_count) * upper_type.itemsize:
        raise ValueError('prepared length mismatch')
    book = np.ndarray((p, centers, dim // p), dtype='<f4', buffer=data, offset=96)
    nodes = np.ndarray((n,), dtype=bottom_type, buffer=data, offset=prefix)
    upper = np.ndarray((upper_count,), dtype=upper_type, buffer=data, offset=prefix + n * bottom_type.itemsize)
    scan = np.ndarray((scan_count,), dtype=upper_type, buffer=data,
                      offset=prefix + n * bottom_type.itemsize + upper_count * upper_type.itemsize)
    return data, (dim, degree, p, centers, scalar, metric, n, entry), book, nodes, upper, scan, scan_level

def prepared(path):
    return prepared_full(path)[:4]
