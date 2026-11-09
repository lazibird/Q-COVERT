"""Memory-mapped readers/writers for .bin vector files and FAISS-style vecs files.

.bin layout: base/query files are [uint32 n][uint32 dim][vectors...] with the
element type (uint8 or float32) inferred from the file length; groundtruth
files are [uint32 nq][uint32 K][uint32 ids...].
"""

import struct
from pathlib import Path

import numpy as np


def read_vector_bin(path):
    """Map a base/query .bin file as an N x D uint8 or float32 array."""
    path = Path(path)
    with path.open('rb') as stream:
        header = stream.read(8)
    if len(header) != 8:
        raise ValueError(f'truncated vector bin header: {path}')
    n, dim = struct.unpack('<II', header)
    payload = path.stat().st_size - 8
    if not n or not dim:
        raise ValueError(f'empty vector bin: {path}')
    if payload == n * dim:
        dtype = np.uint8
    elif payload == n * dim * 4:
        dtype = np.dtype('<f4')
    else:
        raise ValueError(f'vector bin size does not match n={n}, dim={dim}: {path}')
    return np.memmap(path, dtype=dtype, mode='r', offset=8, shape=(n, dim))


def read_groundtruth_bin(path):
    """Map a groundtruth .bin file as an NQ x K uint32 array."""
    path = Path(path)
    with path.open('rb') as stream:
        header = stream.read(8)
    if len(header) != 8:
        raise ValueError(f'truncated groundtruth bin header: {path}')
    nq, width = struct.unpack('<II', header)
    if not nq or not width or path.stat().st_size != 8 + nq * width * 4:
        raise ValueError(f'invalid groundtruth bin: {path}')
    return np.memmap(path, dtype='<u4', mode='r', offset=8, shape=(nq, width))


def read_vecs(path, kind, limit=None):
    """Map fvecs/ivecs, optionally validating and returning only a prefix."""
    if kind not in ('float', 'int'):
        raise ValueError("kind must be 'float' or 'int'")
    path = Path(path)
    with path.open('rb') as stream:
        header = stream.read(4)
    if len(header) != 4:
        raise ValueError(f'empty/truncated vecs file: {path}')
    dim, = struct.unpack('<I', header)
    size = path.stat().st_size
    record_size = 4 * (dim + 1)
    if not dim or (limit is None and size % record_size):
        raise ValueError(f'invalid vecs shape: {path}')
    rows = size // record_size
    if limit is not None:
        if limit <= 0 or limit > rows:
            raise ValueError(f'requested {limit} vectors, but {path} contains {rows} complete records')
        # DEEP1B shards can end mid-record; a complete requested prefix is sufficient.
        rows = limit
    values = np.memmap(path, dtype='<u4', mode='r', shape=(rows, dim + 1))
    if not np.all(values[:, 0] == dim):
        raise ValueError(f'inconsistent vecs dimensions: {path}')
    return values[:, 1:].view('<f4' if kind == 'float' else '<i4')


def write_vector_bin(path, values):
    """Write float32 vectors with one uint32 (count, dimension) header."""
    values = np.asarray(values)
    if values.ndim != 2 or not all(values.shape):
        raise ValueError('expected a nonempty N x D array')
    step = max(1, (1 << 24) // values.shape[1])
    with Path(path).open('wb') as out:
        out.write(struct.pack('<II', *values.shape))
        for start in range(0, len(values), step):
            out.write(np.asarray(values[start:start + step], dtype='<f4').tobytes())


def write_vecs(path, values, kind):
    """Write an N x D array as fvecs ('float') or ivecs ('int') in bounded chunks."""
    values = np.asarray(values)
    if values.ndim != 2 or not values.shape[0] or not values.shape[1]:
        raise ValueError('expected a nonempty N x D array')
    if kind not in ('float', 'int'):
        raise ValueError("kind must be 'float' or 'int'")
    dtype = np.dtype('<f4' if kind == 'float' else '<i4')
    dim = values.shape[1]
    step = max(1, (1 << 24) // (dim + 1))
    with Path(path).open('wb') as out:
        for start in range(0, len(values), step):
            part = np.empty((min(step, len(values) - start), dim + 1), dtype='<u4')
            part[:, 0] = dim
            part[:, 1:] = np.asarray(values[start:start + step], dtype=dtype).view('<u4')
            out.write(part.tobytes())


def read_labels_vecs(path):
    """Read a one-label-per-record ivecs file as a flat int64 array."""
    values = read_vecs(path, 'int')
    if values.shape[1] != 1:
        raise ValueError(f'expected one label per record: {path}')
    return np.asarray(values[:, 0], dtype=np.int64)
