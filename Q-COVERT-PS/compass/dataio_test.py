#!/usr/bin/env python3
import struct
import tempfile
import unittest
from pathlib import Path
import numpy as np
import benchmark
import prepare
from dataio import ROOT, DATASETS, read_vectors, read_groundtruth, resolve_paths

def write_bin(path, values, groundtruth=False, vector_dtype='uint8'):
    with Path(path).open('wb') as out:
        out.write(struct.pack('<II', *values.shape))
        out.write(np.asarray(values, dtype='<u4' if groundtruth else ('u1' if vector_dtype == 'uint8' else '<f4')).tobytes())

class DataIOTest(unittest.TestCase):
    def test_profiles_and_overrides(self):
        for name, profile in DATASETS.items():
            source_root = Path('/external/datasets')
            p = resolve_paths(prepare.parser().parse_args(['--dataset', name.upper(), '--data-root', str(source_root)]), True)
            self.assertEqual(p.base, source_root / profile['directory'] / profile['base'])
            self.assertEqual(p.output, ROOT.parent / 'data/prepared/ps/compass' / profile['prepared'])
            expected_type = profile['scalar']
            self.assertEqual(p.base_dtype, expected_type)
            self.assertEqual(p.scalar, expected_type)
            b = resolve_paths(benchmark.parser().parse_args(['--dataset', name.upper(), '--data-root', str(source_root)]))
            self.assertEqual(b.query, source_root / profile['directory'] / profile['query'])
            self.assertEqual(b.groundtruth, source_root / profile['directory'] / profile['groundtruth'])
            self.assertEqual(b.prepared, p.output)
            self.assertEqual(b.query_dtype, expected_type)
            if name != 'sift':
                self.assertEqual(b.csv.name, f'compass_full_c_{name}_native.csv')
        p = resolve_paths(prepare.parser().parse_args(['--dataset', 'deep1m', '--base', '/custom/base.fvecs', '--output', '/custom/index']), True)
        self.assertEqual(p.base, Path('/custom/base.fvecs'))
        self.assertEqual(p.output, Path('/custom/index'))
        overridden = resolve_paths(prepare.parser().parse_args(['--dataset', 'deep1m', '--base-dtype', 'uint8', '--scalar', 'uint8']), True)
        self.assertEqual((overridden.base_dtype, overridden.scalar), ('uint8', 'uint8'))
        overridden = resolve_paths(benchmark.parser().parse_args(['--dataset', 'sift10m', '--query-dtype', 'float32']))
        self.assertEqual(overridden.query_dtype, 'float32')
        b = resolve_paths(benchmark.parser().parse_args(['--dataset', 'sift100m', '--query', '/custom/q', '--groundtruth', '/custom/gt', '--csv', '/custom/csv']))
        self.assertEqual((b.query, b.groundtruth, b.csv), tuple(map(Path, ['/custom/q', '/custom/gt', '/custom/csv'])))
        defaults = resolve_paths(benchmark.parser().parse_args([]))
        self.assertEqual(defaults.csv, ROOT / 'results/compass_full_c_sift_native.csv')
        self.assertEqual(defaults.query, ROOT.parent / 'data/sift/sift_query.fvecs')

    def test_bin_uint8_and_unsigned_groundtruth(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            values = np.array([[0, 255, 7], [3, 4, 5]], dtype='u1')
            write_bin(path / 'vectors.bin', values)
            np.testing.assert_array_equal(read_vectors(path / 'vectors.bin'), values)
            self.assertEqual(read_vectors(path / 'vectors.bin').dtype, np.dtype('u1'))
            ids = np.array([[0, 100000000, 0x80000001, 0xffffffff]], dtype='<u4')
            write_bin(path / 'gt.bin', ids, True)
            actual = read_groundtruth(path / 'gt.bin')
            np.testing.assert_array_equal(actual, ids)
            self.assertEqual(int(actual[0, 2]), 2147483649)
            (path / 'arbitrary.data').write_bytes((path / 'vectors.bin').read_bytes())
            np.testing.assert_array_equal(read_vectors(path / 'arbitrary.data', 'bin'), values)
            with self.assertRaises(ValueError):
                read_vectors(path / 'arbitrary.data')

    def test_bin_float32(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'vectors.bin'
            values = np.array([[1.25, -2.5, 0.125], [3.75, -4.25, 0]], dtype='<f4')
            write_bin(path, values, vector_dtype='float32')
            actual = read_vectors(path, vector_dtype='float32')
            self.assertIsInstance(actual, np.memmap)
            self.assertEqual(actual.dtype, np.dtype('<f4'))
            np.testing.assert_array_equal(actual, values)
            with self.assertRaises(ValueError):
                read_vectors(path, vector_dtype='uint8')
            del actual
            write_bin(path, np.array([[1, 2, 3]], dtype='u1'))
            with self.assertRaises(ValueError):
                read_vectors(path, vector_dtype='float32')

    def test_malformed_lengths_and_large_header(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'bad.bin'
            for payload in [b'', b'1234567', struct.pack('<II', 0, 128), struct.pack('<II', 2, 0),
                            struct.pack('<II', 2, 3) + b'12345', struct.pack('<II', 2, 3) + b'1234567',
                            struct.pack('<II', 100000000, 128), struct.pack('<II', 0xffffffff, 0xffffffff)]:
                path.write_bytes(payload)
                with self.assertRaises(ValueError):
                    read_vectors(path)
                with self.assertRaises(ValueError):
                    read_groundtruth(path)
            # A sparse file verifies a 100M-row header without allocating a
            # full array or requiring the real dataset on this machine.
            with path.open('wb') as out:
                out.write(struct.pack('<II', 100000000, 1))
                out.truncate(8 + 100000000)
            values = read_vectors(path)
            self.assertEqual(values.shape, (100000000, 1))
            self.assertEqual(int(values[-1, 0]), 0)

    def test_vecs_compatibility(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            vectors = np.array([[1.25, -2.5], [3, 4]], dtype='<f4')
            with (path / 'base.fvecs').open('wb') as out:
                for row in vectors:
                    out.write(struct.pack('<i', 2)); out.write(row.tobytes())
            np.testing.assert_array_equal(read_vectors(path / 'base.fvecs'), vectors)
            with (path / 'gt.ivecs').open('wb') as out:
                out.write(struct.pack('<iii', 2, 17, 42))
            np.testing.assert_array_equal(read_groundtruth(path / 'gt.ivecs'), [[17, 42]])
            invalid = bytearray((path / 'base.fvecs').read_bytes())
            struct.pack_into('<i', invalid, 12, 3)
            (path / 'bad.fvecs').write_bytes(invalid)
            with self.assertRaises(ValueError):
                read_vectors(path / 'bad.fvecs')

if __name__ == '__main__':
    unittest.main()
