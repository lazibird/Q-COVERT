#!/usr/bin/env python3
"""Trusted offline HNSW/PQ preparation; never runs inside the query TEE."""
import argparse
import json
import os
from pathlib import Path
import struct
import numpy as np
from dataio import ROOT, add_dataset_arguments, resolve_paths, read_vectors, vecs

INVALID = (1 << 64) - 1

class Reader:
    def __init__(self, path):
        self.file = open(path, 'rb')
    def read(self, n):
        x = self.file.read(n)
        if len(x) != n:
            raise ValueError('truncated Compass/FAISS index')
        return x
    def get(self, fmt):
        return struct.unpack('<' + fmt, self.read(struct.calcsize('<' + fmt)))
    def array(self, dtype):
        n, = self.get('Q')
        if n > (os.fstat(self.file.fileno()).st_size - self.file.tell()) // np.dtype(dtype).itemsize:
            raise ValueError('invalid index array length')
        return np.frombuffer(self.read(n * np.dtype(dtype).itemsize), dtype=dtype).copy()
    def header(self):
        tag = self.read(4)
        d, n, _, _, trained, metric = self.get('iqqq?i')
        if metric > 1:
            self.get('f')
        if d <= 0 or n <= 0 or not trained or metric not in (0, 1):
            raise ValueError('unsupported/untrained index')
        return tag, d, n, metric

def read_hnsw(path):
    # Same field order as Compass/src/faiss/index_io.cpp. Both standard FAISS
    # files and the paper's truncated graph-only files are accepted.
    r = Reader(path)
    try:
        tag, dim, count, metric = r.header()
        if tag not in (b'IHNf', b'IHNp', b'IHNs'):
            raise ValueError(f'unsupported HNSW type {tag!r}')
        r.array('<f8')  # assign_probas
        cum = r.array('<i4')
        levels = r.array('<i4')
        offsets = r.array('<u8')
        neighbors = r.array('<i4')
        entry, maximum, construction, search, beam = r.get('iiiii')
        if len(levels) != count or len(offsets) != count + 1 or len(cum) < 2 or offsets[-1] != len(neighbors):
            raise ValueError('invalid HNSW graph dimensions')
        degree = int(cum[1])
        if degree <= 0 or not 0 <= entry < count or np.any(offsets[1:] < offsets[:-1]):
            raise ValueError('invalid HNSW graph bounds')
        if np.any(offsets[1:] - offsets[:-1] < degree):
            raise ValueError('HNSW base layer missing neighbor slots')
        rows = neighbors[offsets[:-1].astype('i8')[:, None] + np.arange(degree)]
        if np.any((rows < -1) | (rows >= count)):
            raise ValueError('invalid HNSW neighbor ID')
        if degree % 2 or len(cum) < 3 or int(cum[2] - cum[1]) != degree // 2:
            raise ValueError('Compass requires HNSW base degree = twice upper degree')
        upper_ids = np.flatnonzero(levels > 1)
        scan_ids = np.flatnonzero(levels > 2)
        if np.any(offsets[upper_ids] + int(cum[2]) > offsets[upper_ids + 1]):
            raise ValueError('HNSW layer-1 neighbor slots missing')
        upper_rows = neighbors[offsets[upper_ids].astype('i8')[:, None] + int(cum[1]) + np.arange(degree // 2)]
        if np.any((upper_rows < -1) | (upper_rows >= count)):
            raise ValueError('invalid HNSW upper neighbor ID')
        if np.any(~np.isin(upper_rows[upper_rows >= 0], upper_ids)):
            raise ValueError('layer-1 edge points to a node outside layer 1')
        return dim, count, metric, entry, rows, upper_ids, upper_rows, scan_ids
    finally:
        r.file.close()

def read_pq(path):
    r = Reader(path)
    try:
        tag, dim, count, metric = r.header()
        if tag not in (b'IxPQ', b'IxPq'):
            raise ValueError(f'expected IndexPQ, got {tag!r}')
        d, p, bits = r.get('QQQ')
        book = r.array('<f4')
        packed = r.array('u1')
        if d != dim or not p or d % p or not 1 <= bits <= 8 or len(book) != dim * (1 << bits):
            raise ValueError('unsupported PQ model layout')
        code_size = (p * bits + 7) // 8
        if len(packed) != count * code_size:
            raise ValueError('PQ code count mismatch')
        packed = packed.reshape(count, code_size)
        codes = np.empty((count, p), dtype='u1')
        for j in range(p):
            offset = j * bits
            value = packed[:, offset // 8].astype('u2') >> (offset % 8)
            if offset % 8 + bits > 8:
                value |= packed[:, offset // 8 + 1].astype('u2') << (8 - offset % 8)
            codes[:, j] = value & ((1 << bits) - 1)
        return dim, count, metric, int(p), int(bits), book, codes
    finally:
        r.file.close()

def prepare(args):
    resolve_paths(args, preparation=True)
    if (args.threads <= 0 or args.batch_size <= 0 or args.limit < 0 or args.hnsw_m <= 0 or
        args.ef_construction <= 0 or args.pq_subspaces <= 0 or not 1 <= args.pq_bits <= 8 or
        args.pq_iterations <= 0 or args.training_count < 1 << args.pq_bits):
        raise ValueError('invalid preparation parameters')
    if args.convert_prepared:
        return convert_prepared(args)
    import faiss
    faiss.omp_set_num_threads(args.threads)
    base = read_vectors(args.base, args.base_format, args.base_dtype)
    n, dim = base.shape
    if args.limit:
        base = base[:args.limit]
        n = len(base)
    for start in range(0, n, args.batch_size):
        chunk = base[start:start + args.batch_size]
        if not np.isfinite(chunk).all() or np.max(np.abs(chunk)) > 1e10:
            raise ValueError('invalid embedding values')
        if args.scalar == 'uint8' and (np.any(chunk < 0) or np.any(chunk > 255) or np.any(chunk != np.floor(chunk))):
            raise ValueError('uint8 storage requires exact integer embeddings in [0,255]')
    metric = 1 if args.metric == 'l2' else 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    graph_path = args.hnsw or args.output.with_suffix('.hnsw.index')
    pq_path = args.pq or args.output.with_suffix('.pq.index')
    if not args.hnsw:
        print(f'build HNSW: N={n}, D={dim}, M={args.hnsw_m}', flush=True)
        graph = faiss.IndexHNSWFlat(dim, args.hnsw_m, metric)
        graph.hnsw.efConstruction = args.ef_construction
        for start in range(0, n, args.batch_size):
            graph.add(np.ascontiguousarray(base[start:start + args.batch_size], dtype='f4'))
        faiss.write_index(graph, str(graph_path))
        del graph
    graph_data = read_hnsw(graph_path)
    gd, gn, gm, entry, neighbors = graph_data[:5]
    if (gd, gn, gm) != (dim, n, metric):
        raise ValueError('HNSW dimension/count/metric mismatch with base')
    if not args.pq:
        if dim % args.pq_subspaces or n < 1 << args.pq_bits:
            raise ValueError('PQ training requires D divisible by P and N >= centroid count')
        print(f'train PQ: P={args.pq_subspaces}, bits={args.pq_bits}', flush=True)
        pq = faiss.IndexPQ(dim, args.pq_subspaces, args.pq_bits, metric)
        pq.pq.cp.seed = args.seed
        pq.pq.cp.niter = args.pq_iterations
        rng = np.random.default_rng(args.seed)
        sample = rng.choice(n, min(n, args.training_count), replace=False)
        pq.train(np.ascontiguousarray(base[sample], dtype='f4'))
        for start in range(0, n, args.batch_size):
            pq.add(np.ascontiguousarray(base[start:start + args.batch_size], dtype='f4'))
        faiss.write_index(pq, str(pq_path))
        del pq
    pd, pn, pm, p, bits, book, codes = read_pq(pq_path)
    if (pd, pn, pm) != (dim, n, metric):
        raise ValueError('PQ dimension/count/metric mismatch with base')
    degree = neighbors.shape[1]
    scalar_id = 2 if args.scalar == 'uint8' else 1
    edge_type = np.dtype([('id', '<u8'), ('pq', 'u1', (p,))])
    node_type = np.dtype([('id', '<u8'), ('embedding', 'u1' if scalar_id == 2 else '<f4', (dim,)),
                          ('neighbors', edge_type, (degree,))])
    upper_type = np.dtype([('id', '<u8'), ('embedding', 'u1' if scalar_id == 2 else '<f4', (dim,)),
                           ('neighbors', edge_type, (degree // 2,))])
    temporary = args.output.with_suffix(args.output.suffix + '.pending')
    upper_ids, upper_rows, scan_ids = graph_data[5:]
    scan_level = 2 if len(scan_ids) else (1 if len(upper_ids) else 0)
    if not len(scan_ids):
        scan_ids = upper_ids if len(upper_ids) else np.asarray([entry])
    def attach(records, ids, start_slot=0):
        target = records['neighbors'][:, start_slot:start_slot + ids.shape[1]]
        target['id'] = ids.astype('<u8')
        target['pq'] = codes[np.maximum(ids, 0)]
        target['pq'][ids < 0] = 0
    def routing_records(ids, top=False):
        records = np.zeros(len(ids), dtype=upper_type)
        records['id'] = ids
        records['embedding'] = base[ids]
        records['neighbors']['id'] = INVALID
        if scan_level:
            attach(records, upper_rows[np.searchsorted(upper_ids, ids)])
        return records
    print(f'attach edge PQ: degree={degree}, node wire bytes={node_type.itemsize}', flush=True)
    with open(temporary, 'wb') as out:
        out.write(struct.pack('<9Q', int.from_bytes(b'CMPDO003', 'little'), dim, degree, p, 1 << bits,
                              scalar_id, 0 if metric == 1 else 1, n, entry))
        out.write(struct.pack('<3Q', len(upper_ids), len(scan_ids), scan_level))
        out.write(book.astype('<f4').tobytes())
        for start in range(0, n, args.batch_size):
            end = min(n, start + args.batch_size)
            records = np.zeros(end - start, dtype=node_type)
            records['id'] = np.arange(start, end)
            records['embedding'] = base[start:end]
            records['neighbors']['id'] = INVALID
            attach(records, neighbors[start:end])
            out.write(records.tobytes())
        for ids, top in ((upper_ids, False), (scan_ids, True)):
            for start in range(0, len(ids), args.batch_size):
                out.write(routing_records(ids[start:start + args.batch_size], top).tobytes())
    os.replace(temporary, args.output)
    metadata = {'dataset': args.dataset,
                'base_format': args.base.suffix.lower().lstrip('.') if args.base_format == 'auto' else args.base_format,
                'base_dtype': 'uint8' if base.dtype == np.dtype('u1') else 'float32',
                'batch_size': args.batch_size,
                'base': str(args.base.resolve()), 'hnsw': str(graph_path.resolve()), 'pq': str(pq_path.resolve()),
                'count': n, 'dimension': dim, 'degree': degree, 'pq_subspaces': p, 'pq_centroids': 1 << bits,
                'scalar': args.scalar, 'metric': args.metric, 'entry': entry, 'seed': args.seed,
                'index_bytes': args.output.stat().st_size,
                'prepared_version': 3,
                'upper_count': len(upper_ids), 'scan_count': len(scan_ids), 'scan_level': scan_level,
                'initialization': 'layer-2 scan + one layer-1 step + explicit full layer-0 entry + full C exact TopK'}
    args.output.with_suffix(args.output.suffix + '.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(f'prepared {args.output}\nCMake: -DCOMPASS_DIM={dim} -DCOMPASS_DEGREE={degree} '
          f'-DCOMPASS_PQ_SUBSPACES={p} -DCOMPASS_PQ_CENTROIDS={1 << bits} -DCOMPASS_SCALAR={args.scalar}', flush=True)

def convert_prepared(args):
    from prepared import prepared_full, node_dtype
    raw, header, book, nodes, upper, scan, level = prepared_full(args.convert_prepared, allow_legacy=True)
    dim, degree, p, centers, old_scalar, metric, n, entry = header
    if args.limit:
        raise ValueError('--convert-prepared cannot truncate an existing graph')
    scalar = 2 if args.scalar == 'uint8' else 1
    base = None
    if scalar != old_scalar:
        # Restore original vectors rather than casting a potentially lossy
        # compressed embedding. The imported graph and PQ stay identical.
        base = read_vectors(args.base, args.base_format, args.base_dtype)
        if base.shape != (n, dim): raise ValueError('conversion base shape mismatch')
    target_types = [node_dtype(dim, degree, p, scalar), node_dtype(dim, degree // 2, p, scalar)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + '.pending')
    if temporary.resolve() == args.convert_prepared.resolve(): raise ValueError('conversion source cannot be the pending output')
    with temporary.open('wb') as out:
        out.write(struct.pack('<12Q', int.from_bytes(b'CMPDO003', 'little'), dim, degree, p, centers,
                              scalar, metric, n, entry, len(upper), len(scan), level))
        out.write(book.astype('<f4').tobytes())
        for group, dtype in [(nodes, target_types[0]), (upper, target_types[1]), (scan, target_types[1])]:
            for start in range(0, len(group), args.batch_size):
                chunk = group[start:start + args.batch_size]
                result = np.zeros(len(chunk), dtype=dtype)
                result['id'] = chunk['id']
                embeddings = chunk['embedding'] if base is None else base[chunk['id']]
                if not np.isfinite(embeddings).all() or np.max(np.abs(embeddings)) > 1e10:
                    raise ValueError('invalid conversion embeddings')
                if scalar == 2 and (np.any(embeddings < 0) or np.any(embeddings > 255) or np.any(embeddings != np.floor(embeddings))):
                    raise ValueError('uint8 conversion requires exact integers in [0,255]')
                result['embedding'] = embeddings
                result['neighbors'] = chunk['neighbors'][:, :dtype['neighbors'].shape[0]]
                if group is scan and level == 0:
                    result['neighbors']['id'] = INVALID
                    result['neighbors']['pq'] = 0
                out.write(result.tobytes())
    metadata_path = args.convert_prepared.with_suffix(args.convert_prepared.suffix + '.json')
    metadata = json.loads(metadata_path.read_text()) if metadata_path.exists() else {}
    metadata.update(prepared_version=3, converted_from=str(args.convert_prepared.resolve()), scalar=args.scalar,
                    index_bytes=temporary.stat().st_size, initialization='full C exact TopK with separate layer ORAMs')
    os.replace(temporary, args.output)
    args.output.with_suffix(args.output.suffix + '.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(f'converted {args.output}; recompile with -DCOMPASS_SCALAR={args.scalar}')

def parser():
    p = argparse.ArgumentParser(description=__doc__)
    add_dataset_arguments(p)
    p.add_argument('--base', type=Path, help='override the dataset base path')
    p.add_argument('--base-format', choices=['auto', 'fvecs', 'bin'], default='auto', help='bin: uint32 shape header + vectors in --base-dtype')
    p.add_argument('--base-dtype', choices=['uint8', 'float32'], help='bin payload type (sift/deep1m: float32; sift10m/sift100m: uint8); fvecs is always float32')
    p.add_argument('--output', type=Path, help='override prepared output (default: data/compass/<dataset>.full-c.bin)')
    p.add_argument('--convert-prepared', type=Path, help='mmap-convert CMPDO002/003 without retraining; uses raw base if scalar changes')
    p.add_argument('--hnsw', type=Path, help='import existing Compass/FAISS HNSW (read only)')
    p.add_argument('--pq', type=Path, help='import existing Compass/FAISS IndexPQ (read only)')
    p.add_argument('--scalar', choices=['uint8', 'float32'], help='prepared embedding type (sift/deep1m: float32; sift10m/sift100m: uint8)')
    p.add_argument('--metric', choices=['l2', 'ip'], default='l2')
    p.add_argument('--hnsw-m', type=int, default=64)
    p.add_argument('--ef-construction', type=int, default=80)
    p.add_argument('--pq-subspaces', type=int, default=8)
    p.add_argument('--pq-bits', type=int, default=8)
    p.add_argument('--pq-iterations', type=int, default=20)
    p.add_argument('--training-count', type=int, default=65536)
    p.add_argument('--threads', type=int, default=32)
    p.add_argument('--seed', type=int, default=42)
    p.add_argument('--limit', type=int, default=0, help='public prefix size for smoke tests')
    p.add_argument('--batch-size', type=int, default=4096,
                   help='offline validation, FAISS add and packing chunk size; not the ORAM query batch')
    return p

if __name__ == '__main__':
    prepare(parser().parse_args())
