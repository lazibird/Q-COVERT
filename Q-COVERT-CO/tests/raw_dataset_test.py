"""Exercise PS-compatible raw inputs through CO preprocessing, build and search."""
import copy
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

import numpy as np

SCHEME = Path(__file__).resolve().parents[1]
REPO = SCHEME.parent
BUILD = Path(sys.argv[1]).resolve()
CO = json.loads((SCHEME / 'config.json').read_text())
PS = json.loads((REPO / 'Q-COVERT-PS/config/dataset_defaults.json').read_text())


def vecs(path, values, dtype):
    words = np.empty((len(values), values.shape[1] + 1), dtype='<u4')
    words[:, 0] = values.shape[1]
    words[:, 1:] = np.asarray(values, dtype=dtype).view('<u4')
    path.write_bytes(words.tobytes())


def write(path, values, dtype):
    if path.suffix == '.bin':
        path.write_bytes(struct.pack('<II', *values.shape) + np.asarray(values, dtype=dtype).tobytes())
    else:
        vecs(path, values, dtype)


def run(command, cwd):
    result = subprocess.run([str(x) for x in command], cwd=cwd, text=True, capture_output=True,
                            env={**os.environ, 'OMP_NUM_THREADS': '1', 'OPENBLAS_NUM_THREADS': '1'})
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    assert not re.search(r'[\u4e00-\u9fff]', result.stdout + result.stderr), command
    return result


with tempfile.TemporaryDirectory(prefix='co-raw-input-test-') as temporary:
    work = Path(temporary)
    for name, profile in PS['datasets'].items():
        dataset = next(key for key in CO['datasets'] if key.lower() == name.lower())
        configured = CO['datasets'][dataset]
        expected = {role: Path(path).name for role, path in profile['files'].items()}
        assert configured['files'] == expected, (dataset, configured['files'], expected)
        assert (SCHEME / configured['source']).resolve() == (REPO / profile['files']['base']).parent
        source = work / dataset
        source.mkdir()
        dim = 96 if name == 'DEEP1M' else 768 if name == 'MSMARCO1M' else 128
        dtype = '<f4' if profile['data']['storage_scalar'] == 'float32' else 'u1'
        rng = np.random.default_rng(42)
        base = (rng.random((64, dim)) if dtype == '<f4' else rng.integers(0, 256, (64, dim))).astype(dtype)
        query = base[:2].astype('<f4')
        truth = np.argsort(((query[:, None] - base.astype('<f4')[None]) ** 2).sum(axis=2), axis=1)[:, :10]
        write(source / expected['base'], base, dtype if expected['base'].endswith('.bin') else '<f4')
        write(source / expected['query'], query, '<f4')
        write(source / expected['groundtruth'], truth, '<u4' if expected['groundtruth'].endswith('.bin') else '<i4')
        # Stale legacy copies must not change file selection.
        if dataset + '_base.fvecs' != expected['base']:
            (source / (dataset + '_base.fvecs')).write_bytes(b'stale')
        if dataset + '_query.fvecs' != expected['query']:
            (source / (dataset + '_query.fvecs')).write_bytes(b'stale')
        if expected['query'] != 'query.bin':
            (source / 'query.bin').write_bytes(b'stale')
        config = copy.deepcopy(CO)
        config['default_dataset'] = dataset
        selected = config['datasets'][dataset]
        selected['source'] = str(source)
        selected.pop('train', None)
        selected['clusters'] = 2
        selected['query']['path_batch_clusters'] = 2
        selected['preprocess'].update(kmeans_niter=1, kmeans_nredo=1, size_max=64)
        config_path = work / (dataset + '.json')
        config_path.write_text(json.dumps(config))
        run([sys.executable, SCHEME / 'ivf-rabitq/preprocess.py', 'all', '--config', config_path], work)
        suffix = '_d96' if dim == 96 else '_d768' if dim == 768 else ''
        run([BUILD / ('build_ivf_rabitq_index' + suffix), '--config', config_path], REPO)
        for program in ['search_local' + suffix, 'test_ivf_rabitq_double_oram' + suffix,
                        'test_ivf_rabitq_encrypted_baseline']:
            # The encrypted baseline reads vector dimensions at runtime.
            result = run([BUILD / program, '--config', config_path, '--nprobes', '2', '--k2', '10',
                          '--nqueries', '2'], work)
            if program.startswith('test_ivf_rabitq_double_oram'):
                tag = 'rawu8' if dtype == 'u1' else 'rawf32'
                size = dim * (1 if dtype == 'u1' else 4) + 4
                assert f'FuORAM exact payload format: {tag}, bytes: {size}' in result.stdout
        if name == 'SIFT':
            command = [BUILD / 'test_ivf_rabitq_double_oram', '--config', config_path,
                       '--nprobes', '2', '--k2', '10', '--nqueries', '2']
            float_run = run(command + ['--exact-payload', 'raw-float32'], work)
            assert 'FuORAM exact payload format: rawf32, bytes: 516' in float_run.stdout
            assert 'cache hit; loading FuORAM server state' not in float_run.stdout
            byte_run = run(command, work)
            assert 'cache hit; loading FuORAM server state' in byte_run.stdout
            recall = lambda text: re.search(r'recall@10 ([0-9.]+)', text).group(1)
            assert recall(float_run.stdout) == recall(byte_run.stdout)
        run([BUILD / ('search_local' + suffix), '--no-config', '--dataset', dataset,
             '--source', source, '--clusters', '2', '--nprobes', '2', '--k2', '10',
             '--nqueries', '2'], work)
        print(dataset + ': preprocess, index, local search, CO and encrypted baseline passed', flush=True)
    for csv_path in work.rglob('*.csv'):
        assert not re.search(r'[\u4e00-\u9fff]', csv_path.read_text()), csv_path
    # The subset generator must produce actual PS vecs encodings, not renamed bin files.
    source = work / 'subsets/sift1B'
    source.mkdir(parents=True)
    base = np.arange(32, dtype='u1').reshape(8, 4)
    queries = base[:2]
    write(source / 'base.bin', base, 'u1')
    write(source / 'query.bin', queries, 'u1')
    run([sys.executable, REPO / 'data/prepare_datasets.py', 'sift10m',
         '--data-root', work / 'subsets', '--sift10m-count', '8', '--k', '2', '--faiss-threads', '1'], work)
    target = work / 'subsets/sift10M'
    raw = np.fromfile(target / 'sift10m_query.fvecs', dtype='<u4').reshape(2, 5)
    assert np.all(raw[:, 0] == 4)
    np.testing.assert_array_equal(raw[:, 1:].view('<f4'), queries)
    gt = np.fromfile(target / 'sift10m_groundtruth.ivecs', dtype='<i4').reshape(2, 3)
    assert np.all(gt[:, 0] == 2)
    np.testing.assert_array_equal(gt[:, 1], [0, 1])
    print('Subset generation: PS fvecs/ivecs encodings passed')
