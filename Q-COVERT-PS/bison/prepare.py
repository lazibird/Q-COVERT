#!/usr/bin/env python3
"""Trusted offline BiSON index preparation: build the DiskANN/Vamana graph from the raw base set with the vendored builder, then import and validate it."""
import argparse
from collections import deque
import json
import os
from pathlib import Path
import struct
import subprocess
import numpy as np
from dataio import ROOT, add_dataset_arguments, resolve_paths, read_vectors, headered_bin
INVALID = (1 << 64) - 1
MAGIC = int.from_bytes(b'BISDO001', 'little')
SECTOR = 4096
DEFAULT_BUILDER = ROOT / 'build/diskann-builder/apps/build_disk_index'

def array_bin(path, dtype, offset=0):
    path = Path(path)
    with path.open('rb') as f:
        f.seek(offset); header = f.read(8)
    if len(header) != 8: raise ValueError(f'truncated bin header: {path}')
    n, d = struct.unpack('<II', header)
    dt = np.dtype(dtype)
    end = offset + 8 + int(n) * int(d) * dt.itemsize
    if not n or not d or end > path.stat().st_size: raise ValueError(f'invalid bin section: {path}')
    return np.memmap(path, dtype=dt, mode='r', offset=offset+8, shape=(n, d))

def node_dtype(dim, degree, p, scalar):
    edge = np.dtype([('id', '<u8'), ('pq', 'u1', (p,))])
    return np.dtype([('id', '<u8'), ('degree', '<u4'), ('reserved', '<u4'),
                     ('embedding', 'u1' if scalar == 'uint8' else '<f4', (dim,)),
                     ('pq', 'u1', (p,)), ('neighbors', edge, (degree,))])

def finite(values, name):
    if not np.all(np.isfinite(values)) or np.any(np.abs(values) > 1e4):
        raise ValueError(f'{name} must be finite and within +/-1e4')

def read_model(prefix):
    pivots = Path(str(prefix) + '_pq_pivots.bin')
    offsets = array_bin(pivots, '<u8').reshape(-1)
    if len(offsets) not in (4, 5): raise ValueError('PQ pivots need 4/5 offset layout')
    book = np.asarray(array_bin(pivots, '<f4', int(offsets[0])))
    mean = np.asarray(array_bin(pivots, '<f4', int(offsets[1]))).reshape(-1)
    chunks = np.asarray(array_bin(pivots, '<u4', int(offsets[3 if len(offsets) == 5 else 2]))).reshape(-1)
    if book.shape[0] != 256 or len(mean) != book.shape[1] or len(chunks) < 2:
        raise ValueError('PQ pivot/mean/chunk shape mismatch')
    dim = len(mean)
    if not 0 < dim <= 4096 or chunks[0] != 0 or chunks[-1] != dim or np.any(chunks[1:] <= chunks[:-1]):
        raise ValueError('invalid PQ chunk offsets/dimension')
    rotation_path = Path(str(pivots) + '_rotation_matrix.bin')
    rotation = np.zeros((dim, dim), '<f4')
    if rotation_path.exists():
        rotation = np.asarray(array_bin(rotation_path, '<f4'))
        if rotation.shape != (dim, dim): raise ValueError('OPQ rotation shape mismatch')
    for data, label in [(book, 'PQ pivots'), (mean, 'PQ mean'), (rotation, 'OPQ rotation')]: finite(data, label)
    codes_path = Path(str(prefix) + '_pq_compressed.bin')
    codes = array_bin(codes_path, 'u1')
    if codes.shape[1] != len(chunks) - 1 or codes_path.stat().st_size != 8 + codes.size:
        raise ValueError('PQ code length/shape mismatch')
    return book.T.copy().astype('<f4'), mean.astype('<f4'), chunks.astype('<u4'), rotation.astype('<f4'), int(rotation_path.exists()), codes

class DiskGraph:
    def __init__(self, prefix, scalar):
        self.path = Path(str(prefix) + '_disk.index')
        if Path(str(self.path) + '_pq_pivots.bin').exists(): raise ValueError('disk-PQ graph is unsupported; import full embeddings')
        if any(Path(str(self.path)+suffix).exists() for suffix in
               ('_labels.txt', '_labels_to_medoids.txt', '_dummy_map.txt', '_labels_map.txt', '_universal_label.txt')):
            raise ValueError('filtered/dummy-mapped DiskANN indexes are not supported')
        self.raw = np.memmap(self.path, dtype='u1', mode='r')
        if len(self.raw) < SECTOR: raise ValueError('truncated DiskANN metadata')
        nr, nc = struct.unpack_from('<II', self.raw)
        if nc != 1 or not 8 <= nr <= (SECTOR - 8)//8: raise ValueError('unsupported DiskANN metadata format')
        meta = struct.unpack_from(f'<{nr}Q', self.raw, 8)
        self.n, self.dim, self.entry, self.length, self.per_sector, frozen, frozen_id, reorder = meta[:8]
        if not self.n or not 0 < self.dim <= 4096 or self.entry >= self.n or frozen or reorder:
            raise ValueError('invalid/unsupported DiskANN dimension, frozen points or reorder data')
        self.scalar = np.dtype('u1' if scalar == 'uint8' else '<f4')
        payload = self.dim * self.scalar.itemsize
        if self.length < payload + 4 or (self.length-payload) % 4: raise ValueError('DiskANN scalar/layout mismatch')
        self.degree = (self.length-payload)//4 - 1
        if not self.degree: raise ValueError('empty DiskANN degree')
        self.sectors_per_node = (self.length+SECTOR-1)//SECTOR
        if self.per_sector:
            if self.per_sector != SECTOR//self.length: raise ValueError('DiskANN nodes-per-sector mismatch (scalar/layout mismatch)')
            sectors = (self.n+self.per_sector-1)//self.per_sector
        else:
            if self.length <= SECTOR: raise ValueError('invalid multi-sector layout')
            sectors = self.n*self.sectors_per_node
        if len(self.raw) != (sectors+1)*SECTOR: raise ValueError('DiskANN file length mismatch')
    def read(self, i):
        if self.per_sector: start = (i//self.per_sector+1)*SECTOR + (i%self.per_sector)*self.length
        else: start = SECTOR + i*self.sectors_per_node*SECTOR
        embedding = np.ndarray((self.dim,), self.scalar, buffer=self.raw, offset=start)
        degree = struct.unpack_from('<I', self.raw, start+self.dim*self.scalar.itemsize)[0]
        if degree > self.degree: raise ValueError(f'node {i} degree exceeds R (scalar/layout mismatch)')
        ids = np.ndarray((degree,), '<u4', buffer=self.raw, offset=start+self.dim*self.scalar.itemsize+4)
        if np.any(ids >= self.n): raise ValueError(f'node {i} has out-of-range neighbor')
        return embedding, ids

def verify_cache(nodes, medoids, cache, h):
    """Offline, provider-side validation; ordinary ID indexing is allowed here."""
    n = len(nodes)
    ids = list(map(int, cache['id']))
    if len(set(ids)) != len(ids) or any(i >= n for i in ids): raise ValueError('invalid cache IDs')
    for i, record in zip(ids, cache):
        if record.tobytes() != nodes[i].tobytes(): raise ValueError('cache payload differs from graph')
    if not h: return
    members = set(ids)
    frontier = set(map(int, medoids['id']))
    reached = set()
    for _ in range(h):
        if not frontier <= members: raise ValueError('cached prefix is not covered by complete BFS layers')
        reached.update(frontier)
        nxt = set()
        for i in frontier:
            row = nodes[i]
            nxt.update(map(int, row['neighbors']['id'][:int(row['degree'])]))
        frontier = nxt-reached
        if not frontier: break

def prepared_full(path, validate_cache=True):
    raw = np.memmap(path, dtype='u1', mode='r')
    if len(raw) < 96: raise ValueError('truncated BiSON prepared header')
    fields = struct.unpack_from('<12Q', raw)
    magic, dim, degree, p, sid, n, m, c, h, rotated, metric, reserved = fields
    if magic != MAGIC or not 0 < dim <= 4096 or not degree or not 0 < p <= dim or sid not in (1, 2) or not n or not 0 < m <= n or c > n or rotated > 1 or metric or reserved or h > 1000000:
        raise ValueError('invalid BiSON prepared layout/counts/metric')
    scalar = 'uint8' if sid == 2 else 'float32'
    dtype = node_dtype(dim, degree, p, scalar)
    medtype = np.dtype([('id', '<u8'), ('centroid', '<f4', (dim,)), ('pq', 'u1', (p,))])
    model_bytes = 4*(256*dim+dim+(dim*dim)+(p+1))
    if len(raw) != 96+model_bytes+m*medtype.itemsize+(n+c)*dtype.itemsize:
        raise ValueError('BiSON prepared length mismatch')
    offset = 96
    def view(dtype, shape):
        nonlocal offset
        arr = np.ndarray(shape, dtype, buffer=raw, offset=offset); offset += arr.nbytes
        return arr
    book = view('<f4', (dim, 256)); mean = view('<f4', (dim,)); chunks = view('<u4', (p+1,)); rotation = view('<f4', (dim, dim))
    medoids = view(medtype, (m,)); nodes = view(dtype, (n,)); cache = view(dtype, (c,))
    if chunks[0] or chunks[-1] != dim or np.any(chunks[1:] <= chunks[:-1]): raise ValueError('invalid prepared chunks')
    for data, name in [(book,'pivots'), (mean,'mean'), (rotation,'rotation'), (medoids['centroid'],'medoid centroid')]: finite(data, name)
    if np.any(medoids['id'] >= n): raise ValueError('invalid medoid IDs')
    for start in range(0, n, 4096):
        rows = nodes[start:start+4096]
        if not np.array_equal(rows['id'], np.arange(start,start+len(rows),dtype='u8')) or np.any(rows['degree'] > degree) or np.any(rows['reserved']): raise ValueError('invalid node IDs/degree/reserved')
        mask = np.arange(degree)[None,:] < rows['degree'][:,None]
        ids = rows['neighbors']['id']
        if np.any(ids[mask] >= n) or np.any(ids[~mask] != INVALID): raise ValueError('invalid neighbor padding/range')
        finite(rows['embedding'], 'embedding')
    if validate_cache: verify_cache(nodes, medoids, cache, h)
    layout = {'dimension':dim,'degree':degree,'pq_subspaces':p,'pq_centroids':256,'scalar':scalar}
    return raw, layout, (book,mean,chunks,rotation,rotated), medoids, nodes, cache, h

def clean_index_prefix(prefix):
    prefix = Path(prefix)
    for path in prefix.parent.glob(prefix.name + '*'): path.unlink()

def build(args):
    builder = Path(getattr(args, "builder", None) or DEFAULT_BUILDER)
    if not builder.is_file():
        raise ValueError(f'DiskANN builder not found: {builder}; see the repository README for compilation, '
                         'or pass --builder /path/to/build_disk_index')
    if args.degree <= 0 or args.build_l <= 0 or args.pq_bytes <= 0 or not np.isfinite(args.build_ram_gb) or args.build_ram_gb <= 0 or not np.isfinite(args.alpha) or args.alpha <= 1:
        raise ValueError('invalid DiskANN build R/L/P/RAM/alpha')
    if args.pq_budget_gb is not None and (not np.isfinite(args.pq_budget_gb) or args.pq_budget_gb <= 0):
        raise ValueError('PQ memory budget must be positive and finite')
    clean_index_prefix(args.index_prefix)
    base = read_vectors(args.base, args.base_format, args.base_dtype)
    n, dim = base.shape
    binary = args.index_prefix.parent/'builder_base.bin'
    args.index_prefix.parent.mkdir(parents=True, exist_ok=True)
    dtype = 'u1' if args.scalar == 'uint8' else '<f4'
    with binary.open('wb') as out:
        out.write(struct.pack('<II', n, dim))
        for start in range(0, n, 65536):
            rows = base[start:start+65536]
            finite(rows,'base')
            if dtype == 'u1' and (np.any(rows < 0) or np.any(rows > 255) or np.any(rows != np.floor(rows))): raise ValueError('cannot losslessly build uint8 graph')
            out.write(np.asarray(rows,dtype=dtype).tobytes())
    env = dict(os.environ)
    venv_lib = ROOT.parent / '.venv/lib'
    if venv_lib.is_dir() and list(venv_lib.glob('libmkl_core.so*')):
        env['LD_LIBRARY_PATH'] = str(venv_lib) + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    subprocess.run([str(builder.resolve()), '--data_type', 'uint8' if dtype=='u1' else 'float',
        '--dist_fn','l2','--data_path',str(binary),'--index_path_prefix',str(args.index_prefix),
        '-R',str(args.degree),'-L',str(args.build_l),'-B',str(args.pq_budget_gb or (n*args.pq_bytes/1e9)),
        '-M',str(args.build_ram_gb),'-A',str(args.alpha),'--QD',str(args.pq_bytes)], check=True, env=env)

def run(args):
    resolve_paths(args, True)
    if args.pq_bytes is None: args.pq_bytes = 32 if args.dataset in ('SIFT10M', 'SIFT100M') else 16
    args.output = args.output.resolve(); args.index_prefix = args.index_prefix.resolve()
    args.index_scalar = args.scalar
    if args.cache_depth != 0: raise ValueError('Appendix F requires --cache-depth 0; every round reads W ORAM slots')
    sources = [args.base] + [Path(str(args.index_prefix)+suffix) for suffix in
        ('_disk.index', '_pq_compressed.bin', '_pq_pivots.bin', '_pq_pivots.bin_rotation_matrix.bin',
         '_disk.index_medoids.bin', '_disk.index_centroids.bin')]
    if any(args.output == path.resolve() for path in sources): raise ValueError('output must differ from source files')
    required = [Path(str(args.index_prefix)+suffix) for suffix in ('_disk.index', '_pq_compressed.bin', '_pq_pivots.bin')]
    if args.import_only:
        pass  # Internal testing: import files that already exist at --index-prefix.
    elif not args.rebuild and all(path.exists() for path in required):
        print(f'reusing existing DiskANN index at {args.index_prefix}; pass --rebuild to rebuild from the base set')
    else:
        build(args)
    graph = DiskGraph(args.index_prefix, args.index_scalar)
    book, mean, chunks, rotation, rotated, codes = read_model(args.index_prefix)
    if codes.shape[0] != graph.n or book.shape[0] != graph.dim: raise ValueError('graph/PQ count or dimension mismatch')
    p = codes.shape[1]; dtype = node_dtype(graph.dim,graph.degree,p,args.scalar)
    medpath = Path(str(graph.path)+'_medoids.bin')
    medids = list(map(int,array_bin(medpath,'<u4').reshape(-1))) if medpath.exists() else [graph.entry]
    if not medids or len(set(medids)) != len(medids) or any(i >= graph.n for i in medids): raise ValueError('invalid medoids')
    centroidpath = Path(str(graph.path)+'_centroids.bin')
    centroid = np.asarray(array_bin(centroidpath,'<f4')) if centroidpath.exists() else np.asarray([graph.read(i)[0] for i in medids],dtype='<f4')
    if centroid.shape[0] != len(medids) or centroid.shape[1] < graph.dim or np.any(centroid[:,graph.dim:]): raise ValueError('centroid metadata mismatch')
    finite(centroid, 'centroid')
    medtype = np.dtype([('id','<u8'),('centroid','<f4',(graph.dim,)),('pq','u1',(p,))])
    medoids = np.zeros(len(medids),medtype)
    medoids['id']=medids; medoids['centroid']=centroid[:,:graph.dim]; medoids['pq']=codes[medids]
    cache_ids=[]  # Appendix F retrieves every beam through ORAM.
    args.output.parent.mkdir(parents=True,exist_ok=True)
    # Public preparation uses atomic replacement, preserving existing artifacts on failure.
    pending=args.output.with_name(args.output.name+'.pending')
    try:
        header=[MAGIC,graph.dim,graph.degree,p,2 if args.scalar=='uint8' else 1,graph.n,len(medids),len(cache_ids),args.cache_depth,rotated,0,0]
        with pending.open('wb') as out:
            out.write(struct.pack('<12Q',*header))
            for arr in (book,mean,chunks,rotation,medoids): out.write(arr.tobytes())
            node_start=out.tell()
            out.truncate(node_start+(graph.n+len(cache_ids))*dtype.itemsize)
        prepared=np.memmap(pending,dtype=dtype,mode='r+',offset=node_start,shape=(graph.n+len(cache_ids),))
        base=read_vectors(args.base,args.base_format,args.base_dtype) if args.base.exists() else None
        if base is not None and base.shape != (graph.n,graph.dim): raise ValueError('base and DiskANN graph shape mismatch')
        for start in range(0,graph.n,4096):
            rows=np.zeros(min(4096,graph.n-start),dtype)
            rows['neighbors']['id']=INVALID
            for j in range(len(rows)):
                i=start+j; emb,ids=graph.read(i); finite(emb,'graph embedding')
                if base is not None and not np.array_equal(emb,base[i]): raise ValueError(f'graph embedding differs from base at ID {i}')
                if args.scalar=='uint8' and (np.any(emb<0) or np.any(emb>255) or np.any(emb!=np.floor(emb))): raise ValueError('graph cannot be losslessly stored as uint8')
                rows[j]['id']=i; rows[j]['degree']=len(ids); rows[j]['embedding']=emb; rows[j]['pq']=codes[i]
                rows[j]['neighbors']['id'][:len(ids)]=ids; rows[j]['neighbors']['pq'][:len(ids)]=codes[ids]
            prepared[start:start+len(rows)]=rows
        for j,i in enumerate(cache_ids): prepared[graph.n+j]=prepared[i]
        prepared.flush(); del prepared
        prepared_full(pending)
        os.replace(pending,args.output)
    finally:
        if pending.exists(): pending.unlink()
    manifest={'format':'BISDO001','dataset':args.dataset,'n':graph.n,'dimension':graph.dim,'degree':graph.degree,
        'pq_bytes':p,'scalar':args.scalar,'index_scalar':args.index_scalar,'cache_nodes':len(cache_ids),
        'cached_rounds':args.cache_depth,'index_prefix':str(args.index_prefix),'prepared':str(args.output)}
    args.output.with_suffix(args.output.suffix+'.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest,indent=2))
    return manifest

def parser():
    p=argparse.ArgumentParser(description=__doc__); add_dataset_arguments(p)
    p.add_argument('--builder',type=Path,help='DiskANN build_disk_index executable; see root README')
    p.add_argument('--index-prefix',type=Path,help='DiskANN builder output prefix; an existing complete index there is reused unless --rebuild')
    p.add_argument('--base',type=Path); p.add_argument('--base-format',choices=['auto','fvecs','bin'],default='auto')
    p.add_argument('--base-dtype',choices=['uint8','float32']); p.add_argument('--scalar',choices=['uint8','float32'])
    p.add_argument('--output',type=Path); p.add_argument('--cache-depth',type=int,default=0,choices=[0],help='compatibility option; Appendix F requires 0 (no BFS query cache)')
    p.add_argument('--rebuild',action='store_true',help='rebuild the DiskANN graph from the base set even if the prefix already has one')
    p.add_argument('--import-only',action='store_true',help=argparse.SUPPRESS)  # internal: pipeline tests import fabricated fixtures
    for name,typ,default in [('degree',int,128),('build-l',int,125),('pq-bytes',int,None),('pq-budget-gb',float,None),('build-ram-gb',float,4),('alpha',float,1.2)]: p.add_argument('--'+name,type=typ,default=default)
    return p
if __name__=='__main__':
    cli=parser()
    try: run(cli.parse_args())
    except (ValueError,OSError,subprocess.CalledProcessError) as e: cli.exit(2,f'error: {e}\n')
