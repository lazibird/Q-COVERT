"""Shared CSV parsing and orchestration for the seven experiment entry points."""
import argparse
from contextlib import contextmanager
import copy
import csv
import fcntl
import json
import math
import os
from pathlib import Path
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ROOT / 'Q-COVERT-PS/config/dataset_defaults.json'
CO_NAMES = {'sift': 'sift', 'sift10m': 'sift10M', 'sift100m': 'sift100M',
            'deep1m': 'deep1M', 'msmarco1m': 'msmarco1m'}
SCHEMES = {
    'qcovert_co': ('Q-COVERT-CO', 'QCOVERT_CO', ('nprobe', 'k2')),
    'encrypted_ivf_rabitq': ('Q-COVERT-CO', 'QCOVERT_CO', ('nprobe', 'k2')),
    'qcovert_ps': ('Q-COVERT-PS', 'QCOVERT_PS', ('nprobe', 'k2')),
    'compass_tee': ('Q-COVERT-PS/compass', 'Compass', ('ef', 'ef_spec', 'ef_n')),
    'bison_tee': ('Q-COVERT-PS/bison', 'bison', ('T', 'W', 'L')),
    'compass': ('Compass', 'Compass', ('ef', 'ef_spec', 'ef_n')),
    'bison': ('bison', 'bison', ('T', 'W', 'L')),
}


def positive(text):
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError('must be positive')
    return value


def parser(scheme):
    p = argparse.ArgumentParser(description=f'Run {scheme} using row-paired search parameters from CSV.')
    p.add_argument('--dataset', type=str.lower, choices=CO_NAMES, default='sift')
    p.add_argument('--queries', type=int, default=1000, help='queries per CSV row; 0 means all (default: 1000)')
    p.add_argument('--k', type=int, default=10, help='final top-k and recall@k, 1..100 (default: 10)')
    p.add_argument('--params', type=Path, help='input CSV; default data/search_params/<family>_<DATASET>.csv')
    p.add_argument('--rows', help='1-based CSV data rows, e.g. 1,3-5; default: all, in file order')
    p.add_argument('--stage', choices=['all', 'prepare', 'search'], default='all',
                   help='prepare offline indexes, search existing indexes, or both (default: all)')
    p.add_argument('--rebuild', action='store_true', help='rebuild derived indexes/initial states; requires prepare/all')
    p.add_argument('--threads', type=positive, help='override supported preparation/search thread controls')
    p.add_argument('--data-root', type=Path, default=ROOT / 'data', help='shared dataset root (default: repository data/)')
    p.add_argument('--build-dir', type=Path, default=ROOT / 'build',
                   help='root CMake build directory; standalone scheme builds are also accepted')
    p.add_argument('--results-dir', type=Path, help='result root; default <scheme>/results/<entry-point>/')
    p.add_argument('--dry-run', action='store_true', help='print the plan without changing files or starting processes')
    if scheme in ('qcovert_ps', 'compass_tee', 'bison_tee'):
        p.add_argument('--mode', choices=['native', 'sgx-sim', 'sgx-hw'], default='native')
    if scheme in ('compass', 'bison'):
        p.add_argument('--path', choices=['oram', 'low-storage', 'reference' if scheme == 'compass' else 'plain'],
                       default='oram', help='execution path (default: oram)')
    return p


def select_rows(text, count):
    if text is None:
        return set(range(1, count + 1))
    selected = set()
    for part in text.split(','):
        bounds = part.strip().split('-')
        if len(bounds) not in (1, 2) or any(not value.isdigit() for value in bounds):
            raise ValueError('--rows expects positive row numbers or ranges, e.g. 1,3-5')
        start, end = int(bounds[0]), int(bounds[-1])
        if not 1 <= start <= end <= count:
            raise ValueError(f'--rows range {part} is outside 1..{count}')
        selected.update(range(start, end + 1))
    return selected


def read_jobs(path, columns, rows, k):
    if not path.is_file():
        raise ValueError(f'search parameter CSV missing: {path}; supply --params for this dataset')
    with path.open(newline='', encoding='utf-8-sig') as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != list(columns):
            raise ValueError(f'{path}: expected CSV columns {",".join(columns)}')
        records = list(reader)
    if not records:
        raise ValueError(f'{path}: no parameter rows')
    selected = select_rows(rows, len(records))
    jobs = []
    for number, row in enumerate(records, 1):
        if number not in selected:
            continue
        try:
            values = tuple(int(row[name]) for name in columns)
        except (TypeError, ValueError) as error:
            raise ValueError(f'{path}: row {number} must contain integers') from error
        if None in row or any(value <= 0 for value in values):
            raise ValueError(f'{path}: row {number} must contain exactly {len(columns)} positive integers')
        budget = row.get('k2', row.get('L'))
        if budget is not None and int(budget) < k:
            print(f'Skipping CSV row {number}: candidate/list budget {budget} is less than k={k}.', flush=True)
            continue
        jobs.append((number, values))
    if not jobs:
        raise ValueError('no compatible search parameter rows remain for the requested k')
    return jobs


def vector_shape(path):
    """Inspect only public file shape; this does not scan or copy vector payloads."""
    path = Path(path)
    with path.open('rb') as stream:
        header = stream.read(8)
    if len(header) < 8:
        raise ValueError(f'truncated vector file: {path}')
    first, second = struct.unpack('<II', header)
    if path.suffix in ('.fvecs', '.ivecs'):
        size = 4 * (first + 1)
        if not first or path.stat().st_size % size:
            raise ValueError(f'invalid vecs size: {path}')
        return path.stat().st_size // size, first
    return first, second


def build_subdir(build, directory, executable):
    if (build / executable).is_file():
        return build
    if directory == 'bison/bin' and (build / 'bin' / executable).is_file():
        return build / 'bin'
    cache = build / 'CMakeCache.txt'
    if cache.is_file():
        source = ROOT / directory.split('/')[0]
        if f'CMAKE_HOME_DIRECTORY:INTERNAL={source}' in cache.read_text():
            return build / 'bin' if directory == 'bison/bin' else build
    return build / directory


def environment(threads):
    env = dict(os.environ)
    paths = [ROOT / '.venv/lib', ROOT / '.deps/usr/lib/x86_64-linux-gnu',
             ROOT / '.deps/usr/lib/x86_64-linux-gnu/blas', ROOT / '.deps/usr/lib/x86_64-linux-gnu/lapack']
    env['LD_LIBRARY_PATH'] = os.pathsep.join(str(path) for path in paths if path.is_dir()) + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    if threads:
        for name in ('OMP_NUM_THREADS', 'OPENBLAS_NUM_THREADS', 'MKL_NUM_THREADS'):
            env[name] = str(threads)
    return env


class Experiment:
    def __init__(self, scheme, args):
        self.scheme, self.args = scheme, args
        self.data = args.data_root.expanduser().resolve()
        self.build = args.build_dir.expanduser().resolve()
        self.profile = copy.deepcopy(json.loads(PROFILES.read_text())['datasets'][args.dataset.upper()])
        self.raw = {key: self.data.joinpath(*Path(value).parts[1:])
                    for key, value in self.profile['files'].items()}
        self.dataset = CO_NAMES[args.dataset]
        self.variant = getattr(args, 'path', getattr(args, 'mode', 'native'))
        result_root = args.results_dir or ROOT / SCHEMES[scheme][0] / 'results' / scheme
        self.output = result_root.expanduser().resolve() / args.dataset / self.variant / f'k{args.k}_q{args.queries}'
        family = SCHEMES[scheme][1]
        self.params = (args.params or ROOT / 'data/search_params' / f'{family}_{args.dataset.upper()}.csv').expanduser().resolve()
        self.jobs = [] if args.stage == 'prepare' else read_jobs(self.params, SCHEMES[scheme][2], args.rows, args.k)
        self.queries = args.queries
        if args.stage != 'prepare':
            if self.raw['query'].is_file():
                total, _ = vector_shape(self.raw['query'])
                self.queries = total if not args.queries else args.queries
                if self.queries > total:
                    raise ValueError(f'--queries={self.queries} exceeds the {total} available queries')
            elif not args.dry_run:
                raise FileNotFoundError(f'missing queries: {self.raw["query"]}; download and prepare the dataset first')
            elif not args.queries:
                self.queries = 'ALL'
            if self.raw['groundtruth'].is_file():
                _, width = vector_shape(self.raw['groundtruth'])
                if args.k > width:
                    raise ValueError(f'ground truth contains top-{width} only; k={args.k} requires wider GT')
        self.python = sys.executable
        self.env = environment(args.threads)
        self.counter = 0
        self.preparing = args.stage in ('all', 'prepare')
        self.searching = args.stage in ('all', 'search')
        self.commands = []
        self.active_servers = []

    def write_json(self, path, value):
        if self.args.dry_run:
            print(f'[config] {path}', flush=True)
            return
        path.parent.mkdir(parents=True, exist_ok=True)
        pending = path.with_name(path.name + '.tmp')
        pending.write_text(json.dumps(value, indent=2) + '\n')
        pending.replace(path)

    def run(self, command, label, cwd=None):
        command = list(map(str, command))
        cwd = Path(cwd or self.output)
        self.counter += 1
        log = self.output / 'logs' / f'{self.counter:03d}-{label}.log'
        print('$ ' + shlex.join(command), flush=True)
        self.commands.append({'argv': command, 'cwd': str(cwd), 'log': str(log)})
        if self.args.dry_run:
            return
        cwd.mkdir(parents=True, exist_ok=True)
        log.parent.mkdir(parents=True, exist_ok=True)
        print(f'Log: {log}', flush=True)
        with log.open('w') as stream:
            job = subprocess.Popen(command, cwd=cwd, env=self.env, stdout=stream,
                                   stderr=subprocess.STDOUT, start_new_session=True)
            try:
                while job.poll() is None:
                    if any(server.poll() not in (None, 0) for server in self.active_servers):
                        raise RuntimeError(f'server failed while running {label}; see {self.output / "logs"}')
                    time.sleep(.1)
            finally:
                if job.poll() is None:
                    os.killpg(job.pid, signal.SIGTERM)
                    try:
                        job.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(job.pid, signal.SIGKILL)
                        job.wait()
        if job.returncode:
            raise RuntimeError(f'{label} failed (exit {job.returncode}); see {log}\n' + log.read_text(errors='replace')[-2000:])

    def threads(self, *flags):
        return [item for flag in flags for item in (flag, str(self.args.threads))] if self.args.threads else []

    def remove_derived(self, path):
        print(f'[rebuild] {path}', flush=True)
        if not self.args.dry_run:
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink(missing_ok=True)

    def prepare_step(self, command, label, files):
        if not self.preparing:
            if not self.args.dry_run and not all(path.is_file() for path in files):
                raise FileNotFoundError(f'{label}: prepared files missing; run --stage prepare or all first')
            return
        if self.args.rebuild or not all(path.is_file() for path in files):
            self.run(command, label)
        else:
            print(f'Reusing {label}.', flush=True)

    def finish(self):
        if not self.args.dry_run:
            self.write_json(self.output / 'run.json', {
                'scheme': self.scheme, 'dataset': self.args.dataset, 'path_or_mode': self.variant,
                'k': self.args.k, 'queries': self.queries, 'params': str(self.params),
                'selected_rows': [{'row': row, 'parameters': dict(zip(SCHEMES[self.scheme][2], values))}
                                  for row, values in self.jobs], 'commands': self.commands})
        print(f'Results and logs: {self.output}', flush=True)


def co(ex):
    defaults = json.loads((ROOT / 'Q-COVERT-CO/config.json').read_text())
    selected = defaults['datasets'][ex.dataset]
    selected['source'] = str(ex.raw['base'].parent)
    if 'train' in selected:
        selected['train'] = str(ex.raw['base'])
    selected['files'] = {key: path.name for key, path in ex.raw.items()}
    config = ex.output / 'config.json'
    ex.write_json(config, defaults)
    binary = build_subdir(ex.build, 'Q-COVERT-CO', 'build_ivf_rabitq_index')
    suffix = '_d96' if ex.args.dataset == 'deep1m' else '_d768' if ex.args.dataset == 'msmarco1m' else ''
    source, clusters = ex.raw['base'].parent, selected['clusters']
    manifest = source / f'{ex.dataset}_assignment_{clusters}.txt'
    actual = clusters
    if manifest.exists():
        for line in manifest.read_text().splitlines():
            key, _, value = line.partition(' ')
            if key == 'actual_clusters':
                actual = int(value)
    bits = selected.get('preprocess', {}).get('bits', defaults['defaults']['preprocess']['bits'])
    index = source / f'ivfrabitq{actual}_B{bits}.index'
    if ex.preparing and (ex.args.rebuild or not index.exists()):
        if ex.args.rebuild:
            ex.remove_derived(source / 'double_oram_cache')
        ex.run([ex.python, ROOT / 'Q-COVERT-CO/ivf-rabitq/preprocess.py', 'all', '--config', config,
                '--dataset', ex.dataset, *ex.threads('--cpu-threads', '--faiss-threads')], 'co-preprocess')
        ex.run([binary / ('build_ivf_rabitq_index' + suffix), '--config', config,
                '--dataset', ex.dataset], 'co-index')
    elif not ex.args.dry_run and not index.is_file():
        raise FileNotFoundError(f'CO index missing: {index}; run --stage prepare first')
    if ex.searching:
        name = 'test_ivf_rabitq_double_oram' + suffix if ex.scheme == 'qcovert_co' else 'test_ivf_rabitq_encrypted_baseline'
        command = [binary / name, '--config', config, '--dataset', ex.dataset,
                   '--nprobes', ','.join(str(v[0]) for _, v in ex.jobs),
                   '--k2', ','.join(str(v[1]) for _, v in ex.jobs),
                   '--k', str(ex.args.k), '--nqueries', str(ex.queries)]
        if ex.scheme == 'encrypted_ivf_rabitq':
            command += ['--results-dir', ex.output]
            batch = selected.get('query', {}).get('path_batch_clusters',
                      defaults['defaults']['query'].get('path_batch_clusters', 0))
            if batch == 0:
                # CO uses zero for automatic batching; the encrypted runner requires its resolved value.
                command += ['--path-batch-clusters', str(max(values[0] for _, values in ex.jobs))]
        ex.run(command, 'search')


def ps_config(ex):
    config = copy.deepcopy(json.loads(PROFILES.read_text()))
    profile = config['datasets'][ex.args.dataset.upper()]
    profile['files'] = {key: str(value) for key, value in ex.raw.items()}
    for key in ('precomputed_labels', 'precomputed_centroids'):
        if key in profile['clustering']:
            path = ex.data.joinpath(*Path(profile['clustering'][key]).parts[1:])
            profile['clustering'][key] = str(path)
    clustering = profile['clustering']
    if any(key in clustering and not Path(clustering[key]).is_file()
           for key in ('precomputed_labels', 'precomputed_centroids')):
        # With no external clustering artifacts, use the profile's own k-means defaults.
        clustering.pop('precomputed_labels', None)
        clustering.pop('precomputed_centroids', None)
    path = ex.output / 'config.json'
    ex.write_json(path, config)
    return path


def tee(ex):
    config = ps_config(ex)
    build = build_subdir(ex.build, 'Q-COVERT-PS', 'qcovert_prepare')
    common = ['--dataset', ex.args.dataset.upper(), '--config', config, '--data-root', ex.data]
    if ex.scheme == 'qcovert_ps':
        cache = ex.data / 'prepared/ps/qcovert'
        if ex.args.rebuild:
            ex.remove_derived(cache / ex.args.dataset.upper())
        # PS accepts the parent of data/, while the graph adapters accept data/ itself.
        common[-1] = ex.data.parent
        driver = [ex.python, ROOT / 'Q-COVERT-PS/tools/benchmark.py', *common, '--build-dir', build,
                  '--sgx-build-dir', build, '--mode', ex.args.mode, '--cache-dir', cache,
                  '--k', str(ex.args.k), *ex.threads('--threads')]
        if ex.args.stage == 'prepare':
            ex.run([*driver, '--prepare-only'], 'ps-prepare')
        else:
            if ex.args.stage == 'search' and not ex.args.dry_run and not any((cache / ex.args.dataset.upper()).glob('*/prepared.bin')):
                raise FileNotFoundError('PS prepared files missing; run --stage prepare first')
            if ex.args.stage == 'search':
                driver += ['--require-prepared']
            ex.run([*driver, '--queries', str(ex.queries), '--csv', ex.output / 'summary.csv',
                    '--nprobe', *(str(values[0]) for _, values in ex.jobs),
                    '--k2', *(str(values[1]) for _, values in ex.jobs)], 'search')
        return
    compass = ex.scheme == 'compass_tee'
    directory = 'compass' if compass else 'bison'
    artifact = ex.data / 'prepared/ps' / directory
    prepared = artifact / (ex.args.dataset.upper() + ('.full-c.bin' if compass else '.bin'))
    preparation = [ex.python, ROOT / f'Q-COVERT-PS/{directory}/prepare.py', *common, '--output', prepared]
    if compass:
        preparation += ex.threads('--threads')
        if ex.args.rebuild:
            for path in (prepared.with_suffix('.hnsw.index'), prepared.with_suffix('.pq.index')):
                ex.remove_derived(path)
    else:
        builder = ex.build / 'bison/bin/build_disk_index'
        if not builder.is_file():
            builder = ROOT / 'build/bison/bin/build_disk_index'
        preparation += ['--builder', builder, '--index-prefix', artifact / ex.args.dataset.upper() / 'diskann']
        if ex.args.rebuild:
            preparation += ['--rebuild']
    ex.prepare_step(preparation, directory + '-tee-prepare', [prepared])
    if not ex.searching:
        return
    command = [ex.python, ROOT / f'Q-COVERT-PS/{directory}/benchmark.py', *common,
               '--prepared', prepared, '--build-dir', build, '--mode', ex.args.mode,
               '--queries', str(ex.queries), '--k', str(ex.args.k), '--csv', ex.output / 'summary.csv',
               *ex.threads('--threads')]
    if ex.args.rebuild:
        command += ['--rebuild-oram']
    if compass:
        # Each ef-spec/ef-n group shares an initialization; preserve CSV order across groups.
        groups = []
        for _, values in ex.jobs:
            if groups and groups[-1][0] == values[1:]:
                groups[-1][1].append(values[0])
            else:
                groups.append((values[1:], [values[0]]))
        for (spec, neighbors), efs in groups:
            ex.run([*command, '--ef-spec', str(spec), '--ef-n', str(neighbors),
                    '--ef', *map(str, efs)], 'search')
    else:
        ex.run([*command, '--iterations', *(str(v[0]) for _, v in ex.jobs),
                '--beamwidth', *(str(v[1]) for _, v in ex.jobs),
                '--l-search', *(str(v[2]) for _, v in ex.jobs)], 'search')


def available_ports():
    for _ in range(100):
        held = []
        try:
            first = socket.socket()
            held.append(first)
            first.bind(('127.0.0.1', 0))
            port = first.getsockname()[1]
            if port > 65531:
                continue
            for offset in (1, 2, 3):
                sock = socket.socket()
                held.append(sock)
                sock.bind(('127.0.0.1', port + offset))
            return port
        except OSError:
            pass
        finally:
            for sock in held:
                sock.close()
    raise RuntimeError('could not allocate four consecutive local ports')


def listening(port):
    # A TCP readiness probe would consume the native server's only accept().
    for name in ('/proc/net/tcp', '/proc/net/tcp6'):
        path = Path(name)
        if path.exists():
            for line in path.read_text().splitlines()[1:]:
                fields = line.split()
                if fields[3] == '0A' and int(fields[1].split(':')[1], 16) == port:
                    return True
    return False


@contextmanager
def servers(ex, commands, ports):
    if ex.args.dry_run:
        for command in commands:
            print('[server] ' + shlex.join(map(str, command)), flush=True)
        yield
        return
    jobs, streams = [], []
    try:
        for index, command in enumerate(commands):
            log = ex.output / 'logs' / f'server-{ex.counter}-{index}.log'
            log.parent.mkdir(parents=True, exist_ok=True)
            stream = log.open('w')
            streams.append(stream)
            print(f'[server] {shlex.join(map(str, command))}\nLog: {log}', flush=True)
            ex.commands.append({'argv': list(map(str, command)), 'cwd': str(ex.output),
                                'log': str(log), 'role': 'server'})
            jobs.append(subprocess.Popen(list(map(str, command)), cwd=ex.output, env=ex.env,
                                         stdout=stream, stderr=subprocess.STDOUT, start_new_session=True))
        deadline = time.monotonic() + 120
        while not all(listening(port) for port in ports):
            if any(job.poll() is not None for job in jobs):
                raise RuntimeError(f'server exited before listening; see {ex.output / "logs"}')
            if time.monotonic() >= deadline:
                raise RuntimeError('server startup timed out after 120 seconds')
            time.sleep(.1)
        ex.active_servers = jobs
        yield
    finally:
        ex.active_servers = []
        for job in jobs:
            # The existing Python server driver owns a native child in the same process group.
            try:
                os.killpg(job.pid, signal.SIGTERM)
            except ProcessLookupError:
                continue
        for job in jobs:
            try:
                job.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(job.pid, signal.SIGKILL)
                job.wait()
        for stream in streams:
            stream.close()


def compass_original(ex):
    configuration = copy.deepcopy(json.loads((ROOT / 'Compass/tests/config/config_ring.json').read_text()))
    profile = configuration[ex.dataset]
    for key, value in list(profile.items()):
        if key.endswith('_path'):
            path = Path(value)
            if not path.is_absolute() and path.parts[0] == 'data':
                profile[key] = str(ex.data.joinpath(*path.parts[1:]))
    for key, role in [('base_path', 'base'), ('query_path', 'query'), ('gt_path', 'groundtruth')]:
        profile[key] = str(ex.raw[role])
    if ex.raw['base'].is_file():
        count, _ = vector_shape(ex.raw['base'])
        if count != profile['base_size']:
            profile['base_size'] = count
            profile['num_levels'] = max(2, math.ceil(math.log2(2 * count / profile['real_bucket_size'] + 1)))
            profile['oram_cached_levels'] = min(profile['oram_cached_levels'], profile['num_levels'] - 1)
    # Keep immutable initialization files isolated by path; native servers load them into RAM.
    state = ex.data / 'prepared/compass/quickstart' / ex.args.dataset / ex.variant
    for key, value in list(profile.items()):
        if key.endswith('_path') and key not in ('index_path', 'pq_path', 'base_path', 'query_path', 'gt_path'):
            profile[key] = str(state / Path(value).name)
    config = ex.output / 'config.json'
    ex.write_json(config, configuration)
    binary = build_subdir(ex.build, 'Compass', 'compass_init')
    for name, key, label in [('build_bigann_hnsw.py', 'index_path', 'hnsw'), ('build_bigann_pq.py', 'pq_path', 'pq')]:
        command = [ex.python, ROOT / 'Compass/script' / name, ex.dataset, '--config', config,
                   '--base', ex.raw['base'], '--output', profile[key], *ex.threads('--threads')]
        if ex.args.rebuild:
            command += ['--force']
        ex.prepare_step(command, 'compass-' + label, [Path(profile[key])])
    snapshot_keys = ['buckets_path', 'graph_cache_path', 'metadata_path', 'pos_map_path', 'block_map_path']
    if profile.get('integrity'):
        snapshot_keys.append('hash_path')
    if ex.preparing and not ex.args.dry_run:
        state.mkdir(parents=True, exist_ok=True)
    if ex.args.rebuild:
        for name in ('pq.initial.state', 'pm.initial.state'):
            ex.remove_derived(state / name)
    ex.prepare_step([binary / 'compass_init', f'd={ex.dataset}', f'c={config}'],
                    'compass-initialize', [Path(profile[key]) for key in snapshot_keys])
    if not ex.searching:
        return
    low = ex.variant == 'low-storage'
    pm = int(low and ex.args.dataset in ('sift10m', 'sift100m'))
    program = binary / ('test_compass_accuracy' if ex.variant == 'reference' else 'test_compass_ring')
    for row, (ef, spec, neighbors) in ex.jobs:
        port = 8000 if ex.args.dry_run else available_ports()
        common = [program, f'd={ex.dataset}', f'c={config}', f'p={port}', f'ef_search={ef}',
                  f'efspec={spec}', f'efn={neighbors}', f'topk={ex.args.k}']
        if ex.variant != 'reference':
            common += [f'pq_oram={int(low)}', f'pm_oram={pm}', f'server_metadata={int(low)}',
                       f'pq_oram_state_path={state / "pq.initial.state"}',
                       f'pm_oram_state_path={state / "pm.initial.state"}']
        # Preserve each original implementation's own network defaults.
        with servers(ex, [[*common, 'r=1']], [port]):
            if ex.variant == 'reference':
                ex.run([*common, 'r=2', f'n={ex.queries}',
                        f'f_accuracy={ex.output / f"row-{row}.ivecs"}',
                        f'f_latency={ex.output / f"row-{row}.latency.fvecs"}'], f'row-{row}')
                if not ex.args.dry_run:
                    reference_summary(ex, row, ef, spec, neighbors)
            else:
                ex.run([*common, 'r=2', f'n={ex.queries}', f'f_csv={ex.output / "summary.csv"}'], f'row-{row}')


def reference_summary(ex, row, ef, spec, neighbors):
    """The original access-leaking reference emits IDs/latencies instead of summary CSV."""
    import numpy as np
    sys.path.insert(0, str(ROOT))
    from data.vector_io import read_groundtruth_bin, read_vecs
    ids = read_vecs(ex.output / f'row-{row}.ivecs', 'int')
    truth = (read_groundtruth_bin(ex.raw['groundtruth']) if ex.raw['groundtruth'].suffix == '.bin'
             else read_vecs(ex.raw['groundtruth'], 'int'))
    hits = sum(len(set(map(int, actual)) & set(map(int, expected[:ex.args.k])))
               for actual, expected in zip(ids, truth))
    latency = np.asarray(read_vecs(ex.output / f'row-{row}.latency.fvecs', 'float')).ravel()
    fields = ['dataset', 'path', 'csv_row', 'ef', 'ef_spec', 'ef_n', 'k', 'queries',
              'recall_at_k', 'perceived_ms', 'total_ms']
    path = ex.output / 'summary.csv'
    header = not path.exists()
    with path.open('a', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        if header:
            writer.writeheader()
        writer.writerow(dict(zip(fields, [ex.args.dataset, ex.variant, row, ef, spec, neighbors,
                         ex.args.k, len(ids), hits / (len(ids) * ex.args.k),
                         float(latency[:len(ids)].mean() * 1000), float(latency[len(ids):].mean() * 1000)])))


def clone_file(ex, source, target):
    print(f'[state] {source} -> {target}', flush=True)
    if ex.args.dry_run:
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    # Reflinks avoid duplicating large bucket stores when the filesystem supports them.
    subprocess.run(['cp', '--reflink=auto', '--', str(source), str(target)], check=True)


def bison_original(ex):
    if ex.args.dataset == 'sift100m':
        raise ValueError('original BiSON has no SIFT100M profile; choose a supported dataset')
    binary = build_subdir(ex.build, 'bison/bin', 'build_disk_index')
    artifact = ex.data / 'prepared/bison/quickstart' / ex.variant
    folder = artifact / ex.args.dataset
    config = folder / 'config.json'
    driver = [ex.python, ROOT / 'bison/scripts/benchmark.py', ex.args.dataset]
    options = ['--config', config, '--data-root', artifact, '--build-dir', binary]
    if not config.is_file() or ex.args.rebuild:
        template = ROOT / 'bison/config' / f'bench_{"low_" if ex.variant == "low-storage" else ""}{ex.args.dataset}.json'
        ex.write_json(config, json.loads(template.read_text()))
    _, dtype, _, _ = bison_spec(ex.args.dataset)
    base = folder / ('base.u8bin' if dtype == 'uint8' else 'base.fbin')
    query = folder / ('query.u8bin' if dtype == 'uint8' else 'query.fbin')
    n = vector_shape(ex.raw['base'])[0] if ex.raw['base'].is_file() else None
    prepare = [*driver, 'prepare', *options, '--source', ex.raw['base'].parent,
               '--base', ex.raw['base'], '--query', ex.raw['query'], '--groundtruth', ex.raw['groundtruth']]
    if n is not None:
        prepare += ['--base-count', str(n)]
    ex.prepare_step(prepare, 'bison-convert', [base, query, folder / 'gt10.bin'])
    ex.prepare_step([*driver, 'build', *options], 'bison-index', [folder / 'index/disk_index_disk.index'])
    if ex.variant != 'plain':
        initial_files = [folder / 'oram/buckets.bin', folder / 'oram/metadata.bin',
                         folder / 'oram/position_map.bin', folder / 'oram/block_mapping.bin']
        if config.is_file() and ex.variant == 'low-storage':
            entry = json.loads(config.read_text())[ex.args.dataset]
            for prefix in ('pq_oram_', 'posmap_oram_'):
                if prefix == 'posmap_oram_' and not entry.get('recursive_posmap'):
                    continue
                initial_files.extend(Path(entry[prefix + key]) for key in
                                     ('buckets_path', 'metadata_path', 'pos_map_path'))
        ex.prepare_step([*driver, 'oram-init', *options], 'bison-initialize',
                        initial_files)
    if not ex.searching:
        return
    # L is scalar in the original driver. Batch only adjacent rows with identical L.
    groups = []
    for row, values in ex.jobs:
        if groups and groups[-1][0] == values[2]:
            groups[-1][1].append((row, values))
        else:
            groups.append((values[2], [(row, values)]))
    for l_search, jobs in groups:
        search = ['--queries', str(ex.queries), '--k', str(ex.args.k), '--l', str(l_search),
                  '--t', ','.join(str(v[0]) for _, v in jobs), '--w', ','.join(str(v[1]) for _, v in jobs),
                  '--pairing', 'zip', '--csv', ex.output / 'summary.csv', *ex.threads('--threads')]
        if ex.variant == 'plain':
            ex.run([*driver, 'plain', *options, *search], f'rows-{jobs[0][0]}-{jobs[-1][0]}')
            continue
        run_config = ex.output / f'state-{jobs[0][0]}/config.json'
        try:
            if config.is_file():
                configuration = json.loads(config.read_text())
                entry = configuration[ex.args.dataset]
                for key, value in list(entry.items()):
                    if key.endswith('_path') and 'buckets' in key:
                        if key.startswith('posmap_oram_') and not entry.get('recursive_posmap'):
                            continue
                        target = run_config.parent / Path(value).name
                        clone_file(ex, Path(value), target)
                        entry[key] = str(target)
                ex.write_json(run_config, configuration)
                recursive = entry.get('recursive_posmap', False)
            else:
                recursive = ex.args.dataset == 'sift10m'
                ex.write_json(run_config, {'source': str(config), 'clone_bucket_stores': True})
            run_options = ['--config', run_config, '--data-root', artifact, '--build-dir', binary]
            port = 8000 if ex.args.dry_run else available_ports()
            roles = [('graph', 0)]
            if ex.variant == 'low-storage':
                roles.append(('pq', 2))
                if recursive:
                    roles.append(('posmap', 3))
            commands = [[*driver, 'server', *run_options, '--role', role, '--port', str(port + offset)]
                        for role, offset in roles]
            with servers(ex, commands, [port + offset for _, offset in roles]):
                ex.run([*driver, 'client', *run_options, '--port', str(port), *search],
                        f'rows-{jobs[0][0]}-{jobs[-1][0]}')
        finally:
            if not ex.args.dry_run and run_config.parent.is_dir():
                # Logs/config remain reviewable; mutable bucket clones are scratch space.
                for path in run_config.parent.glob('*buckets.bin'):
                    path.unlink()


def bison_spec(dataset):
    return {'sift': (128, 'float', 'l2', 1_000_000), 'deep1m': (96, 'float', 'l2', 1_000_000),
            'sift10m': (128, 'uint8', 'l2', 10_000_000), 'msmarco1m': (768, 'float', 'l2', 1_000_000)}[dataset]


def main(scheme, argv=None):
    cli = parser(scheme)
    args = cli.parse_args(argv)
    if not 1 <= args.k <= 100 or args.queries < 0:
        cli.error('--k must be in 1..100 and --queries must be nonnegative')
    if args.rebuild and args.stage == 'search':
        cli.error('--rebuild requires --stage prepare or all')
    try:
        ex = Experiment(scheme, args)
        if not args.dry_run:
            ex.output.mkdir(parents=True, exist_ok=True)
        print(f'{scheme}: dataset={args.dataset}, path/mode={ex.variant}, k={args.k}, queries={ex.queries}', flush=True)
        if ex.jobs:
            print(f'CSV: {ex.params}; selected rows: ' + ','.join(str(row) for row, _ in ex.jobs), flush=True)
        # CO and its encrypted baseline share index files and therefore share a lock.
        family = 'co' if scheme in ('qcovert_co', 'encrypted_ivf_rabitq') else scheme
        lock_path = ex.data / 'prepared/.experiment-locks' / f'{family}-{args.dataset}.lock'
        @contextmanager
        def lock():
            if args.dry_run:
                yield
            else:
                lock_path.parent.mkdir(parents=True, exist_ok=True)
                with lock_path.open('a') as stream:
                    fcntl.flock(stream, fcntl.LOCK_EX)
                    yield
        with lock():
            if scheme in ('qcovert_co', 'encrypted_ivf_rabitq'):
                co(ex)
            elif scheme in ('qcovert_ps', 'compass_tee', 'bison_tee'):
                tee(ex)
            elif scheme == 'compass':
                compass_original(ex)
            else:
                bison_original(ex)
            ex.finish()
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        cli.exit(1, f'error: {error}\n')
