"""Public CLI regression checks for CSV experiment orchestration."""
import csv
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCHEMES = ('qcovert_co', 'encrypted_ivf_rabitq', 'qcovert_ps', 'compass_tee', 'bison_tee', 'compass', 'bison')


class ExperimentsTest(unittest.TestCase):
    def cli(self, scheme, *args, good=True):
        result = subprocess.run([sys.executable, str(ROOT / f'scripts/run_{scheme}.py'), *map(str, args)],
                                cwd='/tmp', text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, good, result.stdout + result.stderr)
        return result

    def commands(self, result):
        return [shlex.split(line[2:]) for line in result.stdout.splitlines() if line.startswith('$ ')]

    def test_every_supported_csv_and_dry_run_is_read_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for scheme in SCHEMES:
                for dataset in ('sift', 'sift10m', 'deep1m', 'msmarco1m', 'sift100m'):
                    missing = dataset == 'sift100m' and scheme in ('compass', 'bison', 'compass_tee', 'bison_tee')
                    with self.subTest(scheme=scheme, dataset=dataset):
                        result = self.cli(scheme, '--dataset', dataset.upper(), '--queries', 2,
                                          '--data-root', root / 'absent data', '--results-dir', root / 'results',
                                          '--rows', '1,3-4', '--dry-run', good=not missing)
                        if missing:
                            self.assertIn('parameter CSV missing', result.stderr)
                        else:
                            self.assertIn('selected rows: 1,3,4', result.stdout)
                            self.assertTrue(self.commands(result))
            self.assertEqual(list(root.iterdir()), [])

    def test_tee_and_original_share_csv_and_preserve_row_pairing(self):
        for family in ('compass', 'bison'):
            for scheme in (family, family + '_tee'):
                result = self.cli(scheme, '--dry-run', '--rows', '1,3', '--queries', 2)
                expected = 'Compass_SIFT.csv' if family == 'compass' else 'bison_SIFT.csv'
                self.assertIn(expected, result.stdout)
                commands = self.commands(result)
                if family == 'bison':
                    command = commands[-1]
                    flags = ('--iterations', '--beamwidth', '--l-search') if scheme.endswith('tee') else ('--t', '--w', '--l')
                    if scheme.endswith('tee'):
                        self.assertEqual(command[command.index(flags[0]) + 1:command.index(flags[1])], ['5', '5'])
                        self.assertEqual(command[command.index(flags[1]) + 1:command.index(flags[2])], ['25', '31'])
                        self.assertEqual(command[command.index(flags[2]) + 1:], ['125', '125'])
                    else:
                        self.assertEqual(command[command.index('--t') + 1], '5,5')
                        self.assertEqual(command[command.index('--w') + 1], '25,31')
                        self.assertEqual(command[command.index('--pairing') + 1], 'zip')
                elif scheme.endswith('tee'):
                    self.assertEqual(commands[-1][-3:], ['--ef', '16', '24'])
                    self.assertEqual(commands[-1][commands[-1].index('--ef-spec') + 1], '4')
                else:
                    self.assertEqual([next(value for value in command if value.startswith('ef_search='))
                                      for command in commands if 'r=2' in command], ['ef_search=16', 'ef_search=24'])

    def test_k_filter_does_not_change_csv_budgets(self):
        with tempfile.TemporaryDirectory() as temporary:
            params = Path(temporary) / 'params.csv'
            params.write_text('nprobe,k2\n2,60\n3,100\n4,200\n')
            before = params.read_bytes()
            for scheme in ('qcovert_co', 'encrypted_ivf_rabitq', 'qcovert_ps'):
                result = self.cli(scheme, '--params', params, '--k', 100, '--dry-run')
                self.assertIn('Skipping CSV row 1', result.stdout)
                command = self.commands(result)[-1]
                if scheme == 'qcovert_ps':
                    self.assertEqual(command[command.index('--nprobe') + 1:command.index('--k2')], ['3', '4'])
                    self.assertEqual(command[command.index('--k2') + 1:], ['100', '200'])
                else:
                    self.assertEqual(command[command.index('--nprobes') + 1], '3,4')
                    self.assertEqual(command[command.index('--k2') + 1], '100,200')
            self.assertEqual(params.read_bytes(), before)
            self.cli('qcovert_co', '--params', params, '--rows', 1, '--k', 100, '--dry-run', good=False)

    def test_custom_group_budgets_never_make_cartesian_products(self):
        with tempfile.TemporaryDirectory() as temporary:
            params = Path(temporary) / 'params.csv'
            params.write_text('T,W,L\n3,7,100\n8,2,200\n5,4,100\n')
            result = self.cli('bison', '--path', 'plain', '--params', params, '--dry-run')
            commands = [command for command in self.commands(result) if 'plain' in command]
            self.assertEqual(len(commands), 3)
            self.assertEqual([(command[command.index('--t') + 1], command[command.index('--w') + 1],
                               command[command.index('--l') + 1]) for command in commands],
                             [('3', '7', '100'), ('8', '2', '200'), ('5', '4', '100')])
            params.write_text('ef,ef_spec,ef_n\n16,4,12\n24,3,10\n36,3,10\n48,4,12\n')
            result = self.cli('compass_tee', '--params', params, '--dry-run')
            commands = [command for command in self.commands(result) if '--ef' in command]
            self.assertEqual([command[command.index('--ef') + 1:] for command in commands],
                             [['16'], ['24', '36'], ['48']])

    def test_queries_zero_and_prepare_stage(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'sift').mkdir()
            query = root / 'sift/sift_query.fvecs'
            query.write_bytes((struct.pack('<I', 128) + bytes(128 * 4)) * 7)
            for scheme in ('qcovert_co', 'qcovert_ps', 'compass_tee', 'bison_tee', 'compass', 'bison'):
                result = self.cli(scheme, '--data-root', root, '--queries', 0, '--rows', 1, '--dry-run')
                self.assertIn('queries=7', result.stdout)
            result = self.cli('compass_tee', '--dataset', 'sift100m', '--stage', 'prepare', '--dry-run')
            self.assertNotIn('parameter CSV missing', result.stderr)
            self.assertNotIn('/benchmark.py', result.stdout)
            result = self.cli('qcovert_ps', '--stage', 'search', '--rows', 1, '--dry-run')
            self.assertIn('--require-prepared', result.stdout)

    def test_reference_uses_supported_native_arguments(self):
        result = self.cli('compass', '--path', 'reference', '--rows', 1, '--dry-run')
        command = self.commands(result)[-1]
        self.assertTrue(any(value.startswith('f_accuracy=') for value in command))
        self.assertTrue(any(value.startswith('f_latency=') for value in command))
        self.assertFalse(any(value.startswith('f_csv=') for value in command))

    def test_encrypted_baseline_resolves_automatic_batching(self):
        result = self.cli('encrypted_ivf_rabitq', '--rows', '1,3', '--dry-run')
        command = self.commands(result)[-1]
        self.assertEqual(command[command.index('--path-batch-clusters') + 1], '25')

    def test_low_bison_does_not_clone_a_disabled_position_map_oram(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / 'prepared/bison/quickstart/low-storage/sift/config.json'
            config.parent.mkdir(parents=True)
            data = json.loads((ROOT / 'bison/config/bench_low_sift.json').read_text())
            config.write_text(json.dumps(data))
            before = config.read_bytes()
            result = self.cli('bison', '--data-root', root, '--path', 'low-storage',
                              '--stage', 'search', '--rows', 1, '--dry-run')
            self.assertIn('pq_buckets.bin', result.stdout)
            self.assertNotIn('[state] ' + data['sift']['posmap_oram_buckets_path'], result.stdout)
            self.assertNotIn('--role posmap', result.stdout)
            self.assertEqual(config.read_bytes(), before)

    def test_standalone_build_locations(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / 'CMakeCache.txt').write_text(
                f'CMAKE_HOME_DIRECTORY:INTERNAL={ROOT / "Q-COVERT-PS"}\n')
            result = self.cli('compass_tee', '--build-dir', build, '--rows', 1, '--dry-run')
            command = self.commands(result)[-1]
            self.assertEqual(command[command.index('--build-dir') + 1], str(build))
            (build / 'bin').mkdir()
            (build / 'bin/build_disk_index').touch()
            result = self.cli('bison', '--build-dir', build, '--rows', 1, '--dry-run')
            command = self.commands(result)[-1]
            self.assertEqual(command[command.index('--build-dir') + 1], str(build / 'bin'))

    def test_reject_invalid_options_before_preparation(self):
        for args in (('--k', '0'), ('--k', '101'), ('--queries', '-1'), ('--rows', '0'),
                     ('--rows', '1000'), ('--rows', '4-2'), ('--rebuild', '--stage', 'search')):
            with self.subTest(args=args):
                self.cli('qcovert_co', *args, '--dry-run', good=False)
        with tempfile.TemporaryDirectory() as temporary:
            params = Path(temporary) / 'params.csv'
            for payload in ('nprobe,k2\n1,2,3\n', 'nprobe,k2\n0,100\n', 'ef\n16\n'):
                params.write_text(payload)
                self.cli('qcovert_co', '--params', params, '--dry-run', good=False)


if __name__ == '__main__':
    unittest.main()
