#!/usr/bin/env python3
"""Validate full-C exact recall and prepared data using mmap, without running ORAM.

This offline check uses the current plaintext reference and double distances.
It is not a private query path or an ORAM performance measurement.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from benchmark import prepared_full, recall
from plaintext import reference_scan
from dataio import profiles, add_dataset_arguments, resolve_paths, read_vectors, read_groundtruth
from prepare import INVALID, Reader, read_pq


class MappedHNSW:
    """Read upper layers without materializing the million-row bottom graph."""
    def __init__(self, path):
        reader = Reader(path)
        try:
            tag, self.dim, self.count, self.metric = reader.header()
            if tag not in (b'IHNf', b'IHNp', b'IHNs'):
                raise ValueError('unsupported HNSW index type')

            def array(dtype):
                count, = reader.get('Q')
                offset = reader.file.tell()
                size = count * np.dtype(dtype).itemsize
                if offset + size > Path(path).stat().st_size:
                    raise ValueError('truncated HNSW array')
                reader.file.seek(size, 1)
                return np.memmap(path, dtype=dtype, mode='r', offset=offset, shape=(count,))

            self.probabilities = array('<f8')
            self.cum = array('<i4')
            self.levels = array('<i4')
            self.offsets = array('<u8')
            self.edges = array('<i4')
            self.entry, self.maximum, _, _, _ = reader.get('iiiii')
            if (len(self.levels) != self.count or len(self.offsets) != self.count + 1 or
                    self.offsets[-1] != len(self.edges) or len(self.cum) <= self.maximum + 1 or
                    not 0 <= self.entry < self.count):
                raise ValueError('invalid HNSW dimensions')
        finally:
            reader.file.close()

    def neighbors(self, node, level):
        if not 0 <= node < self.count or not 0 <= level < self.levels[node]:
            raise ValueError('HNSW layer/ID mismatch')
        start = int(self.offsets[node]) + int(self.cum[level])
        stop = int(self.offsets[node]) + int(self.cum[level + 1])
        return self.edges[start:stop]



def run(args):
    resolve_paths(args)
    profile = profiles(args.config)[args.dataset]
    if args.base is None:
        args.base = args.data_root / profile['directory'] / profile['base']
    if args.base_dtype is None:
        args.base_dtype = profile['bin_dtype']
    _, header, book, nodes, upper, scan, scan_level = prepared_full(args.prepared)
    dim, degree, p, centers, scalar, metric, count, entry = header
    if not len(scan):
        raise ValueError('requires a CMPDO003 layer-specific index; repack the existing HNSW/PQ')
    metadata_path = args.prepared.with_suffix(args.prepared.suffix + '.json')
    metadata = json.loads(metadata_path.read_text()) if metadata_path.exists() else {}
    def source_path(explicit, suffix, field):
        default = args.prepared.with_suffix(suffix)
        return explicit or (default if default.exists() else Path(metadata.get(field, default)))
    graph = MappedHNSW(source_path(args.hnsw, '.hnsw.index', 'hnsw'))
    pd, pn, pm, pp, bits, pqbook, codes = read_pq(source_path(args.pq, '.pq.index', 'pq'))
    if (graph.dim, graph.count, graph.metric, graph.entry, int(graph.cum[1])) != (dim, count, 1 - metric, entry, degree):
        raise ValueError('source HNSW and prepared layout/entry mismatch')
    if (pd, pn, pm, pp, 1 << bits) != (dim, count, 1 - metric, p, centers) or not np.array_equal(pqbook.reshape(book.shape), book):
        raise ValueError('source PQ and prepared layout/codebook mismatch')
    base = read_vectors(args.base, args.base_format, args.base_dtype)
    queries = read_vectors(args.query, args.query_format, args.query_dtype)
    truth = read_groundtruth(args.groundtruth, args.gt_format)
    nq = min(args.queries, len(queries), len(truth))
    if (base.shape != (count, dim) or queries.shape[1] != dim or truth.shape[1] < args.k or
            nq <= 0 or min(args.ef, args.ef_spec, args.ef_n, args.k, args.upper_steps) <= 0):
        raise ValueError('invalid dataset/search dimensions or parameters')
    # Sequential batches bound transient RAM even with --audit-all.
    sample = (np.arange(count) if args.audit_all else
              np.unique(np.linspace(0, count - 1, min(count, 4096), dtype='i8')))
    for start in range(0, len(sample), 4096):
        ids = sample[start:start + 4096]
        if not np.array_equal(base[ids], nodes[ids]['embedding']):
            raise ValueError('prepared embeddings differ from decoded base vectors')
        if not np.array_equal(nodes[ids]['id'], ids):
            raise ValueError('prepared node ID mismatch')
    # Also check neighbor IDs and attached neighbor PQ on a fixed sample.
    for node in np.unique(np.linspace(0, count - 1, min(count, 256), dtype='i8')):
        source = graph.neighbors(int(node), 0)
        expected_ids = source.astype('u8')
        if not np.array_equal(expected_ids, nodes[node]['neighbors']['id']):
            raise ValueError('prepared bottom edges differ from source HNSW')
        valid = source >= 0
        if not np.array_equal(nodes[node]['neighbors']['pq'][valid], codes[source[valid]]):
            raise ValueError('attached neighbor PQ does not match source codes')
    if len(scan):
        upper_ids = np.flatnonzero(graph.levels > 1)
        scan_ids = np.flatnonzero(graph.levels > 2)
        expected_level = 2 if len(scan_ids) else (1 if len(upper_ids) else 0)
        if not len(scan_ids):
            scan_ids = upper_ids if len(upper_ids) else np.asarray([entry])
        if (scan_level != expected_level or not np.array_equal(upper['id'], upper_ids) or
                not np.array_equal(scan['id'], scan_ids)):
            raise ValueError('prepared routing pool differs from HNSW levels')
        half = degree // 2
        for start in range(0, len(upper), 1024):
            ids = upper_ids[start:start + 1024]
            rows = upper[start:start + 1024]
            if not np.array_equal(rows['embedding'], base[ids]):
                raise ValueError('prepared layer-1 embedding mismatch')
            source = graph.edges[graph.offsets[ids].astype('i8')[:, None] + int(graph.cum[1]) + np.arange(half)]
            edges = rows['neighbors'][:, :half]
            valid = source >= 0
            if (not np.array_equal(edges['id'], source.astype('u8')) or
                    not np.array_equal(edges['pq'][valid], codes[source[valid]])):
                raise ValueError('prepared layer-1 edges/PQ mismatch')
        if not np.array_equal(scan['embedding'], base[scan_ids]):
            raise ValueError('prepared scan embedding mismatch')
        if scan_level:
            expected = upper[np.searchsorted(upper_ids, scan_ids)].copy()
        else:
            expected = scan.copy()
            expected['embedding'] = nodes[scan_ids]['embedding']
            expected['neighbors']['id'] = INVALID
            expected['neighbors']['pq'] = 0
        if not np.array_equal(scan, expected):
            raise ValueError('prepared scan payload mismatch')
    outputs = [reference_scan(query, nodes, upper, scan, scan_level, book,
                              metric, args.ef, args.ef_spec, args.ef_n, args.k, args.upper_steps)
               for query in queries[:nq]]
    report = {'dataset': args.dataset, 'prepared': str(args.prepared), 'base_dtype': str(base.dtype),
              'stored_dtype': str(nodes.dtype['embedding'].base), 'query_dtype': str(queries.dtype),
              'embedding_vectors_checked': len(sample), 'embedding_equal': True,
              'neighbor_rows_checked': min(count, 256), 'neighbors_and_pq_equal': True,
              'count': count, 'queries': nq, 'ef': args.ef, 'ef_spec': args.ef_spec,
              'ef_n': args.ef_n, 'k': args.k,
              'upper_steps': args.upper_steps, 'entry': entry, 'max_level': graph.maximum,
              'prepared_version': 3, 'scan_count': len(scan), 'upper_count': len(upper),
              'routing_equal': True,
              'recall_at_k': {'full_c_exact': recall(outputs, truth[:nq], args.k)},
              'scope': 'current full-C exact plaintext reference; double distances; no ORAM'}
    encoded = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded)
    print(encoded, end='')


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.set_defaults(mode='native', csv=None)
    add_dataset_arguments(p)
    for name in ('prepared', 'base', 'query', 'groundtruth', 'hnsw', 'pq', 'output'):
        p.add_argument('--' + name, type=Path)
    p.add_argument('--base-format', choices=('auto', 'fvecs', 'bin'), default='auto')
    p.add_argument('--base-dtype', choices=('uint8', 'float32'))
    p.add_argument('--query-format', choices=('auto', 'fvecs', 'bin'), default='auto')
    p.add_argument('--query-dtype', choices=('uint8', 'float32'))
    p.add_argument('--gt-format', choices=('auto', 'ivecs', 'bin'), default='auto')
    p.add_argument('--queries', type=int, default=100)
    p.add_argument('--ef', type=int, default=20)
    p.add_argument('--ef-spec', type=int, default=4)
    p.add_argument('--ef-n', type=int, default=12)
    p.add_argument('--k', type=int, default=10)
    p.add_argument('--upper-steps', type=int, default=1)
    p.add_argument('--audit-all', action='store_true', help='compare all base/prepared embeddings instead of 4096 samples')
    return p


if __name__ == '__main__':
    args = parser().parse_args()
    try:
        run(args)
    except (ValueError, OSError) as error:
        parser().exit(2, f'Compass recall diagnostic: {error}\n')
