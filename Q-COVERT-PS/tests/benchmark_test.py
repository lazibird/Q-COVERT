"""Small end-to-end benchmark/CSV regression, with actual IVF/ORAM execution."""
import copy
import contextlib
import csv
import io
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import benchmark


def write_vecs(path, values, floats):
    dtype = '<f4' if floats else '<i4'
    with path.open('wb') as out:
        for row in values:
            out.write(struct.pack('<I', len(row)))
            out.write(np.asarray(row, dtype=dtype).tobytes())


def main():
    timing = {'recall_hits': 8, 'call_wall_ns': 20_000_000,
              'qu_read_ns': 2_000_000, 'fu_read_ns': 3_000_000,
              'centroid_distance_ns': 1_000_000, 'centroid_topk_ns': 2_000_000,
              'query_rotation_ns': 3_000_000, 'quantized_distance_ns': 4_000_000,
              'candidate_topk_ns': 5_000_000, 'exact_distance_ns': 6_000_000,
              'rerank_topk_ns': 7_000_000, 'qu_evict_ns': 8_000_000,
              'fu_evict_ns': 9_000_000, 'qu_stash_peak': 2, 'fu_stash_peak': 3}
    summary = benchmark.compact_summary({'query': {'nprobe': 4, 'k2': 32, 'k': 10}},
                                        [timing, {**timing, 'recall_hits': 10,
                                                  'call_wall_ns': 22_000_000,
                                                  'qu_stash_peak': 5, 'fu_stash_peak': 1}])
    assert list(summary) == benchmark.CSV_COLUMNS
    assert summary['recall_at_k'] == .9 and summary['total_ms'] == 21
    assert summary['online_ms'] == 4
    assert summary['candidate_clusters_ms'] == 3 and summary['quantized_distance_ms'] == 7
    assert summary['topk2_ms'] == 5 and summary['topk_ms'] == 13
    assert summary['qu_stash_peak'] == 5 and summary['fu_stash_peak'] == 3
    approximate_summary = benchmark.compact_summary({'query': {'nprobe': 4, 'k2': 32, 'k': 10}}, [timing], [16] * 4)
    assert list(approximate_summary) == benchmark.APPROX_CSV_COLUMNS
    assert approximate_summary['alpha'] == 2
    visual = io.StringIO()
    with contextlib.redirect_stderr(visual):
        indicator = benchmark.Progress()
        indicator.update('runner/queries', 0, 6)
        indicator.update('runner/queries', 6, 6)
        indicator.close()
    assert visual.getvalue().count('\n') == 1 and '\r' in visual.getvalue()
    build = Path(sys.argv[1]).resolve()
    sgx = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    with tempfile.TemporaryDirectory(prefix='qcovert-benchmark-test-') as temp:
        root = Path(temp)
        cache_root = root / 'parent-sgx-build'
        nested_build = cache_root / 'Q-COVERT-PS'
        image = nested_build / 'apps/sgx/ringdoram_enclave.signed.so'
        image.parent.mkdir(parents=True)
        image.write_bytes(b'path-selection-fixture')
        (cache_root / 'CMakeCache.txt').write_text(
            'RINGDORAM_SGX_MODE:STRING=SIM\nRINGDORAM_SGX_HEAP_SIZE:STRING=128M\n')
        selected = benchmark.select_sgx_enclave(nested_build, 'sgx-sim',
            {'execution': {'sgx_heap_bytes': 128 * 1024 * 1024}})
        assert selected == image
        rng = np.random.default_rng(91)
        vectors = rng.integers(0, 255, (32, 128), dtype=np.uint8)
        queries = vectors[:4]
        distances = ((queries[:, None, :].astype(np.int64) - vectors[None, :, :]) ** 2).sum(axis=2)
        truth = np.argsort(distances, axis=1, kind='stable')[:, :10]
        write_vecs(root / 'base.fvecs', vectors, True)
        write_vecs(root / 'query.fvecs', queries, True)
        write_vecs(root / 'gt.ivecs', truth, False)
        cfg = json.loads((ROOT / 'config/dataset_defaults.json').read_text())
        p = cfg['datasets']['SIFT']
        assert p['query']['candidate_selection']['mode'] in ('exact', 'approx')
        p['files'].update(base='base.fvecs', query='query.fvecs', groundtruth='gt.ivecs')
        p['clustering'].update(initial_clusters=4, iterations=2)
        p['query'].update(nprobe=4, k2=32)
        p['query']['candidate_selection']['mode'] = 'exact'
        p['oram'].update(Z=8, A=2, S=16)
        p['oram']['qu'] = {'R': 0, 'E': 2}
        p['oram']['fu'] = {'R': 0, 'E': 2}
        p['execution']['sgx_heap_bytes'] = 128 * 1024 * 1024
        config = root / 'config.json'
        config.write_text(json.dumps(cfg))
        requested_output = root / 'results.csv'
        output = benchmark.dataset_csv_path(requested_output, 'SIFT', 'native')
        sgx_output = benchmark.dataset_csv_path(requested_output, 'SIFT', 'sgx-sim')
        assert output.name == 'results_SIFT_native_exact.csv'
        assert sgx_output.name == 'results_SIFT_sgx-sim_exact.csv'
        assert benchmark.dataset_csv_path(output, 'SIFT', 'native') == output
        assert benchmark.dataset_csv_path(output, 'SIFT', 'sgx-hw').name == 'results_SIFT_sgx-hw_exact.csv'
        assert benchmark.dataset_csv_path(root / 'results_SIFT_native.csv', 'SIFT', 'native') == output
        approx_path = benchmark.dataset_csv_path(requested_output, 'SIFT', 'native', 'approx')
        assert approx_path.name == 'results_SIFT_native_approx.csv'
        assert benchmark.dataset_csv_path(approx_path, 'SIFT', 'native', 'approx') == approx_path
        assert benchmark.dataset_csv_path(root / 'results_SIFT_native_approx-0123456789.csv',
                                          'SIFT', 'native', 'approx') == approx_path
        assert benchmark.dataset_csv_path(approx_path, 'SIFT', 'native') == output
        command = [sys.executable, str(ROOT / 'tools/benchmark.py'), '--config', str(config),
                   '--data-root', str(root), '--build-dir', str(build), '--cache-dir', str(root / 'cache'),
                   '--csv', str(requested_output), '--warmup', '1', '--queries', '3', '--repeats', '2', '--threads', '2']
        env = {**os.environ, 'OMP_NUM_THREADS': '1', 'OPENBLAS_NUM_THREADS': '1'}
        first_run = subprocess.run(command, check=True, env=env, capture_output=True, text=True)
        assert not re.search(r'[\u4e00-\u9fff]', first_run.stdout + first_run.stderr)
        assert 'oram_snapshot_status=built' in first_run.stderr
        assert 'Offline quantization/Encode vectors: 100%' in first_run.stderr
        assert 'Offline preparation/Save cache: 100%' in first_run.stderr
        assert 'ORAM Setup/FuORAM BulkLoad: 0%' in first_run.stderr
        assert 'ORAM Setup/QuORAM BulkLoad: 100%' in first_run.stderr
        assert 'Query warmup: 100%' in first_run.stderr
        assert 'Measured queries: 100% (6/6)' in first_run.stderr
        first = output.read_bytes()
        assert not re.search(r'[\u4e00-\u9fff]', first.decode('utf-8'))
        image = root / 'cache/SIFT/initial_oram.native.state'
        assert image.is_file()
        assert image.stat().st_mode & 0o777 == 0o600
        first_image = image.read_bytes()
        cached_run = subprocess.run(command, check=True, env=env, capture_output=True, text=True)
        assert 'oram_snapshot_status=loaded' in cached_run.stderr
        assert 'Offline cache/Verify prepared data: 100%' in cached_run.stderr
        assert 'ORAM snapshot/Restore FuORAM: 100%' in cached_run.stderr
        assert 'ORAM Setup/FuORAM BulkLoad' not in cached_run.stderr
        assert image.read_bytes() == first_image
        assert output.read_bytes().startswith(first)
        calibrated = root / 'approx_config.json'
        calibrated.write_text(json.dumps(cfg))
        prepared = next((root / 'cache/SIFT').glob('*/prepared.bin'))
        calibration = subprocess.run([sys.executable, str(ROOT / 'tools/calibrate_topk.py'),
                                      '--config', str(calibrated), '--data-root', str(root),
                                      '--prepared', str(prepared), '--nprobe', '4', '--k2', '32',
                                      '--alpha', '2', '--queries', '2', '--write-config'],
                                     check=True, env=env, capture_output=True, text=True)
        fractions = json.loads(calibrated.read_text())['datasets']['SIFT']['query']['candidate_selection']['rank_fractions']
        assert len(fractions) == 4 and sum(benchmark.selection_quotas(
            json.loads(calibrated.read_text())['datasets']['SIFT'], 32)) == 64
        assert '[' in calibration.stdout
        approx_command = command + ['--config', str(calibrated)]
        approx_first = subprocess.run(approx_command, check=True, env=env, capture_output=True, text=True)
        approx_second = subprocess.run(approx_command, check=True, env=env, capture_output=True, text=True)
        approx_image = image
        approx_csv = approx_path
        assert approx_image.is_file() and approx_csv.is_file()
        assert 'oram_snapshot_status=loaded' in approx_first.stderr
        assert 'oram_snapshot_status=loaded' in approx_second.stderr
        approx_rows = list(csv.DictReader(approx_csv.open()))
        assert len(approx_rows) == 2
        assert list(approx_rows[0]) == benchmark.APPROX_CSV_COLUMNS
        assert all(float(row['alpha']) == 2 for row in approx_rows)
        legacy_approx = root / 'legacy_approx.csv'
        legacy_approx.write_text(','.join(benchmark.CSV_COLUMNS) + '\n')
        try:
            benchmark.check_csv_schema(legacy_approx, 'approx')
        except ValueError:
            pass
        else:
            raise AssertionError('old approximate CSV schema accepted')
        missing_newline = root / 'missing_newline.csv'
        benchmark.append_csv(missing_newline, [approximate_summary], 'approx')
        missing_newline.write_bytes(missing_newline.read_bytes().rstrip(b'\r\n'))
        benchmark.append_csv(missing_newline, [approximate_summary], 'approx')
        assert len(list(csv.DictReader(missing_newline.open()))) == 2
        assert len(list(csv.DictReader(output.open()))) == 2
        exact_override = subprocess.run(approx_command + ['--candidate-selection', 'exact',
                                                          '--csv', str(root / 'override.csv')],
                                        check=True, env=env, capture_output=True, text=True)
        assert 'oram_snapshot_status=loaded' in exact_override.stderr
        assert (root / 'override_SIFT_native_exact.csv').is_file()
        full_profile = copy.deepcopy(cfg)
        full_profile['datasets']['SIFT']['query']['candidate_selection'] = {
            'mode': 'exact', 'rank_fractions': [1, 1, 1, 1]}
        full_config = root / 'full_partition_config.json'
        full_config.write_text(json.dumps(full_profile))
        full_run = subprocess.run(command + ['--config', str(full_config), '--candidate-selection', 'approx', '--alpha', '4'], check=True, env=env,
                                  capture_output=True, text=True)
        assert 'oram_snapshot_status=loaded' in full_run.stderr
        full_rows = list(csv.DictReader(approx_csv.open()))
        assert len(full_rows) == 3
        full_row = full_rows[-1]
        assert float(full_row['recall_at_k']) == 1 and float(full_row['alpha']) == 4
        rows = list(csv.DictReader(output.open()))
        assert len(rows) == 2
        assert list(rows[0]) == benchmark.CSV_COLUMNS
        assert not requested_output.exists()
        for row in rows:
            assert float(row['recall_at_k']) == 1
            assert int(row['n_probe']) == 4 and int(row['k2']) == 32
            assert float(row['total_ms']) > 0
            for name in benchmark.CSV_COLUMNS[4:12]:
                assert float(row[name]) >= 0
            assert int(row['qu_stash_peak']) >= 0 and int(row['fu_stash_peak']) >= 0
        rebuilt = subprocess.run(command + ['--rebuild-oram'], check=True, env=env, capture_output=True, text=True)
        assert 'oram_snapshot_status=built' in rebuilt.stderr
        assert image.read_bytes() != first_image
        image.write_bytes(b'corrupt')
        recovered = subprocess.run(command, check=True, env=env, capture_output=True, text=True)
        assert 'ORAM snapshot/Invalid, rebuilding' in recovered.stderr
        assert 'oram_snapshot_status=built' in recovered.stderr
        assert len(list(csv.DictReader(output.open()))) == 4
        # An existing schema is rejected before opening the prepared data.
        bad = benchmark.dataset_csv_path(root / 'bad.csv', 'SIFT', 'native')
        bad.write_text('wrong,header\n')
        try:
            benchmark.append_csv(bad, [{'n_probe': 4}])
        except ValueError:
            pass
        else:
            raise AssertionError('schema mismatch not rejected')
        assert bad.read_text() == 'wrong,header\n'
        preflight = subprocess.run(command + ['--csv', str(root / 'bad.csv')], env=env,
                                   capture_output=True, text=True)
        assert preflight.returncode != 0 and 'CSV schema mismatch' in preflight.stderr
        # A failed runner invocation leaves the compact results CSV unchanged.
        fake = root / 'fake-build'
        fake.mkdir()
        (fake / 'qcovert_prepare').symlink_to(build / 'qcovert_prepare')
        runner = fake / 'qcovert_benchmark'
        runner.write_text('#!/bin/sh\necho "intentional failure" >&2\nexit 9\n')
        runner.chmod(0o755)
        rows_before_failure = output.read_bytes()
        failed = subprocess.run(command + ['--build-dir', str(fake)], env=env, capture_output=True, text=True)
        assert failed.returncode != 0
        assert 'intentional failure' in failed.stderr and output.read_bytes() == rows_before_failure
        if sgx:
            sgx_command = command + ['--mode', 'sgx-sim', '--sgx-build-dir', str(sgx)]
            sgx_first = subprocess.run(sgx_command, check=True, env=env, capture_output=True, text=True)
            sgx_second = subprocess.run(sgx_command, check=True, env=env, capture_output=True, text=True)
            sgx_approx = sgx_command + ['--config', str(full_config), '--candidate-selection', 'approx', '--alpha', '4']
            sgx_approx_first = subprocess.run(sgx_approx, check=True, env=env, capture_output=True, text=True)
            sgx_approx_second = subprocess.run(sgx_approx, check=True, env=env, capture_output=True, text=True)
            assert 'oram_snapshot_status=loaded' in sgx_approx_first.stderr
            assert 'oram_snapshot_status=loaded' in sgx_approx_second.stderr
            assert (root / 'cache/SIFT/initial_oram.sgx-sim.state').is_file()
            assert not (root / 'cache/SIFT/initial_oram.sgx-sim.approx.state').exists()
            sgx_approx_csv = benchmark.dataset_csv_path(requested_output, 'SIFT', 'sgx-sim', 'approx')
            assert sgx_approx_csv.is_file()
            assert all(float(row['recall_at_k']) == 1 and float(row['alpha']) == 4
                       for row in csv.DictReader(sgx_approx_csv.open()))
            sgx_rows = list(csv.DictReader(sgx_output.open()))
            sgx_image = root / 'cache/SIFT/initial_oram.sgx-sim.state'
            assert sgx_image.is_file() and sgx_image.stat().st_mode & 0o777 == 0o600
            assert 'oram_snapshot_status=built' in sgx_first.stderr
            assert 'oram_snapshot_status=loaded' in sgx_second.stderr
            assert len(sgx_rows) == 2 and len(list(csv.DictReader(output.open()))) == 4
            assert all(float(row['recall_at_k']) == 1 for row in sgx_rows)
            sgx_rebuilt = subprocess.run(sgx_command + ['--rebuild-oram'], check=True, env=env,
                                         capture_output=True, text=True)
            assert 'oram_snapshot_status=built' in sgx_rebuilt.stderr
        p['data']['storage_scalar'] = 'float32'
        config.write_text(json.dumps(cfg))
        new_type = subprocess.run(command, check=True, env=env, capture_output=True, text=True)
        assert 'oram_snapshot_status=built' in new_type.stderr
        assert len(list((root / 'cache/SIFT').glob('initial_oram.native.state'))) == 1
        float_prepared = next(path.parent / 'prepared.bin' for path in (root / 'cache/SIFT').glob('*/manifest.json')
                              if json.loads(path.read_text())['identity']['scalar'] == 'float32')
        float_calibration = subprocess.run([sys.executable, str(ROOT / 'tools/calibrate_topk.py'),
                                            '--config', str(config), '--data-root', str(root),
                                            '--prepared', str(float_prepared), '--nprobe', '4', '--k2', '32',
                                            '--alpha', '2', '--queries', '1'],
                                           check=True, env=env, capture_output=True, text=True)
        assert len(json.loads(float_calibration.stdout.splitlines()[-1])) == 4
        if sgx:
            sgx_new_type = subprocess.run(command + ['--mode', 'sgx-sim', '--sgx-build-dir', str(sgx)], check=True,
                                          env=env, capture_output=True, text=True)
            assert 'oram_snapshot_status=built' in sgx_new_type.stderr
            assert len(list(csv.DictReader(sgx_output.open()))) == 4
        assert float(list(csv.DictReader(output.open()))[-1]['recall_at_k']) == 1
        # Unlike the all-cluster cases above, nprobe < nc exercises compact
        # centroid selection and its reusable scratch in the 128D runner.
        config_before_override = config.read_bytes()
        limited_output = root / 'limited.csv'
        limited_command = command + ['--nprobe', '2', '--k2', '16', '--csv', str(limited_output)]
        limited_run = subprocess.run(limited_command, check=True, env=env, capture_output=True, text=True)
        assert 'Offline cache/Verify prepared data: 100%' in limited_run.stderr
        assert 'oram_snapshot_status=loaded' in limited_run.stderr
        native_limited = list(csv.DictReader(benchmark.dataset_csv_path(
            limited_output, 'SIFT', 'native').open()))[-1]
        assert int(native_limited['n_probe']) == 2 and int(native_limited['k2']) == 16
        limited_cached = subprocess.run(limited_command, check=True, env=env, capture_output=True, text=True)
        assert 'oram_snapshot_status=loaded' in limited_cached.stderr
        for overrides, expected in [(['--nprobe', '2'], (2, 32)), (['--k2', '16'], (4, 16))]:
            overridden = subprocess.run(command + overrides + ['--csv', str(limited_output)], check=True,
                                        env=env, capture_output=True, text=True)
            assert 'oram_snapshot_status=loaded' in overridden.stderr
            row = list(csv.DictReader(benchmark.dataset_csv_path(limited_output, 'SIFT', 'native').open()))[-1]
            assert (int(row['n_probe']), int(row['k2'])) == expected
        assert config.read_bytes() == config_before_override
        limited_csv = benchmark.dataset_csv_path(limited_output, 'SIFT', 'native')
        csv_before_invalid = limited_csv.read_bytes()
        for overrides, message in [(['--nprobe', '0'], 'nprobe must be a positive integer'),
                                   (['--k2', '9'], 'k2 must be an integer >= k'),
                                   (['--nprobe', '2', '4', '--k2', '16'], 'same number of values'),
                                   (['--nprobe', '1000'], 'nprobe exceeds the resolved cluster count'),
                                   (['--nprobe', '1', '--k2', '33'], 'k2 exceeds the quantized candidate capacity'),
                                   (['--config', str(full_config), '--candidate-selection', 'approx',
                                     '--nprobe', '2', '--use-config-quotas'], 'one rank_fraction per probed cluster'),
                                   (['--candidate-selection', 'approx', '--alpha', '.5'], 'alpha must be finite'),
                                   (['--candidate-selection', 'approx', '--alpha', 'nan'], 'alpha must be finite'),
                                   (['--candidate-selection', 'approx', '--alpha', '1000'], 'available partition capacity'),
                                   (['--candidate-selection', 'approx', '--calibration-queries', '0'], 'positive integer'),
                                   (['--candidate-selection', 'approx', '--calibration-queries', '5'], 'queries exceed dataset'),
                                   (['--candidate-selection', 'exact', '--alpha', '2'], 'automatic approx selection'),
                                   (['--candidate-selection', 'approx', '--use-config-quotas', '--alpha', '2'],
                                    'automatic approx selection')]:
            failed_override = subprocess.run(command + overrides + ['--csv', str(limited_output)],
                                             env=env, capture_output=True, text=True)
            assert failed_override.returncode != 0 and message in failed_override.stderr, (overrides, failed_override.stderr)
        assert limited_csv.read_bytes() == csv_before_invalid
        approx_override_cfg = copy.deepcopy(cfg)
        approx_override_cfg['datasets']['SIFT']['query']['candidate_selection'] = {
            'mode': 'approx', 'rank_fractions': [1, 1]}
        approx_override_config = root / 'approx_override.json'
        approx_override_config.write_text(json.dumps(approx_override_cfg))
        approx_override_bytes = approx_override_config.read_bytes()
        approx_override_command = limited_command + ['--config', str(approx_override_config), '--use-config-quotas']
        approx_overridden = subprocess.run(approx_override_command, check=True, env=env, capture_output=True, text=True)
        assert 'oram_snapshot_status=loaded' in approx_overridden.stderr
        approx_override_row = list(csv.DictReader(benchmark.dataset_csv_path(
            limited_output, 'SIFT', 'native', 'approx').open()))[-1]
        assert (int(approx_override_row['n_probe']), int(approx_override_row['k2'])) == (2, 16)
        assert float(approx_override_row['alpha']) == 2
        assert approx_override_config.read_bytes() == approx_override_bytes
        # A .bin base with a precomputed unbounded assignment skips k-means.
        bin_cfg = copy.deepcopy(cfg)
        with (root / 'base.bin').open('wb') as stream:
            stream.write(struct.pack('<II', len(vectors), 128))
            stream.write(vectors.tobytes())
        write_vecs(root / 'labels.ivecs',
                   (np.arange(len(vectors), dtype=np.int32) % 4).reshape(-1, 1), False)
        bin_cfg['datasets']['SIFT']['files']['base'] = 'base.bin'
        bin_cfg['datasets']['SIFT']['data']['storage_scalar'] = 'uint8'
        bin_cfg['datasets']['SIFT']['clustering']['precomputed_labels'] = 'labels.ivecs'
        bin_config = root / 'bin_config.json'
        bin_config.write_text(json.dumps(bin_cfg))
        bin_run = subprocess.run(command + ['--config', str(bin_config), '--cache-dir', str(root / 'bin-cache'),
                                            '--csv', str(root / 'bin.csv')],
                                 check=True, env=env, capture_output=True, text=True)
        assert 'Offline cache/Hash cluster labels: 100%' in bin_run.stderr
        assert 'Offline clustering/Split by capacity: 100%' in bin_run.stderr
        assert 'oram_snapshot_status=built' in bin_run.stderr
        bin_row = list(csv.DictReader(benchmark.dataset_csv_path(root / 'bin.csv', 'SIFT', 'native').open()))[-1]
        assert float(bin_row['recall_at_k']) == 1
        # A 96-dimensional float32 dataset exercises the second runner instantiation.
        vectors96 = rng.normal(size=(32, 96)).astype('<f4')
        queries96 = vectors96[:4]
        distances96 = ((queries96[:, None, :] - vectors96[None, :, :]) ** 2).sum(axis=2)
        truth96 = np.argsort(distances96, axis=1, kind='stable')[:, :10]
        write_vecs(root / 'base96.fvecs', vectors96, True)
        write_vecs(root / 'query96.fvecs', queries96, True)
        write_vecs(root / 'gt96.ivecs', truth96, False)
        dim96_cfg = copy.deepcopy(cfg)
        dim96_cfg['datasets']['SIFT']['files'].update(base='base96.fvecs', query='query96.fvecs',
                                                      groundtruth='gt96.ivecs')
        dim96_config = root / 'dim96_config.json'
        dim96_config.write_text(json.dumps(dim96_cfg))
        dim96_run = subprocess.run(command + ['--config', str(dim96_config),
                                              '--cache-dir', str(root / 'd96-cache'),
                                              '--csv', str(root / 'd96.csv')],
                                   check=True, env=env, capture_output=True, text=True)
        assert 'oram_snapshot_status=built' in dim96_run.stderr
        dim96_row = list(csv.DictReader(benchmark.dataset_csv_path(root / 'd96.csv', 'SIFT', 'native').open()))[-1]
        assert float(dim96_row['recall_at_k']) == 1
        # Multiple (nprobe, k2) pairs share one runner process and one ORAM
        # initialization; each finished job streams its report block and the
        # driver appends its CSV row while later jobs are still running.
        multi_csv = benchmark.dataset_csv_path(root / 'multi.csv', 'SIFT', 'native')
        multi_run = subprocess.run(command + ['--nprobe', '2', '4', '--k2', '16', '32',
                                              '--csv', str(root / 'multi.csv')],
                                   check=True, env=env, capture_output=True, text=True)
        assert multi_run.stderr.count('oram_snapshot_status=loaded') == 1
        assert multi_run.stdout.count('Appended one summary') == 2
        multi_rows = list(csv.DictReader(multi_csv.open()))
        assert [(int(r['n_probe']), int(r['k2'])) for r in multi_rows] == [(2, 16), (4, 32)]
        assert all(float(r['online_ms']) >= 0 and float(r['online_ms']) <= float(r['total_ms'])
                   for r in multi_rows)
        # --resume skips jobs whose summary rows are already recorded.
        multi_bytes = multi_csv.read_bytes()
        resumed = subprocess.run(command + ['--nprobe', '2', '4', '--k2', '16', '32', '--resume',
                                            '--csv', str(root / 'multi.csv')],
                                 check=True, env=env, capture_output=True, text=True)
        assert 'nothing to run' in resumed.stderr and 'oram_snapshot_status' not in resumed.stderr
        assert multi_csv.read_bytes() == multi_bytes
        partial = subprocess.run(command + ['--nprobe', '2', '3', '--k2', '16', '24', '--resume',
                                            '--csv', str(root / 'multi.csv')],
                                 check=True, env=env, capture_output=True, text=True)
        assert 'skipping 1 job(s)' in partial.stderr
        assert partial.stderr.count('oram_snapshot_status=loaded') == 1
        multi_rows = list(csv.DictReader(multi_csv.open()))
        assert [(int(r['n_probe']), int(r['k2'])) for r in multi_rows] == [(2, 16), (4, 32), (3, 24)]
        if sgx:
            sgx_limited_run = subprocess.run(limited_command + ['--mode', 'sgx-sim', '--sgx-build-dir', str(sgx)],
                                            check=True, env=env, capture_output=True, text=True)
            assert 'oram_snapshot_status=loaded' in sgx_limited_run.stderr
            sgx_limited = list(csv.DictReader(benchmark.dataset_csv_path(
                limited_output, 'SIFT', 'sgx-sim').open()))[-1]
            assert int(sgx_limited['n_probe']) == 2 and int(sgx_limited['k2']) == 16
            assert float(native_limited['recall_at_k']) == float(sgx_limited['recall_at_k'])
            sgx_overridden = subprocess.run(approx_override_command + ['--mode', 'sgx-sim', '--sgx-build-dir', str(sgx)],
                                           check=True, env=env, capture_output=True, text=True)
            assert 'oram_snapshot_status=loaded' in sgx_overridden.stderr
            sgx_approx_override = list(csv.DictReader(benchmark.dataset_csv_path(
                limited_output, 'SIFT', 'sgx-sim', 'approx').open()))[-1]
            assert (int(sgx_approx_override['n_probe']), int(sgx_approx_override['k2'])) == (2, 16)
            assert float(sgx_approx_override['alpha']) == 2
            # Independent native/SGX Setup shuffles can produce different
            # approximate partitions, so their partial-budget recall can differ.
            assert 0 <= float(sgx_approx_override['recall_at_k']) <= 1
            assert 0 <= float(approx_override_row['recall_at_k']) <= 1
        # Approx calibrates with CLI parameters despite stale JSON fractions.
        # Integer quotas must be passed directly: ceil(1.5*17)=26, without a
        # fraction round-trip changing the requested representative budget.
        automatic_cfg = copy.deepcopy(cfg)
        automatic_cfg['datasets']['SIFT']['query']['candidate_selection'] = {'mode': 'approx', 'rank_fractions': []}
        automatic_config = root / 'automatic.json'
        automatic_config.write_text(json.dumps(automatic_cfg))
        config_bytes = automatic_config.read_bytes()
        automatic_command = command + ['--config', str(automatic_config), '--candidate-selection', 'approx',
                                       '--nprobe', '3', '--k2', '17', '--alpha', '1.5',
                                       '--calibration-queries', '2', '--csv', str(root / 'automatic.csv')]
        for runtime in ([], ['--mode', 'sgx-sim', '--sgx-build-dir', str(sgx)]) if sgx else ([],):
            image_now = root / ('cache/SIFT/initial_oram.sgx-sim.state' if runtime
                                else 'cache/SIFT/initial_oram.native.state')
            initial_bytes = image_now.read_bytes()
            automatic_run = subprocess.run(automatic_command + runtime, check=True, env=env,
                                           capture_output=True, text=True)
            assert 'oram_snapshot_status=loaded' in automatic_run.stderr
            assert 'Approximate quotas/Calibrate exact quantized top-k2: 100%' in automatic_run.stderr
            assert 'calibration_queries=2' in automatic_run.stderr
            assert automatic_config.read_bytes() == config_bytes
            assert not automatic_config.with_name(automatic_config.name + '.pending').exists()
            assert image_now.read_bytes() == initial_bytes
            mode = 'sgx-sim' if runtime else 'native'
            row = list(csv.DictReader(benchmark.dataset_csv_path(root / 'automatic.csv', 'SIFT', mode, 'approx').open()))[-1]
            assert (int(row['n_probe']), int(row['k2'])) == (3, 17)
            assert abs(float(row['alpha']) - 26 / 17) < 1e-12
        # Restore a snapshot made with a smaller window and one worker into a
        # larger query window and a different worker count, without BulkLoad.
        for runtime in ([], ['--mode', 'sgx-sim', '--sgx-build-dir', str(sgx)]) if sgx else ([],):
            bootstrap = subprocess.run(limited_command + runtime + ['--threads', '1', '--rebuild-oram'],
                                       check=True, env=env, capture_output=True, text=True)
            assert 'oram_snapshot_status=built' in bootstrap.stderr
            image_now = root / ('cache/SIFT/initial_oram.sgx-sim.state' if runtime
                                else 'cache/SIFT/initial_oram.native.state')
            snapshot_before_changes = image_now.read_bytes()
            changed_env = {**env, 'OMP_NUM_THREADS': '2'}
            enlarged = subprocess.run(command + runtime + ['--csv', str(root / 'enlarged.csv')],
                                      check=True, env=changed_env, capture_output=True, text=True)
            assert 'oram_snapshot_status=loaded' in enlarged.stderr
            assert 'Offline cache/Verify prepared data: 100%' in enlarged.stderr
            assert 'Offline clustering/FAISS' not in enlarged.stderr and 'ORAM Setup/FuORAM BulkLoad' not in enlarged.stderr
            assert image_now.read_bytes() == snapshot_before_changes
            mode = 'sgx-sim' if runtime else 'native'
            enlarged_row = list(csv.DictReader(benchmark.dataset_csv_path(
                root / 'enlarged.csv', 'SIFT', mode).open()))[-1]
            assert (int(enlarged_row['n_probe']), int(enlarged_row['k2'])) == (4, 32)
            assert float(enlarged_row['recall_at_k']) == 1
        # An old manifest with build/environment fields remains reusable.
        float_manifest = float_prepared.parent / 'manifest.json'
        legacy_meta = json.loads(float_manifest.read_text())
        legacy_meta['identity'].pop('format_version')
        legacy_meta['identity'].update(converter_sha256='old-converter', clustering_source_sha256='old-source',
                                       numpy_version='old-numpy', faiss_version='old-faiss', faiss_threads=99)
        float_manifest.write_text(json.dumps(legacy_meta))
        legacy_reused = subprocess.run(command + ['--csv', str(root / 'legacy_cache.csv')],
                                       check=True, env=env, capture_output=True, text=True)
        assert 'Offline cache/Verify prepared data: 100%' in legacy_reused.stderr
        assert 'oram_snapshot_status=loaded' in legacy_reused.stderr
        legacy_calibration = subprocess.run(
            [sys.executable, str(ROOT / 'tools/calibrate_topk.py'), '--config', str(config),
             '--data-root', str(root), '--cache-dir', str(root / 'cache'),
             '--nprobe', '4', '--k2', '32', '--alpha', '2', '--queries', '1'],
            check=True, env=env, capture_output=True, text=True)
        assert len(json.loads(legacy_calibration.stdout.splitlines()[-1])) == 4
        # Forest snapshots keep R/E, but remain reusable across query jobs and
        # odd access thread counts; every subtree uses shared-top eviction.
        forest_cfg = copy.deepcopy(cfg)
        forest_cfg['datasets']['SIFT']['oram'].update(A=1)
        forest_cfg['datasets']['SIFT']['oram']['qu'] = {'R': 1, 'E': 2}
        forest_cfg['datasets']['SIFT']['oram']['fu'] = {'R': 1, 'E': 2}
        forest_config = root / 'forest.json'
        forest_config.write_text(json.dumps(forest_cfg))
        forest_command = command + ['--config', str(forest_config), '--csv', str(root / 'forest.csv')]
        for runtime in ([], ['--mode', 'sgx-sim', '--sgx-build-dir', str(sgx)]) if sgx else ([],):
            mode = 'sgx-sim' if runtime else 'native'
            forest_image = root / f'cache/SIFT/initial_oram.{mode}.state'
            forest_first = subprocess.run(forest_command + runtime + ['--qu-routing-depth', '1', '--qu-evict-batch', '2',
                                                                      '--fu-routing-depth', '1', '--fu-evict-batch', '2'],
                                          check=True, env=env,
                                          capture_output=True, text=True)
            assert 'oram_snapshot_status=built' in forest_first.stderr
            assert 'Offline cache/Verify prepared data: 100%' in forest_first.stderr
            saved = forest_image.read_bytes()
            forest_second = subprocess.run(forest_command + runtime + ['--threads', '3', '--nprobe', '2', '--k2', '16'],
                                           check=True, env=env, capture_output=True, text=True)
            assert 'oram_snapshot_status=loaded' in forest_second.stderr
            assert forest_image.read_bytes() == saved
            rows = list(csv.DictReader(benchmark.dataset_csv_path(root / 'forest.csv', 'SIFT', mode).open()))
            assert float(rows[0]['recall_at_k']) == 1
            # Changing only the Fu E must invalidate the same-R snapshot.
            forest_cfg['datasets']['SIFT']['oram']['fu']['E'] = 1
            forest_config.write_text(json.dumps(forest_cfg))
            changed_e = subprocess.run(forest_command + runtime, check=True, env=env,
                                       capture_output=True, text=True)
            assert 'oram_snapshot_status=built' in changed_e.stderr
            forest_cfg['datasets']['SIFT']['oram']['fu']['E'] = 2
            forest_config.write_text(json.dumps(forest_cfg))
        # Invalid forest geometry is caught before starting the C++ runner.
        forest_cfg['datasets']['SIFT']['oram']['qu']['R'] = 31
        forest_config.write_text(json.dumps(forest_cfg))
        invalid_forest = subprocess.run(forest_command, env=env, capture_output=True, text=True)
        assert invalid_forest.returncode != 0 and 'ORAM eviction schedule exceeds uint32 capacity' in invalid_forest.stderr
        # ORAM geometry changes rebuild only the ORAM; clustering changes
        # rebuild the prepared data and then the ORAM as well.
        changed_cfg = copy.deepcopy(cfg)
        changed_cfg['datasets']['SIFT']['oram']['S'] += 1
        changed_config = root / 'changed_oram.json'
        changed_config.write_text(json.dumps(changed_cfg))
        geometry_run = subprocess.run(command + ['--config', str(changed_config), '--csv', str(root / 'geometry.csv')],
                                      check=True, env=env, capture_output=True, text=True)
        assert 'Offline cache/Verify prepared data: 100%' in geometry_run.stderr
        assert 'oram_snapshot_status=built' in geometry_run.stderr
        changed_cfg = copy.deepcopy(cfg)
        changed_cfg['datasets']['SIFT']['clustering']['seed'] += 1
        changed_config.write_text(json.dumps(changed_cfg))
        clustering_run = subprocess.run(command + ['--config', str(changed_config), '--csv', str(root / 'clustering.csv')],
                                        check=True, env=env, capture_output=True, text=True)
        assert 'Offline clustering/FAISS k-means: 100%' in clustering_run.stderr
        assert 'Offline quantization/Encode vectors: 100%' in clustering_run.stderr
        assert 'oram_snapshot_status=built' in clustering_run.stderr
        print('Benchmark pipeline, recall, counters and append-only CSV passed')


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(error.stdout or '', file=sys.stderr)
        print(error.stderr or '', file=sys.stderr)
        raise
