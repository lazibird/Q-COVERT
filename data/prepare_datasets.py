#!/usr/bin/env python3
"""Prepare shared datasets and recompute exact L2 GT for every derived base."""
import argparse
import gzip
from pathlib import Path
import shutil
import struct
import tarfile
import tempfile

import numpy as np

from vector_io import read_vecs, read_vector_bin, write_vecs

DATA_ROOT = Path(__file__).resolve().parent
DATASETS = ('sift', 'sift10m', 'sift100m', 'msmarco1m', 'deep1m')


def log(message):
    print(message, flush=True)


def positive_count(text):
    value = str(text).lower().replace('_', '')
    scale = {'k': 1_000, 'm': 1_000_000}.get(value[-1:], 1)
    if scale != 1:
        value = value[:-1]
    if not value.isdigit() or int(value) <= 0:
        raise argparse.ArgumentTypeError('expected a positive integer, optionally ending in K or M')
    return int(value) * scale


def expand_gzip(path):
    """Preserve the archive and publish only a fully decompressed output."""
    path = Path(path)
    output = path.with_suffix('')
    if output.is_file() and output.stat().st_size:
        return output
    temporary = output.with_name(output.name + '.unpack.part')
    log(f'Decompressing {path}')
    try:
        with gzip.open(path, 'rb') as src, temporary.open('wb') as dst:
            shutil.copyfileobj(src, dst, length=1 << 20)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return output


class ByteVectors:
    """Read TEXMEX bvecs or legacy uint8 header-bin sources in bounded chunks."""
    def __init__(self, path):
        self.path = Path(path)
        self.records = None
        if self.path.suffix == '.bin':
            self.values = read_vector_bin(self.path)
            if self.values.dtype != np.uint8:
                raise ValueError(f'expected uint8 vectors: {self.path}')
        else:
            with self.path.open('rb') as stream:
                header = stream.read(4)
            if len(header) != 4:
                raise ValueError(f'truncated bvecs header: {self.path}')
            dim, = struct.unpack('<I', header)
            size = self.path.stat().st_size
            if not dim or size % (dim + 4):
                raise ValueError(f'invalid bvecs record size: {self.path}')
            self.records = np.memmap(self.path, dtype='u1', mode='r', shape=(size // (dim + 4), dim + 4))
            self.values = self.records[:, 4:]
        self.shape = self.values.shape

    def rows(self, start, end):
        if self.records is not None:
            dimensions = self.records[start:end, :4].copy().view('<u4')[:, 0]
            if np.any(dimensions != self.shape[1]):
                raise ValueError(f'inconsistent bvecs dimension: {self.path}')
        return self.values[start:end]


def find_bigann(root, role):
    folder = root / 'sift1B'
    path = folder / f'bigann_{role}.bvecs'
    if path.exists():
        return path
    if path.with_suffix('.bvecs.gz').exists():
        return expand_gzip(path.with_suffix('.bvecs.gz'))
    legacy = folder / f'{role}.bin'
    if legacy.exists():
        return legacy
    raise FileNotFoundError(f'missing BIGANN {role}: {path} or its .gz archive')


def check_sift(folder):
    base = read_vecs(folder / 'sift_base.fvecs', 'float')
    query = read_vecs(folder / 'sift_query.fvecs', 'float')
    truth = read_vecs(folder / 'sift_groundtruth.ivecs', 'int')
    if base.shape[1] != query.shape[1] or len(truth) != len(query):
        raise ValueError('SIFT base, query, and official GT shapes do not match')
    if np.any(truth < 0) or np.any(truth >= len(base)):
        raise ValueError('SIFT official GT contains out-of-range IDs')


def prepare_sift(root):
    folder = root / 'sift'
    required = ('sift_base.fvecs', 'sift_query.fvecs', 'sift_groundtruth.ivecs')
    if all((folder / name).is_file() for name in required):
        check_sift(folder)
        log('SIFT: keeping original vectors and official GT.')
        return
    folder.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.sift-', dir=folder) as temporary:
        stage = Path(temporary)
        # Read named regular files only; archive paths are never used as output paths.
        with tarfile.open(folder / 'sift.tar.gz', 'r:gz') as archive:
            for member in archive:
                name = Path(member.name).name
                if member.isfile() and name in (*required, 'sift_learn.fvecs'):
                    if (stage / name).exists():
                        raise ValueError(f'duplicate SIFT archive member: {name}')
                    with archive.extractfile(member) as src, (stage / name).open('wb') as dst:
                        shutil.copyfileobj(src, dst, length=1 << 20)
        check_sift(stage)
        for path in stage.iterdir():
            path.replace(folder / path.name)
    log('SIFT: extracted original vectors and official GT.')


def write_bin(path, count, dim, chunks, dtype):
    written = 0
    with Path(path).open('wb') as stream:
        stream.write(struct.pack('<II', count, dim))
        for chunk in chunks:
            chunk = np.ascontiguousarray(chunk, dtype=dtype)
            if chunk.ndim != 2 or chunk.shape[1] != dim:
                raise ValueError('unexpected vector chunk shape')
            written += len(chunk)
            stream.write(chunk.tobytes())
    if written != count:
        raise ValueError(f'expected {count} vectors, wrote {written}')


def exact_l2_gt(base, queries, k, base_batch, query_batch):
    """Merge exact top-k results from disjoint base blocks, bounding index memory."""
    import faiss

    if not 0 < k <= len(base):
        raise ValueError(f'cannot compute top-{k} with {len(base)} base vectors')
    best_distances = np.full((len(queries), k), np.inf, dtype='f4')
    best_ids = np.full((len(queries), k), -1, dtype='i8')
    for start in range(0, len(base), base_batch):
        end = min(start + base_batch, len(base))
        index = faiss.IndexFlatL2(base.shape[1])
        index.add(np.ascontiguousarray(base[start:end], dtype='f4'))
        width = min(k, end - start)
        for qstart in range(0, len(queries), query_batch):
            qend = min(qstart + query_batch, len(queries))
            distances, ids = index.search(np.ascontiguousarray(queries[qstart:qend], dtype='f4'), width)
            distances = np.concatenate((best_distances[qstart:qend], distances), axis=1)
            ids = np.concatenate((best_ids[qstart:qend], ids + start), axis=1)
            order = np.lexsort((ids, distances), axis=1)[:, :k]
            best_distances[qstart:qend] = np.take_along_axis(distances, order, axis=1)
            best_ids[qstart:qend] = np.take_along_axis(ids, order, axis=1)
        log(f'Exact L2 GT: searched base rows {start:,}:{end:,} / {len(base):,}')
    if np.any(best_ids < 0) or not np.isfinite(best_distances).all():
        raise ValueError('non-finite distances or incomplete GT; check input coordinates')
    return best_ids


def prepare_derived(root, dataset, count, seed, k, base_batch, query_batch):
    """Stage base/query/GT together; GT always uses the newly materialized base."""
    byte_base = None
    indices = None
    if dataset in ('sift10m', 'sift100m'):
        folder = root / ('sift10M' if dataset == 'sift10m' else 'sift100M')
        byte_base = ByteVectors(find_bigann(root, 'base'))
        byte_query = ByteVectors(find_bigann(root, 'query'))
        queries = np.asarray(byte_query.rows(0, byte_query.shape[0]), dtype='f4')
        source_count, dim = byte_base.shape
        dtype = 'u1'
    elif dataset == 'msmarco1m':
        folder = root / 'msmarco1m'
        base = read_vecs(folder / 'passages.fvecs', 'float')
        queries = read_vecs(folder / 'queries.fvecs', 'float')
        source_count, dim = base.shape
        if count <= source_count:
            indices = np.sort(np.random.default_rng(seed).choice(source_count, count, replace=False))
        dtype = '<f4'
    else:
        folder = root / 'deep1M'
        source = folder / 'source'
        base_path = source / 'base/base_00'
        if not base_path.exists():
            base_path = source / 'deep1M_base.fvecs'
        base = read_vecs(base_path, 'float', limit=count)
        queries = read_vecs(source / 'deep1B_queries.fvecs', 'float')
        source_count, dim = base.shape
        if dim != 96:
            raise ValueError('DEEP base vectors must have 96 dimensions')
        dtype = '<f4'
    if not 0 < k <= count <= source_count:
        raise ValueError(f'expected 0 < k <= count <= {source_count}; got k={k}, count={count}')
    if queries.shape[1] != dim:
        raise ValueError(f'query dim {queries.shape[1]} does not match base dim {dim}')
    if count > np.iinfo(np.int32).max:
        raise ValueError('base IDs exceed the shared ivecs int32 range')
    folder.mkdir(parents=True, exist_ok=True)
    names = ('base.bin', 'query.bin', 'gt.bin') if dataset == 'msmarco1m' else (
        'base.bin', f'{dataset}_query.fvecs', f'{dataset}_groundtruth.ivecs')
    log(f'{dataset}: preparing {count:,} x {dim} base vectors and {len(queries):,} original queries')
    with tempfile.TemporaryDirectory(prefix='.prepare-', dir=folder) as temporary:
        stage = Path(temporary)

        def chunks():
            for start in range(0, count, base_batch):
                end = min(start + base_batch, count)
                if byte_base is not None:
                    yield byte_base.rows(start, end)
                else:
                    yield base[indices[start:end]] if indices is not None else base[start:end]

        write_bin(stage / names[0], count, dim, chunks(), dtype)
        if dataset == 'msmarco1m':
            write_bin(stage / names[1], len(queries), dim,
                      (queries[start:start + base_batch] for start in range(0, len(queries), base_batch)), '<f4')
            processed_queries = read_vector_bin(stage / names[1])
        else:
            write_vecs(stage / names[1], queries, 'float')
            processed_queries = read_vecs(stage / names[1], 'float')
        processed_base = read_vector_bin(stage / names[0])
        truth = exact_l2_gt(processed_base, processed_queries, k, base_batch, query_batch)
        if dataset == 'msmarco1m':
            write_bin(stage / names[2], len(truth), k, [truth], '<u4')
        else:
            write_vecs(stage / names[2], truth, 'int')
        for name in names:
            (stage / name).replace(folder / name)
    log(f'{dataset}: wrote base, queries, and exact L2 top-{k} GT to {folder}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('datasets', nargs='*', choices=(*DATASETS, 'sift1b', 'bigann', 'all'),
                        help='default: all; sift1b/bigann selects both SIFT prefixes')
    parser.add_argument('--data-root', type=Path, default=DATA_ROOT,
                        help='shared data directory (default: beside this script)')
    for name, default in [('sift10m', 10_000_000), ('sift100m', 100_000_000),
                          ('msmarco', 1_000_000), ('deep', 1_000_000)]:
        parser.add_argument(f'--{name}-count', type=positive_count, default=default,
                            help=f'base subset size (default: {default:,}); accepts K/M suffixes')
    parser.add_argument('--seed', type=int, default=42, help='MSMARCO uniform sampling seed (default: 42)')
    parser.add_argument('--k', type=positive_count, default=100, help='exact L2 GT width (default: 100)')
    parser.add_argument('--base-batch', type=positive_count, default=100_000,
                        help='base rows per copy/search block (default: 100000)')
    parser.add_argument('--query-batch', type=positive_count, default=100,
                        help='query rows per search batch (default: 100)')
    parser.add_argument('--faiss-threads', type=int, default=0,
                        help='FAISS OpenMP threads; 0 keeps the library default')
    args = parser.parse_args()
    if args.faiss_threads < 0:
        parser.error('--faiss-threads must be nonnegative')
    selected = args.datasets or ['all']
    if 'all' in selected:
        selected = list(DATASETS)
    selected = list(dict.fromkeys(item for name in selected
                                 for item in (('sift10m', 'sift100m') if name in ('sift1b', 'bigann') else (name,))))
    if any(name != 'sift' for name in selected):
        import faiss
        if args.faiss_threads:
            faiss.omp_set_num_threads(args.faiss_threads)
    counts = {'sift10m': args.sift10m_count, 'sift100m': args.sift100m_count,
              'msmarco1m': args.msmarco_count, 'deep1m': args.deep_count}
    for name in selected:
        if name == 'sift':
            prepare_sift(args.data_root)
        else:
            prepare_derived(args.data_root, name, counts[name], args.seed, args.k,
                            args.base_batch, args.query_batch)


if __name__ == '__main__':
    main()
