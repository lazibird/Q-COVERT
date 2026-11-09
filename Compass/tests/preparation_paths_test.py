"""Check the offline builders resolve the same raw files as runtime configuration."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile

scheme = Path(__file__).resolve().parents[1]
config = json.loads((scheme / 'tests/config/config_ring.json').read_text())
ps = json.loads((scheme.parent / 'Q-COVERT-PS/config/dataset_defaults.json').read_text())['datasets']
for filename in ['build_bigann_hnsw.py', 'build_bigann_pq.py']:
    spec = importlib.util.spec_from_file_location('builder', scheme / 'script' / filename)
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    with tempfile.TemporaryDirectory() as temporary:
        os.chdir(temporary)
        for name in ['sift', 'sift10M', 'sift100M', 'deep1M', 'msmarco1m']:
            selected = builder.load_dataset_config(scheme, name)
            base = builder.resolve_config_path(scheme, selected['base_path'])
            assert base == scheme.parent / ps[name.upper()]['files']['base']
            for field in ['index_path', 'pq_path']:
                path = builder.resolve_config_path(scheme, selected[field])
                assert path.is_relative_to(scheme.parent / 'data/prepared/compass/indexes')
print('Offline HNSW/PQ builder paths passed from an unrelated launch directory')
