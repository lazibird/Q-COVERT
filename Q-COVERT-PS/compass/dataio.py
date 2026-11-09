"""Public dataset profiles and memory-mapped fvecs/ivecs/headered-bin readers."""
from pathlib import Path
import struct
import json
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = ROOT / 'config/dataset_defaults.json'

def profiles(config):
    result = {}
    for name, profile in json.loads(Path(config).read_text())['datasets'].items():
        files = {key: Path(value) for key, value in profile['files'].items()}
        scalar = 'float32' if name == 'SIFT' else profile['data']['storage_scalar']
        result[name.lower()] = {'directory': files['base'].parent.name,
            **{key: value.name for key, value in files.items()}, 'files': files,
            'prepared': name + '.full-c.bin', 'bin_dtype': scalar, 'scalar': scalar}
    return result

DATASETS = profiles(DEFAULTS)

def add_dataset_arguments(parser):
    parser.add_argument('--dataset', type=str.lower, choices=DATASETS, default='sift')
    parser.add_argument('--config', type=Path, default=DEFAULTS)
    parser.add_argument('--data-root', type=Path, default=ROOT.parent / 'data',
                        help='root containing the lowercase dataset directories')

def resolve_paths(args, preparation=False):
    profile = profiles(args.config)[args.dataset]
    def source(name):
        value = profile['files'][name]
        if value.is_absolute(): return value
        if value.parts[0] == 'data': return args.data_root.joinpath(*value.parts[1:])
        return ROOT.parent / value
    for name in ('base', 'query', 'groundtruth'):
        if hasattr(args, name) and getattr(args, name) is None: setattr(args, name, source(name))
    if preparation:
        if args.base_dtype is None: args.base_dtype = profile['bin_dtype']
        if args.scalar is None: args.scalar = profile['scalar']
        if args.output is None: args.output = ROOT.parent / 'data/prepared/ps/compass' / profile['prepared']
    else:
        if args.query_dtype is None: args.query_dtype = profile['bin_dtype']
        if args.prepared is None: args.prepared = ROOT.parent / 'data/prepared/ps/compass' / profile['prepared']
        if args.csv is None:
            args.csv = ROOT / 'results' / f'compass_full_c_{args.dataset}_{args.mode}.csv'
    return args

def vecs(path, integers=False):
    path = Path(path)
    size = path.stat().st_size
    if size < 4 or size % 4:
        raise ValueError(f'invalid vecs file length: {path}')
    raw = np.memmap(path, dtype='<i4', mode='r')
    dim = int(raw[0])
    if dim <= 0 or raw.size % (dim + 1):
        raise ValueError(f'invalid vecs dimensions: {path}')
    rows = raw.reshape(-1, dim + 1)
    for start in range(0, len(rows), 65536):
        if np.any(rows[start:start + 65536, 0] != dim):
            raise ValueError(f'inconsistent vecs dimensions: {path}')
    return rows[:, 1:] if integers else rows[:, 1:].view('<f4')

def headered_bin(path, integers=False, vector_dtype='uint8'):
    """An 8-byte u32 shape header, then uint8/float32 vectors or uint32 GT IDs."""
    if vector_dtype not in ('uint8', 'float32'):
        raise ValueError(f'unsupported bin vector dtype: {vector_dtype}')
    path = Path(path)
    with path.open('rb') as source:
        header = source.read(8)
    if len(header) != 8:
        raise ValueError(f'truncated bin header: {path}')
    count, width = struct.unpack('<II', header)
    dtype = np.dtype('<u4' if integers else ('u1' if vector_dtype == 'uint8' else '<f4'))
    # Python integers avoid uint32 multiplication overflow on large datasets.
    expected = 8 + int(count) * int(width) * dtype.itemsize
    if not count or not width or path.stat().st_size != expected:
        raise ValueError(f'bin dimensions/length mismatch: {path}; expected {expected} bytes')
    return np.memmap(path, dtype=dtype, mode='r', offset=8, shape=(count, width))

def read_vectors(path, file_format='auto', vector_dtype='uint8'):
    if file_format == 'auto':
        file_format = {'.bin': 'bin', '.fvecs': 'fvecs'}.get(Path(path).suffix.lower())
    if file_format == 'bin':
        return headered_bin(path, vector_dtype=vector_dtype)
    if file_format == 'fvecs':
        return vecs(path)
    raise ValueError(f'unknown vector format: {path}; use --base-format/--query-format')

def read_groundtruth(path, file_format='auto'):
    if file_format == 'auto':
        file_format = {'.bin': 'bin', '.ivecs': 'ivecs'}.get(Path(path).suffix.lower())
    if file_format == 'bin':
        return headered_bin(path, integers=True)
    if file_format == 'ivecs':
        return vecs(path, integers=True)
    raise ValueError(f'unknown groundtruth format: {path}; use --gt-format')
