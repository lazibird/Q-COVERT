#!/usr/bin/env python3
"""End-to-end preparation, full-C oracle, two-tree snapshots and ef sweeps."""
import csv
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import stat
import sys
import tempfile
import numpy as np
from prepare import ROOT, read_hnsw, read_pq, INVALID
from dataio_test import write_bin
from dataio import DATASETS
from prepared import prepared_full, node_dtype

def write_vecs(path, values, integer=False):
    with open(path, 'wb') as out:
        for row in values:
            out.write(struct.pack('<i', len(row)))
            out.write(np.asarray(row, dtype='<i4' if integer else '<f4').tobytes())

def test(build, mode='native'):
    runner = build / 'compass' / ('compass_benchmark' if mode == 'native' else 'compass_sgx_host')
    layout = json.loads(subprocess.check_output([str(runner), '--describe-layout'], text=True))
    assert (layout['prepared_protocol'], layout['search_protocol'], layout['snapshot_protocol']) == (3, 5, 4)
    dim, degree, p, centers, scalar = [layout[x] for x in ('dimension', 'degree', 'pq_subspaces', 'pq_centroids', 'scalar')]
    with tempfile.TemporaryDirectory(prefix='compass-pipeline-') as directory:
        work = Path(directory)
        rng = np.random.default_rng(42)
        base = rng.integers(0, 32, (512, dim)).astype('f4')
        if scalar == 'float32':
            base = (base - 16) / 8 + rng.uniform(-0.05, 0.05, base.shape).astype('f4')
        queries = base[[1, 17, 400]]
        truth = np.argsort(np.sum((base[None].astype('f8') - queries[:, None].astype('f8'))**2, axis=2), axis=1)[:, :10]
        write_vecs(work / 'base.fvecs', base); write_vecs(work / 'query.fvecs', queries); write_vecs(work / 'gt.ivecs', truth, True)
        def run(script, *args, good=True):
            result = subprocess.run([sys.executable, str(ROOT / 'compass' / script), *map(str, args)],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if (result.returncode == 0) != good: raise AssertionError(result.stdout + result.stderr)
            return result
        trained = work / 'prepared.bin'
        run('prepare.py', '--base', work / 'base.fvecs', '--output', trained,
            '--scalar', scalar, '--pq-iterations', 2, '--threads', 1,
            '--hnsw-m', degree // 2, '--pq-subspaces', p, '--pq-bits', centers.bit_length() - 1)
        graph_path, pq_path = work / 'prepared.hnsw.index', work / 'prepared.pq.index'
        graph, pq = read_hnsw(graph_path), read_pq(pq_path)
        assert graph[:3] == (dim, 512, 1) and graph[4].shape == (512, degree)
        assert pq[:5] == (dim, 512, 1, p, centers.bit_length() - 1)
        run('prepare.py', '--base', work / 'base.fvecs', '--output', work / 'imported.bin',
            '--scalar', scalar, '--hnsw', graph_path, '--pq', pq_path)
        assert trained.read_bytes() == (work / 'imported.bin').read_bytes()
        raw, header, book, nodes, upper, scan, level = prepared_full(trained)
        np.testing.assert_array_equal(nodes['embedding'], base)
        assert upper.dtype['neighbors'].shape == (degree // 2,)
        common = ['--prepared', trained, '--query', work / 'query.fvecs', '--groundtruth', work / 'gt.ivecs',
                  '--build-dir', build, '--mode', mode, '--queries', 3, '--warmup', 1,
                  '--ef', 17, '--ef-spec', 4, '--ef-n', 4, '--k', 10, '--threads', 2,
                  '--z', 8, '--s', 16, '--a', 4, '--evict-batch', 1, '--reference', '--csv', work / 'result.csv']
        missing = run('benchmark.py', *common, '--build-dir', work / 'unbuilt', good=False)
        assert 'Compass runner not found' in missing.stderr and 'Traceback' not in missing.stderr
        first = run('benchmark.py', *common)
        assert 'oram_snapshot_status=built' in first.stdout
        image = work / f'prepared.initial_oram.{mode}.state'
        assert image.is_file() and stat.S_IMODE(image.stat().st_mode) == 0o600
        digest = hashlib.sha256(image.read_bytes()).digest()
        second = run('benchmark.py', *common)
        assert 'oram_snapshot_status=loaded' in second.stdout and 'compass/bulk_setup' not in second.stderr
        assert hashlib.sha256(image.read_bytes()).digest() == digest
        rows = list(csv.DictReader((work / 'result.csv').open()))
        assert len(rows) == 2 and all(r['reference_result_match'] == 'True' for r in rows)
        upper_reads = 4 if len(upper) else 0
        assert all(int(r['logical_reads']) == 81 + upper_reads and int(r['rounds']) == 5 for r in rows)
        assert all(r['search_variant'] == 'full-c-exact' and int(r['candidate_capacity']) == 81 for r in rows)
        assert all(int(r['position_map_bytes']) == (512 + len(upper)) * 8 for r in rows)
        assert all(int(r['upper_node_block_bytes']) < int(r['node_block_bytes']) for r in rows)
        batch_csv = work / 'batch.csv'
        batch = run('benchmark.py', *common, '--ef', 17, 10, 29, 17, '--csv', batch_csv)
        assert batch.stdout.count('oram_snapshot_status=loaded') == 1 and 'compass/bulk_setup' not in batch.stderr
        batch_rows = list(csv.DictReader(batch_csv.open()))
        assert [int(r['logical_reads']) for r in batch_rows] == [81 + upper_reads, 49 + upper_reads, 129 + upper_reads, 81 + upper_reads]
        assert [int(r['ef']) for r in batch_rows] == [17, 10, 29, 17]
        assert len({r['setup_ms'] for r in batch_rows}) == 1 and all(r['reference_result_match'] == 'True' for r in batch_rows)
        assert hashlib.sha256(image.read_bytes()).digest() == digest
        # A pristine image is independent of public query budgets and threads.
        changed = run('benchmark.py', *common, '--ef', 29, '--ef-spec', 3, '--ef-n', 6,
                      '--threads', 1, '--queries', 2, '--warmup', 0, '--csv', batch_csv)
        assert 'oram_snapshot_status=loaded' in changed.stdout
        assert list(csv.DictReader(batch_csv.open()))[-1]['reference_result_match'] == 'True'
        disabled = run('benchmark.py', *common, '--no-oram-snapshot', '--csv', batch_csv)
        assert 'oram_snapshot_status=none' in disabled.stdout
        forced = run('benchmark.py', *common, '--rebuild-oram', '--csv', batch_csv)
        assert 'oram_snapshot_status=built' in forced.stdout and hashlib.sha256(image.read_bytes()).digest() != digest
        image.write_bytes(image.read_bytes()[:-11])
        recovered = run('benchmark.py', *common, '--csv', batch_csv)
        assert 'oram_snapshot_status=built' in recovered.stdout and 'compass/snapshot_invalid' in recovered.stderr
        modified = work / 'modified.bin'; modified_bytes = bytearray(trained.read_bytes())
        struct.pack_into('<f', modified_bytes, 96, struct.unpack_from('<f', modified_bytes, 96)[0] + .125); modified.write_bytes(modified_bytes)
        mismatch = run('benchmark.py', *common, '--prepared', modified, '--oram-snapshot', image, '--csv', batch_csv)
        assert 'oram_snapshot_status=built' in mismatch.stdout and 'compass/snapshot_invalid' in mismatch.stderr
        previous_csv = (work / 'result.csv').read_bytes()
        for invalid in [('--ef', 0), ('--ef', 1 << 64), ('--ef-spec', 0), ('--upper-steps', 2), ('--k', 0), ('--no-oram-snapshot', '--rebuild-oram')]:
            run('benchmark.py', *common, *invalid, good=False)
        assert (work / 'result.csv').read_bytes() == previous_csv
        # Convert the old full-M upper/scan wire blocks, without retraining.
        legacy = work / 'legacy.bin'
        with legacy.open('wb') as out:
            out.write(struct.pack('<12Q', int.from_bytes(b'CMPDO002', 'little'), *header, len(upper), len(scan), level))
            out.write(book.tobytes()); out.write(nodes.tobytes())
            for group in (upper, scan):
                old = np.zeros(len(group), dtype=nodes.dtype); old['id'] = group['id']; old['embedding'] = group['embedding']
                old['neighbors']['id'] = INVALID; old['neighbors'][:, :degree // 2] = group['neighbors']
                out.write(old.tobytes())
        rejected = run('benchmark.py', *common, '--prepared', legacy, good=False)
        assert '--convert-prepared' in rejected.stderr
        run('prepare.py', '--convert-prepared', legacy, '--scalar', scalar, '--output', work / 'converted.bin')
        assert trained.read_bytes() == (work / 'converted.bin').read_bytes()
        if scalar == 'float32':
            # A legacy uint8 payload has lost negative/fractional values.
            # Conversion must reload the original float32 base, not cast it.
            compressed = work / 'compressed-legacy.bin'
            old_header = list(header); old_header[4] = 2
            with compressed.open('wb') as out:
                out.write(struct.pack('<12Q', int.from_bytes(b'CMPDO002', 'little'), *old_header, len(upper), len(scan), level))
                out.write(book.tobytes())
                for group in (nodes, upper, scan):
                    old = np.zeros(len(group), dtype=node_dtype(dim, degree, p, 2))
                    old['id'] = group['id']; old['embedding'] = np.clip(group['embedding'], 0, 255).astype('u1')
                    old['neighbors']['id'] = INVALID
                    old['neighbors'][:, :group.dtype['neighbors'].shape[0]] = group['neighbors']
                    out.write(old.tobytes())
            run('prepare.py', '--convert-prepared', compressed, '--scalar', 'float32',
                '--base', work / 'base.fvecs', '--output', work / 'rehydrated.bin')
            assert trained.read_bytes() == (work / 'rehydrated.bin').read_bytes()
        # Explicit nonempty L2 and no-L1 fixtures exercise both hierarchy paths.
        assert len(upper) >= 2
        no_upper_scan = np.zeros(1, dtype=upper.dtype); no_upper_scan['id'] = header[-1]
        no_upper_scan['embedding'] = nodes[header[-1]]['embedding']; no_upper_scan['neighbors']['id'] = INVALID
        for label, routing, pool, scan_level in [('layer2', upper, upper[[0, -1]], 2), ('no-layer1', upper[:0], no_upper_scan, 0)]:
            path = work / f'{label}.bin'
            with path.open('wb') as out:
                out.write(struct.pack('<12Q', int.from_bytes(b'CMPDO003', 'little'), *header, len(routing), len(pool), scan_level))
                out.write(book.tobytes()); out.write(nodes.tobytes()); out.write(routing.tobytes()); out.write(pool.tobytes())
            run('benchmark.py', *common, '--prepared', path, '--csv', work / f'{label}.csv')
            row = list(csv.DictReader((work / f'{label}.csv').open()))[0]
            assert row['reference_result_match'] == 'True' and int(row['logical_reads']) == 81 + (4 if len(routing) else 0)
        # Dataset defaults and headered binaries, matching this compiled scalar.
        names = ['sift10m', 'sift100m'] if scalar == 'uint8' else (['sift'] if dim == 128 else ['deep1m', 'msmarco1m'])
        for name in names:
            profile = DATASETS[name]; folder = work / 'data' / profile['directory']; folder.mkdir(parents=True)
            for key, values in [('base', base), ('query', queries), ('groundtruth', truth)]:
                path = folder / profile[key]
                if path.suffix == '.bin': write_bin(path, values, key == 'groundtruth', profile['bin_dtype'])
                else: write_vecs(path, values, key == 'groundtruth')
            index_path = work / f'{name}.bin'
            run('prepare.py', '--dataset', name, '--data-root', work / 'data', '--output', index_path, '--hnsw', graph_path, '--pq', pq_path)
            assert index_path.read_bytes() == trained.read_bytes()
            run('benchmark.py', *common, '--dataset', name, '--data-root', work / 'data', '--prepared', index_path,
                '--query', folder / profile['query'], '--groundtruth', folder / profile['groundtruth'], '--csv', work / f'{name}.csv')
            assert list(csv.DictReader((work / f'{name}.csv').open()))[0]['reference_result_match'] == 'True'
        print(f'Compass {mode}: full-C oracle, two-tree snapshots, ef sweeps, migration and profiles {names} passed; recall@10={rows[0]["recall_at_k"]}')

if __name__ == '__main__':
    test(Path(sys.argv[1]).resolve(), sys.argv[2].lower() if len(sys.argv) > 2 else 'native')
