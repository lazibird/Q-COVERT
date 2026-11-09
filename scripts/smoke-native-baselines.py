#!/usr/bin/env python3
"""Run small, real client/server checks without touching shared raw datasets."""
import argparse
import copy
import json
import os
from pathlib import Path
import random
import socket
import subprocess
import tempfile
import time
import numpy as np

ROOT = Path(__file__).resolve().parents[1]


def vecs(path, values, dtype):
    words = np.empty((len(values), values.shape[1] + 1), dtype='<u4')
    words[:, 0] = values.shape[1]
    words[:, 1:] = np.asarray(values, dtype=dtype).view('<u4')
    path.write_bytes(words.tobytes())


def ports():
    for _ in range(100):
        port = random.randrange(20000, 50000)
        opened = []
        try:
            for offset in range(4):
                sock = socket.socket()
                opened.append(sock)
                sock.bind(('127.0.0.1', port + offset))
            return port
        except PermissionError as error:
            raise RuntimeError("Local socket creation is blocked by the execution sandbox") from error
        except OSError:
            pass
        finally:
            for sock in opened:
                sock.close()
    raise RuntimeError('No four consecutive local ports available')


def run(command, label, work, env):
    log = work / (label + '.log')
    with log.open('w') as output:
        result = subprocess.run(list(map(str, command)), cwd=work, env=env,
                                stdout=output, stderr=subprocess.STDOUT, timeout=180)
    if result.returncode:
        raise RuntimeError(f'{label} failed ({result.returncode}); see {log}\n' + log.read_text()[-2500:])
    return log.read_text()


def pair(servers, client, label, work, env):
    jobs, handles = [], []
    try:
        for i, command in enumerate(servers):
            log = (work / f'{label}-server-{i}.log').open('w')
            handles.append(log)
            jobs.append(subprocess.Popen(list(map(str, command)), cwd=work, env=env,
                                         stdout=log, stderr=subprocess.STDOUT))
        time.sleep(.3)
        text = run(client, label + '-client', work, env)
        if 'Recall@' not in text and 'R@' not in text and 'recall_at_k_pct' not in text:
            raise RuntimeError(f'{label}: client completed without reporting recall')
        print(label + ': passed', flush=True)
    finally:
        for job in jobs:
            if job.poll() is None:
                job.terminate()
            try:
                job.wait(timeout=5)
            except subprocess.TimeoutExpired:
                job.kill()
                job.wait()
        for handle in handles:
            handle.close()


def compass(binary_dir, work, base, query, truth, env):
    import faiss
    faiss.omp_set_num_threads(1)
    index = faiss.IndexHNSWFlat(128, 64, faiss.METRIC_L2)
    index.hnsw.efConstruction = 80
    index.add(base)
    faiss.write_index(index, str(work / 'hnsw.index'))
    pq = faiss.IndexPQ(128, 8, 8, faiss.METRIC_L2)
    pq.pq.cp.niter = 1
    pq.pq.cp.min_points_per_centroid = 1
    pq.train(base)
    pq.add(base)
    faiss.write_index(pq, str(work / 'pq.index'))
    cfg = copy.deepcopy(json.loads((ROOT / 'Compass/tests/config/config_ring.json').read_text())['sift'])
    for key in cfg:
        if key.endswith('_path'):
            cfg[key] = str(work / 'compass-state' / (key + '.bin'))
    cfg.update(base_size=len(base), num_levels=8, oram_cached_levels=0,
               index_path=str(work / 'hnsw.index'), pq_path=str(work / 'pq.index'),
               base_path=str(work / 'sift_base.fvecs'), query_path=str(work / 'sift_query.fvecs'),
               gt_path=str(work / 'sift_groundtruth.ivecs'), ef_search=20, ef_lowest_cached_layer=1)
    (work / 'compass-state').mkdir()
    config = work / 'compass.json'
    config.write_text(json.dumps({'sift': cfg}))
    run([binary_dir / 'compass_init', 'd=sift', f'c={config}'], 'compass-init', work, env)
    for label, program, flags in [
        ('compass-access-leaking', 'test_compass_accuracy', []),
        ('compass-original', 'test_compass_ring', ['pq_oram=0', 'pm_oram=0', 'server_metadata=0']),
        ('compass-low', 'test_compass_ring', ['pq_oram=1', 'pm_oram=1', 'server_metadata=1'])]:
        port = ports()
        common = [binary_dir / program, 'd=sift', f'c={config}', f'p={port}', *flags]
        pair([[*common, 'r=1']], [*common, 'r=2', 'n=2', 'topk=10'], label, work, env)


def bison(binary_dir, work, base, query, truth, env):
    driver = ROOT / 'bison/scripts/benchmark.py'
    config = work / 'bison.json'
    artifacts = work / 'bison-data'
    common = [ROOT / '.venv/bin/python', driver, 'sift']
    options = ['--config', config, '--data-root', artifacts, '--build-dir', binary_dir]
    run([*common, 'prepare', *options, '--source', work, '--base-count', str(len(base))],
        'bison-prepare', work, env)
    run([*common, 'build', *options, '--ram-gb', '1', '--build-l', '32'], 'bison-index', work, env)
    run([*common, 'plain', *options, '--t', '5', '--w', '4', '--l', '32', '--threads', '1', '--queries', '2'],
        'bison-plain', work, env)
    print('bison-plain: passed', flush=True)
    cfg = json.loads(config.read_text())['sift']
    cfg.update(num_levels=8, oram_cached_levels=0)
    config.write_text(json.dumps({'sift': cfg}))
    run([*common, 'oram-init', *options], 'bison-init', work, env)
    port = ports()
    pair([[*common, 'server', *options, '--port', port, '--memory']],
         [*common, 'client', *options, '--port', port, '--t', '5', '--w', '4', '--l', '32',
          '--threads', '1', '--queries', '2', '--cached-levels', '0', '--cache-nodes', '0'],
         'bison-original', work, env)
    low = json.loads((ROOT / 'bison/config/bench_low_sift.json').read_text())['sift']
    cfg.update({key: value for key, value in low.items() if key.startswith(('pq_', 'posmap_'))})
    cfg.update(low_client_storage=True, recursive_posmap=True, oram_cached_levels=0,
               pq_oram_num_blocks=0, pq_oram_num_levels=0,
               posmap_oram_num_blocks=0, posmap_oram_num_levels=0)
    for key in list(cfg):
        if key.endswith('_path') and key.startswith(('pq_', 'posmap_')):
            cfg[key] = str(work / 'bison-low' / Path(cfg[key]).name)
    (work / 'bison-low').mkdir()
    config.write_text(json.dumps({'sift': cfg}))
    run([binary_dir / 'oram_initializer_low', 'd=sift', f'c={config}'], 'bison-low-init', work, env)
    port = ports()
    servers = [[*common, 'server', *options, '--role', role, '--port', port + offset, '--memory']
               for role, offset in [('graph', 0), ('pq', 2), ('posmap', 3)]]
    pair(servers, [*common, 'client', *options, '--port', port, '--t', '5', '--w', '4', '--l', '32',
          '--threads', '1', '--queries', '2'], 'bison-low', work, env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', choices=['compass', 'bison', 'all'], default='all')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    args = parser.parse_args()
    args.build_dir = args.build_dir.resolve()
    output = args.build_dir / 'smoke-logs'
    output.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='baselines-', dir=output))
    print('Smoke-test artifacts: ' + str(work), flush=True)
    rng = np.random.default_rng(73)
    base = rng.normal(size=(512, 128)).astype('<f4')
    query = base[:2].copy()
    truth = np.argsort(((query[:, None] - base[None]) ** 2).sum(axis=2), axis=1)[:, :10]
    vecs(work / 'sift_base.fvecs', base, '<f4')
    vecs(work / 'sift_query.fvecs', query, '<f4')
    vecs(work / 'sift_groundtruth.ivecs', truth, '<i4')
    env = {**os.environ, 'OMP_NUM_THREADS': '1', 'OPENBLAS_NUM_THREADS': '1', 'MKL_NUM_THREADS': '1'}
    if args.only in ('compass', 'all'):
        compass(args.build_dir / 'Compass', work, base, query, truth, env)
    if args.only in ('bison', 'all'):
        bison(args.build_dir / 'bison/bin', work, base, query, truth, env)


if __name__ == '__main__':
    main()
