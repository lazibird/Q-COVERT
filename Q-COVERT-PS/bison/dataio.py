"""Dataset defaults from the shared JSON and the existing mmap readers."""
import argparse
import importlib.util
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = ROOT / 'config/dataset_defaults.json'
_spec = importlib.util.spec_from_file_location('_ringdoram_vector_readers', ROOT / 'compass/dataio.py')
_readers = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_readers)
read_vectors = _readers.read_vectors
read_groundtruth = _readers.read_groundtruth
headered_bin = _readers.headered_bin

def add_dataset_arguments(parser):
    parser.add_argument('--dataset', type=str.upper, choices=['SIFT', 'SIFT10M', 'SIFT100M', 'DEEP1M', 'MSMARCO1M'], default='SIFT')
    parser.add_argument('--config', type=Path, default=DEFAULTS)
    parser.add_argument('--data-root', type=Path, default=ROOT.parent / 'data', help='replace the data/ root in profile paths')

def resolve_paths(args, preparation=False):
    profile = json.loads(args.config.read_text())['datasets'][args.dataset]
    def path(name):
        value = Path(profile['files'][name])
        if not value.is_absolute():
            if args.data_root is not None and value.parts[0] == 'data':
                return args.data_root.joinpath(*value.parts[1:])
            return ROOT.parent / value
        return value
    for name in ('base', 'query', 'groundtruth'):
        if hasattr(args, name) and getattr(args, name) is None:
            setattr(args, name, path(name))
    # User-confirmed BiSON types: SIFT and DEEP1M are float32.
    # Shared Q-COVERT defaults still compress SIFT to uint8; do not change them.
    scalar = 'float32' if args.dataset == 'SIFT' else profile['data']['storage_scalar']
    if preparation:
        if args.scalar is None: args.scalar = scalar
        if args.base_dtype is None: args.base_dtype = scalar
        if args.output is None: args.output = ROOT.parent / 'data/prepared/ps/bison' / (args.dataset + '.bin')
        if args.index_prefix is None: args.index_prefix = ROOT.parent / 'data/prepared/ps/bison' / args.dataset / 'diskann'
    else:
        if args.query_dtype is None: args.query_dtype = scalar
        if args.prepared is None: args.prepared = ROOT.parent / 'data/prepared/ps/bison' / (args.dataset + '.bin')
        if args.csv is None: args.csv = ROOT / 'results' / ('bison_appendix_f_' + args.dataset.lower() + '_' + args.mode + '.csv')
        for name, key in [('queries', 'queries'), ('warmup', 'warmup'), ('repeats', 'repeats')]:
            if getattr(args, name) is None: setattr(args, name, profile['benchmark'][key])
        if args.threads is None: args.threads = profile['execution']['access_threads']
        if args.k is None: args.k = profile['query']['k']
        for name, key in [('z', 'Z'), ('s', 'S'), ('a', 'A'), ('stash', 'stash_blocks')]:
            if getattr(args, name) is None: setattr(args, name, profile['oram'][key])
        if args.evict_batch is None: args.evict_batch = profile['oram']['fu']['E']
    return profile
