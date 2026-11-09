"""Small-file checks for the unified download/preparation entry points."""
import gzip
import http.server
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from data.vector_io import read_groundtruth_bin, read_vecs, read_vector_bin, write_vecs


class DataToolsTest(unittest.TestCase):
    def prepare(self, root, *args, check=True):
        return subprocess.run([sys.executable, str(ROOT / 'data/prepare_datasets.py'),
                               '--data-root', str(root), '--faiss-threads', '1', *map(str, args)],
                              cwd=root.parent, check=check, capture_output=True, text=True,
                              env={**os.environ, 'OMP_NUM_THREADS': '1', 'OPENBLAS_NUM_THREADS': '1'})

    def fixture_sources(self, root):
        rng = np.random.default_rng(17)
        bigann = rng.integers(0, 256, (24, 7), dtype='u1')
        bigann_query = bigann[[0, 9, 20]]
        folder = root / 'sift1B'
        folder.mkdir(parents=True)
        for name, values in [('base', bigann), ('query', bigann_query)]:
            payload = b''.join(struct.pack('<I', 7) + row.tobytes() for row in values)
            with gzip.open(folder / f'bigann_{name}.bvecs.gz', 'wb') as stream:
                stream.write(payload)
        passages = rng.normal(size=(32, 6)).astype('<f4')
        passage_query = passages[[1, 7, 20]]
        folder = root / 'msmarco1m'
        folder.mkdir()
        write_vecs(folder / 'passages.fvecs', passages, 'float')
        write_vecs(folder / 'queries.fvecs', passage_query, 'float')
        deep = rng.normal(size=(16, 96)).astype('<f4')
        deep_query = deep[[0, 3, 14]]
        folder = root / 'deep1M/source'
        (folder / 'base').mkdir(parents=True)
        write_vecs(folder / 'base/base_00', deep, 'float')
        with (folder / 'base/base_00').open('ab') as stream:
            stream.write(b'partial trailing record')
        write_vecs(folder / 'deep1B_queries.fvecs', deep_query, 'float')
        # The unified processor must ignore downloaded GT and recompute it from the selected base.
        (folder / 'gt.zip').write_bytes(b'not an archive')
        write_vecs(folder / 'deep1M_groundtruth.ivecs', np.full((3, 3), 15), 'int')
        return bigann, bigann_query, passages, passage_query, deep, deep_query

    def assert_gt(self, base, queries, truth):
        distances = ((queries.astype('f4')[:, None] - base.astype('f4')[None]) ** 2).sum(axis=2)
        expected = np.argsort(distances, axis=1)[:, :truth.shape[1]]
        np.testing.assert_array_equal(truth, expected)

    def test_all_datasets_and_exact_gt_for_every_derived_base(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'data'
            bigann, bq, passages, pq, deep, dq = self.fixture_sources(root)
            sift = root / 'sift'
            sift.mkdir()
            # Preserve official SIFT GT verbatim even when it differs from locally computed GT.
            official = np.array([[3, 2], [1, 0]], dtype='<i4')
            original_base = np.arange(28, dtype='f4').reshape(4, 7)
            for name, values, kind in [('sift_base.fvecs', original_base, 'float'),
                                       ('sift_query.fvecs', original_base[:2], 'float'),
                                       ('sift_groundtruth.ivecs', official, 'int')]:
                write_vecs(sift / name, values, kind)
            with tarfile.open(sift / 'sift.tar.gz', 'w:gz') as archive:
                for name in ('sift_base.fvecs', 'sift_query.fvecs', 'sift_groundtruth.ivecs'):
                    archive.add(sift / name, arcname='sift/' + name)
                    (sift / name).unlink()
                item = tarfile.TarInfo('../unexpected')
                item.size = 4
                archive.addfile(item, io.BytesIO(b'skip'))
            result = self.prepare(root, 'all', '--sift10m-count', 8, '--sift100m-count', 16,
                                  '--msmarco-count', 12, '--deep-count', 8, '--k', 3,
                                  '--base-batch', 5, '--query-batch', 2)
            self.assertIn('Exact L2 GT:', result.stdout)
            np.testing.assert_array_equal(read_vecs(sift / 'sift_groundtruth.ivecs', 'int'), official)
            self.assertFalse((root / 'unexpected').exists())
            selected = np.sort(np.random.default_rng(42).choice(32, 12, replace=False))
            for name, expected_base, expected_query in [('sift10M', bigann[:8], bq),
                                                       ('sift100M', bigann[:16], bq),
                                                       ('msmarco1m', passages[selected], pq),
                                                       ('deep1M', deep[:8], dq)]:
                folder = root / name
                base = read_vector_bin(folder / 'base.bin')
                np.testing.assert_array_equal(base, expected_base)
                if name == 'msmarco1m':
                    queries = read_vector_bin(folder / 'query.bin')
                    truth = read_groundtruth_bin(folder / 'gt.bin')
                else:
                    queries = read_vecs(folder / f'{name.lower()}_query.fvecs', 'float')
                    truth = read_vecs(folder / f'{name.lower()}_groundtruth.ivecs', 'int')
                np.testing.assert_array_equal(queries, expected_query)
                self.assertEqual(truth.shape, (3, 3))
                self.assert_gt(base, queries, truth)
                self.assertFalse(any(folder.glob('.prepare-*')))
            self.assertEqual(read_vector_bin(root / 'sift10M/base.bin').dtype, np.dtype('u1'))
            self.assertEqual(read_vector_bin(root / 'deep1M/base.bin').dtype, np.dtype('<f4'))
            # Re-running with another sample/GT width replaces the entire derived dataset.
            self.prepare(root, 'msmarco1m', '--msmarco-count', 10, '--seed', 7,
                         '--k', 4, '--base-batch', 3, '--query-batch', 2)
            selected = np.sort(np.random.default_rng(7).choice(32, 10, replace=False))
            np.testing.assert_array_equal(read_vector_bin(root / 'msmarco1m/base.bin'), passages[selected])
            self.assert_gt(passages[selected], pq, read_groundtruth_bin(root / 'msmarco1m/gt.bin'))
            # Failed preparation must not replace existing outputs.
            before = (root / 'deep1M/base.bin').read_bytes()
            dq[0, 0] = np.nan
            write_vecs(root / 'deep1M/source/deep1B_queries.fvecs', dq, 'float')
            failed = self.prepare(root, 'deep1m', '--deep-count', 7, '--k', 3, check=False)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual((root / 'deep1M/base.bin').read_bytes(), before)

    def test_legacy_bigann_bins_remain_supported(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'data'
            source = root / 'sift1B'
            source.mkdir(parents=True)
            base = np.arange(48, dtype='u1').reshape(12, 4)
            query = base[[0, 9]]
            for name, values in [('base', base), ('query', query)]:
                (source / f'{name}.bin').write_bytes(struct.pack('<II', *values.shape) + values.tobytes())
            self.prepare(root, 'sift10m', '--sift10m-count', 8, '--k', 4, '--base-batch', 3)
            np.testing.assert_array_equal(read_vector_bin(root / 'sift10M/base.bin'), base[:8])
            self.assert_gt(base[:8], query, read_vecs(root / 'sift10M/sift10m_groundtruth.ivecs', 'int'))

    def test_reject_malformed_bvecs_without_publishing_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'data'
            source = root / 'sift1B'
            source.mkdir(parents=True)
            valid = (struct.pack('<I', 4) + bytes([1, 2, 3, 4])) * 4
            (source / 'bigann_query.bvecs').write_bytes(valid[:8])
            for payload in (valid[:-1], valid[:8] + struct.pack('<I', 3) + valid[12:]):
                with self.subTest(payload=payload):
                    (source / 'bigann_base.bvecs').write_bytes(payload)
                    failed = self.prepare(root, 'sift10m', '--sift10m-count', 4, '--k', 2, check=False)
                    self.assertNotEqual(failed.returncode, 0)
                    self.assertFalse((root / 'sift10M/base.bin').exists())
                    self.assertEqual((source / 'bigann_base.bvecs').read_bytes(), payload)

    def test_invalid_counts_fail_before_replacing_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'data'
            self.fixture_sources(root)
            for dataset, count_option, count, k in [('deep1m', '--deep-count', 17, 3),
                                                   ('msmarco1m', '--msmarco-count', 33, 3),
                                                   ('sift10m', '--sift10m-count', 4, 5)]:
                with self.subTest(dataset=dataset):
                    failed = self.prepare(root, dataset, count_option, count, '--k', k, check=False)
                    self.assertNotEqual(failed.returncode, 0)

    def test_unified_downloader_sources_and_resume(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            root = work / 'raw data'
            tools = work / 'tools'
            tools.mkdir()
            calls = work / 'calls.jsonl'
            fake = tools / 'curl'
            fake.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ['MOCK_CURL_LOG'], 'a') as stream:
    stream.write(json.dumps(args) + '\\n')
if '--get' in args:
    name = next(arg[6:] for arg in args if arg.startswith('path=/'))
    print(json.dumps({'href': 'https://download.example/' + name}))
else:
    path = pathlib.Path(args[args.index('--output') + 1])
    with path.open('ab') as stream:
        stream.write(b'fixture')
    if os.environ.get('FAIL_QUERY') and args[-1].endswith('/deep1B_queries.fvecs'):
        raise SystemExit(22)
''')
            fake.chmod(0o755)
            env = {**os.environ, 'PATH': str(tools) + os.pathsep + os.environ['PATH'],
                   'MOCK_CURL_LOG': str(calls)}
            command = ['bash', str(ROOT / 'data/download_datasets.sh'), '--data-root', str(root)]
            dry = subprocess.run([*command, '--dry-run'], cwd=work, env=env,
                                 check=True, capture_output=True, text=True)
            self.assertEqual(len(dry.stdout.splitlines()), 7)
            self.assertFalse(root.exists())
            self.assertFalse(calls.exists())
            failed = subprocess.run(command, cwd=work, env={**env, 'FAIL_QUERY': '1'},
                                    capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0)
            partial = root / 'deep1M/source/deep1B_queries.fvecs.part'
            self.assertTrue(partial.is_file())
            subprocess.run(command, cwd=work, env=env, check=True, capture_output=True)
            self.assertEqual((root / 'deep1M/source/deep1B_queries.fvecs').read_bytes(), b'fixturefixture')
            subprocess.run(command, cwd=work, env=env, check=True, capture_output=True)
            invocations = [json.loads(line) for line in calls.read_text().splitlines()]
            resolutions = [args for args in invocations if '--get' in args]
            downloads = [args for args in invocations if '--get' not in args]
            self.assertEqual(len(resolutions), 3)
            self.assertEqual(len(downloads), 8)
            self.assertEqual({arg for args in resolutions for arg in args if arg.startswith('path=')},
                             {'path=/base/base_00', 'path=/deep1B_queries.fvecs'})
            self.assertTrue(all('--continue-at' in args for args in downloads))
            self.assertFalse(any('gt.zip' in arg or 'learn' in arg for args in downloads for arg in args))
            self.assertEqual(len([args for args in downloads if args[-1].startswith('ftp://')]), 3)
            self.assertFalse(partial.exists())
            alias = subprocess.run([*command, 'bigann', '--dry-run'], cwd=work, env=env,
                                   check=True, capture_output=True, text=True)
            self.assertEqual(len(alias.stdout.splitlines()), 2)

    def test_curl_resumes_a_real_http_range_download(self):
        payload = bytes(range(256)) * 32
        ranges = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                header = self.headers.get('Range')
                ranges.append(header)
                start = int(header.split('=')[1].split('-')[0]) if header else 0
                self.send_response(206 if header else 200)
                self.send_header('Content-Length', str(len(payload) - start))
                if header:
                    self.send_header('Content-Range', f'bytes {start}-{len(payload) - 1}/{len(payload)}')
                self.end_headers()
                self.wfile.write(payload[start:])

            def log_message(self, *args):
                pass

        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            root = work / 'data'
            (root / 'deep1M/source/base').mkdir(parents=True)
            (root / 'deep1M/source/base/base_00').write_bytes(b'already complete')
            partial = root / 'deep1M/source/deep1B_queries.fvecs.part'
            partial.write_bytes(payload[:1234])
            try:
                server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
            except PermissionError:
                self.skipTest('local sockets are not permitted in this execution sandbox')
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                # A curl wrapper only supplies the signed URL; the payload uses the real curl binary.
                real_curl = shutil.which('curl')
                tools = work / 'tools'
                tools.mkdir()
                wrapper = tools / 'curl'
                wrapper.write_text('''#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
if '--get' in args:
    print(json.dumps({'href': os.environ['TEST_DOWNLOAD_URL']}))
else:
    args[-1] = os.environ['LOCAL_DOWNLOAD_URL']
    os.execv(os.environ['REAL_CURL'], [os.environ['REAL_CURL'], '--noproxy', '*', *args])
''')
                wrapper.chmod(0o755)
                # The endpoint is local HTTP; the metadata still passes the HTTPS-only parser,
                # and the wrapper maps the resolved fixture URL to the local range server.
                env = {**os.environ, 'PATH': str(tools) + os.pathsep + os.environ['PATH'],
                       'REAL_CURL': real_curl, 'TEST_DOWNLOAD_URL': 'https://fixture.example/query',
                       'LOCAL_DOWNLOAD_URL': f'http://127.0.0.1:{server.server_port}/query'}
                subprocess.run(['bash', str(ROOT / 'data/download_datasets.sh'), 'deep1m',
                                '--data-root', str(root)], cwd=work, env=env,
                               check=True, capture_output=True)
                self.assertEqual((root / 'deep1M/source/deep1B_queries.fvecs').read_bytes(), payload)
                self.assertEqual(ranges, ['bytes=1234-'])
                self.assertFalse(partial.exists())
            finally:
                server.shutdown()
                server.server_close()
                thread.join()


if __name__ == '__main__':
    unittest.main()
