#!/usr/bin/env python3
"""Q-COVERT experiment driver: reusable preprocessing, native/SGX runner, append-only CSV."""
import argparse
import csv
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
REQUEST_MAGIC = 0x354E555243565151
REPORT_MAGIC = 0x3150455243565151
PREPARATION_FORMAT_VERSION = 1
ORAM_SNAPSHOT_FORMAT_VERSION = 5
# Keep the established offline-cache identity stable while the JSON contains
# only inputs. These fixed values describe the current converter, not options.
CACHE_QUANTIZATION_ID = {
    'algorithm': 'RaBitQ', 'data_bits': 1, 'query_bits': 4,
    'rotation_seed': 42, 'quantized_entries_per_block': 32,
    'quantized_payload_bytes': 1280, 'full_payload_bytes': 128,
    'payload_sizes_exclude_oram_metadata_and_aead': True,
}
REPORT_FIELDS = ['repetition', 'query_index', 'centroid_distance_ns', 'centroid_topk_ns', 'qu_read_ns', 'query_rotation_ns', 'quantized_distance_ns', 'candidate_topk_ns', 'fu_read_ns', 'fu_leaf_update_ns', 'exact_distance_ns', 'rerank_topk_ns', 'qu_evict_ns', 'fu_evict_ns', 'online_latency_ns', 'total_latency_ns', 'call_wall_ns', 'qu_stash_peak', 'fu_stash_peak', 'qu_stash_total_peak', 'fu_stash_total_peak', 'qu_stash_capacity', 'fu_stash_capacity', 'qu_pending_slots', 'fu_pending_slots', 'fu_pending_real', 'valid_quantized_candidates', 'qu_read_bytes', 'qu_write_bytes', 'fu_read_bytes', 'fu_write_bytes', 'recall_hits', 'cluster_recall_hits', 'candidate_recall_hits', 'prepared_decode_ns', 'setup_total_ns', 'fu_setup_ns', 'qu_setup_ns', 'qu_capacity', 'fu_capacity', 'qu_storage_bytes', 'fu_storage_bytes', 'qu_window', 'fu_window', 'qu_eviction_chunks', 'fu_eviction_chunks', 'qu_evicted_paths', 'fu_evicted_paths', 'qu_padding_blocks', 'fu_padding_blocks', 'qu_reshuffles', 'fu_reshuffles', 'measured_loop_ns']
CSV_COLUMNS = ['n_probe', 'k2', 'k', 'recall_at_k', 'online_ms', 'total_ms', 'qu_oram_access_ms',
               'fu_oram_access_ms', 'candidate_clusters_ms', 'quantized_distance_ms',
               'topk2_ms', 'topk_ms', 'qu_oram_evict_ms', 'fu_oram_evict_ms',
               'qu_stash_peak', 'fu_stash_peak']
APPROX_CSV_COLUMNS = CSV_COLUMNS[:2] + ['alpha'] + CSV_COLUMNS[2:]
PROGRESS_PREFIX = 'QCOVERT_PROGRESS\t'
PROGRESS_LABELS = {
    'offline/base_hash': 'Offline cache/Hash base vectors',
    'offline/labels_hash': 'Offline cache/Hash cluster labels',
    'offline/cache_lock': 'Offline cache/Wait for lock',
    'offline/cache_verify': 'Offline cache/Verify prepared data',
    'offline/validate_vectors': 'Offline preparation/Validate vectors',
    'offline/kmeans': 'Offline clustering/FAISS k-means',
    'offline/assign': 'Offline clustering/Assign initial clusters',
    'offline/split': 'Offline clustering/Split by capacity',
    'offline/centroids': 'Offline clustering/Compute centroids',
    'offline/write_raw': 'Offline preparation/Write intermediate data',
    'offline/quantize_vectors': 'Offline quantization/Encode vectors',
    'offline/finalize': 'Offline preparation/Save cache',
    'calibration/start': 'Approximate quotas/Load calibration data',
    'calibration/queries': 'Approximate quotas/Calibrate exact quantized top-k2',
    'runner/request': 'Benchmark input/Load and pack',
    'runner/start': 'Benchmark runner/Start',
    'runner/decode': 'Benchmark runner/Decode prepared data',
    'setup/validate_vectors': 'ORAM Setup/Validate vectors',
    'setup/prepare_records': 'ORAM Setup/Prepare initial records',
    'setup/fu_bulk_load': 'ORAM Setup/FuORAM BulkLoad',
    'setup/qu_sort': 'ORAM Setup/Sort Qu records',
    'setup/qu_place': 'ORAM Setup/Place Qu records',
    'setup/qu_cluster_shuffle': 'ORAM Setup/Shuffle within clusters',
    'setup/qu_pack': 'ORAM Setup/Pack Qu blocks',
    'setup/qu_shuffle': 'ORAM Setup/Shuffle Qu blocks',
    'setup/qu_bulk_load': 'ORAM Setup/QuORAM BulkLoad',
    'setup/query_workspace': 'ORAM Setup/Allocate query workspace',
    'setup/restore_fu': 'ORAM snapshot/Restore FuORAM',
    'setup/restore_qu': 'ORAM snapshot/Restore QuORAM',
    'setup/snapshot_save': 'ORAM snapshot/Save initial state',
    'setup/snapshot_invalid': 'ORAM snapshot/Invalid, rebuilding',
    'runner/warmup': 'Query warmup',
    'runner/queries': 'Measured queries',
    'runner/report': 'Results/Parse and append CSV',
}


class Progress:
    """Stage-local item progress plus a heartbeat for indivisible FAISS/ORAM operations."""
    def __init__(self, heartbeat_seconds=15):
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.stage = None
        self.completed = self.total = self.percent = 0
        self.stage_started = self.last_output = time.monotonic()
        self.heartbeat_seconds = heartbeat_seconds
        self.line_open = False
        self.line_width = 0
        self.worker = threading.Thread(target=self._heartbeat, daemon=True)
        self.worker.start()

    def _print(self, heartbeat=False):
        label = PROGRESS_LABELS.get(self.stage, self.stage)
        age = int(time.monotonic() - self.stage_started)
        suffix = f', stage elapsed {age}s' if heartbeat else ''
        line = f'[{time.strftime("%H:%M:%S")}] {label}: {self.percent}% ({self.completed}/{self.total}){suffix}'
        if self.line_open:
            sys.stderr.write('\r' + line + ' ' * max(0, (self.line_width - len(line)) * 2))
        else:
            sys.stderr.write(line)
        sys.stderr.flush()
        self.line_open = True
        self.line_width = len(line)
        self.last_output = time.monotonic()

    def finish_line(self):
        with self.lock:
            if self.line_open:
                sys.stderr.write('\n')
                sys.stderr.flush()
                self.line_open = False
                self.line_width = 0

    def update(self, stage, completed, total):
        total = max(1, int(total))
        completed = max(0, min(int(completed), total))
        percent = completed * 100 // total
        with self.lock:
            changed = stage != self.stage
            if changed:
                if self.line_open:
                    sys.stderr.write('\n')
                    self.line_open = False
                    self.line_width = 0
                self.stage = stage
                self.stage_started = time.monotonic()
            self.completed, self.total, self.percent = completed, total, percent
            due = time.monotonic() - self.last_output >= 1
            if changed or (percent != getattr(self, 'shown_percent', -1) and (percent == 100 or due)):
                self._print()
                self.shown_percent = percent

    def _heartbeat(self):
        while not self.stop.wait(self.heartbeat_seconds):
            with self.lock:
                if self.stage is not None and time.monotonic() - self.last_output >= self.heartbeat_seconds:
                    self._print(heartbeat=True)

    def close(self):
        self.stop.set()
        self.worker.join(timeout=1)
        self.finish_line()


def run_streaming(command, progress):
    """Forward child output as it arrives; retain diagnostics for runner errors."""
    lines = []
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, bufsize=1, errors='replace') as process:
        for line in process.stdout:
            if line.startswith(PROGRESS_PREFIX):
                parts = line.rstrip('\n').split('\t')
                if len(parts) == 4:
                    try:
                        progress.update(parts[1], int(parts[2]), int(parts[3]))
                        continue
                    except ValueError:
                        pass
            lines.append(line)
            progress.finish_line()
            print(line, end='', file=sys.stderr, flush=True)
        status = process.wait()
    return subprocess.CompletedProcess(command, status, stdout=''.join(lines), stderr='')


def sha256(path, progress=None):
    h = hashlib.sha256()
    total = Path(path).stat().st_size
    completed = 0
    if progress:
        progress(0, total)
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
            completed += len(block)
            if progress:
                progress(completed, total)
    return h.hexdigest()


def vecs(path, kind):
    import numpy as np
    path = Path(path)
    if path.suffix == '.bin':
        from vector_io import read_groundtruth_bin, read_vector_bin
        return read_vector_bin(path) if kind == 'float' else read_groundtruth_bin(path)
    with path.open('rb') as f:
        header = f.read(4)
    if len(header) != 4:
        raise ValueError(f'empty/truncated vecs file: {path}')
    dim, = struct.unpack('<I', header)
    if not dim or path.stat().st_size % (4 * (dim + 1)):
        raise ValueError(f'invalid vecs shape: {path}')
    values = np.memmap(path, dtype='<u4', mode='r').reshape(-1, dim + 1)
    if not np.all(values[:, 0] == dim):
        raise ValueError(f'inconsistent vecs dimensions: {path}')
    return values[:, 1:].view('<f4' if kind == 'float' else '<i4')


def resolve_path(value, root):
    path = Path(value)
    return path if path.is_absolute() else root / path


def load_base(path):
    """Base vectors from fvecs, or a .bin file ([uint32 n][uint32 dim][vectors])."""
    path = Path(path)
    if path.suffix == '.bin':
        from vector_io import read_vector_bin
        return read_vector_bin(path)
    return vecs(path, 'float')


def preparation_identity(profile, base_hash, dimensions, base_vectors):
    clustering = profile['clustering']
    return {'format_version': PREPARATION_FORMAT_VERSION, 'base_sha256': base_hash,
            'scalar': profile['data']['storage_scalar'], 'dimensions': dimensions,
            'base_vectors': base_vectors, 'initial_clusters': clustering['initial_clusters'],
            'ratio': clustering['size_multiplier_ratio'], 'seed': clustering['seed'],
            'iterations': clustering['iterations'], 'quantization': CACHE_QUANTIZATION_ID}


def preparation_identity_with_sources(profile, root, values, emit=None):
    """Cache identity: base content, clustering inputs and any precomputed assignment."""
    emit = emit or (lambda _stage, _done, _total: None)
    source = resolve_path(profile['files']['base'], root)
    identity = preparation_identity(
        profile, sha256(source, lambda done, total: emit('offline/base_hash', done, total)),
        values.shape[1], len(values))
    precomputed = profile['clustering'].get('precomputed_labels')
    if precomputed is not None:
        identity['precomputed_labels_sha256'] = sha256(
            resolve_path(precomputed, root), lambda done, total: emit('offline/labels_hash', done, total))
    return identity


def matching_prepared_manifests(cache, identity):
    """Prefer the stable key, then reuse compatible caches made by older drivers."""
    token = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:20]
    candidates = []
    for manifest in cache.glob('*/manifest.json'):
        try:
            meta = json.loads(manifest.read_text())
            old = meta['identity']
            if not isinstance(old, dict):
                continue
            semantic = {key: old.get(key, PREPARATION_FORMAT_VERSION) if key == 'format_version'
                        else old[key] for key in identity}
            if semantic == identity:
                candidates.append((manifest, meta))
        except (OSError, ValueError, KeyError, TypeError):
            continue
    return sorted(candidates, key=lambda item: (item[0].parent.name != token,
                                               -item[0].stat().st_mtime_ns, str(item[0])))


def prepare(profile, root, cache, converter, progress=None, require_existing=False):
    import numpy as np
    import faiss
    from clustering import cluster_vectors, split_clusters
    from vector_io import read_labels_vecs, read_vecs as read_vecs_file
    source = resolve_path(profile['files']['base'], root)
    emit = progress.update if progress else lambda _stage, _completed, _total: None
    clustering = profile['clustering']
    values = load_base(source)
    # The C++ runner instantiates D=96/D=128/D=768 and decode_dataset() accepts at most 100,000,000 entries.
    if values.shape[1] not in (96, 128, 768) or not 0 < len(values) <= 100_000_000:
        raise ValueError('runner requires D in {96, 128, 768} and 1..100,000,000 base vectors')
    initial = clustering['initial_clusters']
    num, den = clustering['size_multiplier_ratio']
    if not 0 < initial <= len(values) or num <= 0 or den <= 0:
        raise ValueError('invalid clustering count or size multiplier')
    identity = preparation_identity_with_sources(profile, root, values, emit)
    token = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:20]
    folder = cache / token
    folder.mkdir(parents=True, exist_ok=True)
    prepared = folder / 'prepared.bin'
    manifest = folder / 'manifest.json'
    # Serialize preparation of the same cache key across benchmark processes.
    emit('offline/cache_lock', 0, 1)
    with (folder / 'prepare.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        emit('offline/cache_lock', 1, 1)
        for existing_manifest, meta in matching_prepared_manifests(cache, identity):
            existing = existing_manifest.parent / 'prepared.bin'
            if existing.is_file() and meta.get('prepared_sha256') == sha256(
                    existing, lambda done, total: emit('offline/cache_verify', done, total)):
                return existing, meta, True
        if require_existing:
            raise ValueError('no compatible prepared cache found; run --stage prepare with the experiment script first')
        scalar = profile['data']['storage_scalar']
        emit('offline/validate_vectors', 0, len(values))
        if scalar == 'uint8' and values.dtype == np.uint8:
            # Native uint8 input (e.g. .bin) is already exact; no conversion check needed.
            vectors = values
            emit('offline/validate_vectors', len(values), len(values))
        elif scalar == 'uint8':
            # Check in bounded chunks before conversion, preserving IDs and exact values.
            for start in range(0, len(values), 8192):
                part = values[start:start + 8192]
                if not (np.isfinite(part).all() and ((part >= 0) & (part <= 255)).all() and (part == np.floor(part)).all()):
                    raise ValueError('base vectors are not losslessly representable as uint8')
                emit('offline/validate_vectors', min(start + len(part), len(values)), len(values))
            vectors = np.asarray(values, dtype='u1')
        elif scalar == 'float32':
            vectors = np.asarray(values, dtype='<f4')
            emit('offline/validate_vectors', len(values), len(values))
        else:
            raise ValueError('storage_scalar must be uint8 or float32')
        n = len(vectors)
        tau = ((n * num + initial * den * 32 - 1) // (initial * den * 32)) * 32
        start = time.perf_counter_ns()
        precomputed = clustering.get('precomputed_labels')
        if precomputed is not None:
            # External unbounded assignment: skip k-means, only split oversized clusters.
            emit('offline/split', 0, 1)
            labels = read_labels_vecs(resolve_path(precomputed, root))
            centroids = clustering.get('precomputed_centroids')
            centroid_count = (read_vecs_file(resolve_path(centroids, root), 'float').shape[0]
                              if centroids else None)
            clusters = split_clusters(vectors, labels, tau, centroid_count)
            emit('offline/split', 1, 1)
        else:
            emit('offline/kmeans', 0, 1)
            clusters = cluster_vectors(vectors, tau, initial, clustering['iterations'], clustering['seed'])
            emit('offline/kmeans', 1, 1)
        clustering_ns = time.perf_counter_ns() - start
        count = len(clusters.centroids)
        raw = folder / 'clusters.raw'
        emit('offline/write_raw', 0, 1)
        with raw.open('wb') as out:
            out.write(struct.pack('<7Q', 0x31534C4356435151, values.shape[1], 2 if scalar == 'uint8' else 1,
                                  n, count, tau, count))
            out.write(np.asarray(clusters.centroids, dtype='<f4').tobytes())
            out.write(np.asarray(clusters.labels, dtype='<u8').tobytes())
            out.write(vectors.tobytes(order='C'))
        emit('offline/write_raw', 1, 1)
        staged = folder / 'prepared.pending'
        start = time.perf_counter_ns()
        emit('offline/quantize_vectors', 0, n)
        result = subprocess.run([str(converter), str(raw), str(staged)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f'offline quantizer failed (exit={result.returncode}): '
                               f'{(result.stdout + result.stderr)[-1500:]}')
        quantization_ns = time.perf_counter_ns() - start
        emit('offline/quantize_vectors', n, n)
        emit('offline/finalize', 0, 1)
        staged.replace(prepared)
        sizes = np.bincount(clusters.labels, minlength=count)
        meta = {'identity': identity, 'prepared_sha256': sha256(prepared), 'N': n, 'tau': tau,
                'provenance': {'converter_sha256': sha256(converter),
                               'clustering_source_sha256': sha256(ROOT / 'tools/clustering.py'),
                               'numpy_version': np.__version__, 'faiss_version': faiss.__version__,
                               'faiss_threads': faiss.omp_get_max_threads()},
                'actual_cluster_count': count, 'qu_capacity': count * (tau // 32),
                'clustering_ns': clustering_ns, 'quantization_ns': quantization_ns,
                'cluster_size_min': int(sizes.min()), 'cluster_size_mean': float(sizes.mean()),
                'cluster_size_p95': float(np.percentile(sizes, 95)), 'cluster_size_max': int(sizes.max()),
                'cluster_padding_fraction': 1 - n / (count * tau), 'prepared_file_bytes': prepared.stat().st_size}
        temp = folder / 'manifest.pending'
        temp.write_text(json.dumps(meta, indent=2) + '\n')
        temp.replace(manifest)
        emit('offline/finalize', 1, 1)
        return prepared, meta, False


def selection_quotas(profile, tau):
    query = profile['query']
    selection = query.get('candidate_selection', {'mode': 'exact', 'rank_fractions': []})
    mode = selection.get('mode')
    fractions = selection.get('rank_fractions')
    if mode == 'exact':
        return []
    if mode != 'approx' or not isinstance(fractions, list) or len(fractions) != query['nprobe']:
        raise ValueError('approx selection requires one rank_fraction per probed cluster')
    quotas = []
    for fraction in fractions:
        if isinstance(fraction, bool) or not isinstance(fraction, (int, float)) or not math.isfinite(fraction) or fraction < 0:
            raise ValueError('rank_fractions must be finite nonnegative numbers')
        target = fraction * query['k2']
        if not math.isfinite(target) or target > tau + 1:
            raise ValueError('rank_fraction quota exceeds the padded cluster capacity')
        nearest = round(target)
        quota = nearest if abs(target - nearest) < 1e-9 else math.ceil(target)
        if quota > tau:
            raise ValueError('rank_fraction quota exceeds the padded cluster capacity')
        quotas.append(quota)
    if sum(quotas) < query['k2']:
        raise ValueError('approx selection needs at least k2 partition representatives')
    return quotas


def uniform_quotas(alpha, nprobe, k2, tau):
    """Every probed cluster gets the same quota ceil(alpha*k2/nprobe)."""
    quota = math.ceil(alpha * k2 / nprobe)
    if quota > tau:
        raise ValueError('uniform quota exceeds the padded cluster capacity')
    return [quota] * nprobe


def calibrate_quotas(args, profile, prepared, tau, progress):
    """Run the plaintext calibrator with transient parameters and retain exact integer quotas."""
    nprobe, k2 = profile['query']['nprobe'], profile['query']['k2']
    alpha = args.alpha if args.alpha is not None else 2.0
    budget = math.ceil(alpha * k2)
    if budget > nprobe * tau:
        raise ValueError('alpha*k2 exceeds the available partition capacity')
    available = vecs(resolve_path(profile['files']['query'], args.data_root.resolve()), 'float')
    count = args.calibration_queries if args.calibration_queries is not None else min(100, len(available))
    if count > len(available):
        raise ValueError('requested calibration queries exceed dataset')
    with tempfile.TemporaryDirectory(prefix='qcovert-calibration-') as temp:
        output = Path(temp) / 'quotas.json'
        progress.update('calibration/start', 0, 1)
        result = run_streaming(
            [sys.executable, str(ROOT / 'tools/calibrate_topk.py'),
             '--config', str(args.config.resolve()), '--dataset', args.dataset,
             '--data-root', str(args.data_root.resolve()), '--prepared', str(prepared),
             '--nprobe', str(nprobe), '--k2', str(k2), '--alpha', str(alpha),
             '--queries', str(count), '--output-json', str(output), '--progress-protocol'], progress)
        if result.returncode:
            raise RuntimeError(f'candidate calibration failed (exit={result.returncode}): {result.stdout[-1500:]}')
        quotas = json.loads(output.read_text())['quotas']
    if (not isinstance(quotas, list) or len(quotas) != nprobe or
            any(type(q) is not int or not 0 <= q <= tau for q in quotas) or sum(quotas) != budget):
        raise ValueError('calibrator returned invalid partition quotas')
    progress.finish_line()
    print(f'Temporary approximate quotas: nprobe={nprobe}, k2={k2}, alpha={sum(quotas) / k2:.6g}, '
          f'calibration_queries={count}, quotas={json.dumps(quotas)}', file=sys.stderr, flush=True)
    return quotas


def request_bytes(profile, root, prepared, tau, count, warmup, repeats, threads, image_id, force_rebuild,
                  jobs):
    import numpy as np
    queries = vecs(resolve_path(profile['files']['query'], root), 'float')
    truth = vecs(resolve_path(profile['files']['groundtruth'], root), 'int')
    dim = queries.shape[1]
    k = profile['query']['k']
    if dim not in (96, 128, 768) or len(truth) != len(queries) or truth.shape[1] < k or not 0 < count <= len(queries):
        raise ValueError('query/groundtruth shape or requested count invalid')
    q = queries[:count]
    if not np.isfinite(q).all():
        raise ValueError('nonfinite query')
    scalar = profile['data']['storage_scalar']
    if scalar == 'uint8' and not (((q >= 0) & (q <= 255)).all() and (q == np.floor(q)).all()):
        raise ValueError('queries are not losslessly representable as uint8')
    q = np.asarray(q, dtype='u1' if scalar == 'uint8' else '<f4')
    gt = np.asarray(truth[:count, :k], dtype='<u8')
    p = profile['oram']
    qu, fu = p['qu'], p['fu']
    for layer in (qu, fu):
        if type(layer['R']) is not int or not 0 <= layer['R'] < 32:
            raise ValueError('benchmark requires 0<=R<32 per ORAM layer')
    if not jobs:
        raise ValueError('at least one (nprobe, k2) job is required')
    for nprobe, k2, quotas in jobs:
        if k2 < k or k2 > nprobe * tau:
            raise ValueError('k2 must be between k and the quantized candidate capacity')
        if quotas and len(quotas) != nprobe:
            raise ValueError('one partition quota per probed cluster')
    data = prepared.read_bytes()
    if len(data) < 16 or struct.unpack_from('<Q', data, 8)[0] != dim:
        raise ValueError('prepared dataset dimension differs from the query file')
    header = [REQUEST_MAGIC, 2 if scalar == 'uint8' else 1, dim, k, threads, p['Z'], p['S'], p['A'],
              qu['E'], qu['R'], fu['E'], fu['R'],
              p['stash_blocks'], warmup, count, repeats, len(jobs), len(data), int(force_rebuild)]
    if len(image_id) != 32:
        raise ValueError('ORAM snapshot identity must be 32 bytes')
    out = bytearray(struct.pack('<19Q', *header))
    out.extend(image_id)
    for nprobe, k2, quotas in jobs:
        out.extend(struct.pack('<3Q', nprobe, k2, len(quotas)))
        if quotas:
            out.extend(struct.pack(f'<{len(quotas)}Q', *quotas))
    out.extend(data)
    for query, ids in zip(q, gt):
        out.extend(query.tobytes())
        out.extend(ids.tobytes())
    return out


def finish_report_rows(names, rows, k):
    for row in rows:
        row['recall_at_k'] = row['recall_hits'] / k
        row['cluster_recall_at_k'] = row['cluster_recall_hits'] / k
        row['candidate_recall_at_k'] = row['candidate_recall_hits'] / k
        row['writeback_latency_ns'] = row['qu_evict_ns'] + row['fu_evict_ns']
        phases = sum(row[n] for n in names if n in ('centroid_distance_ns', 'centroid_topk_ns', 'qu_read_ns',
            'query_rotation_ns', 'quantized_distance_ns', 'candidate_topk_ns', 'fu_read_ns', 'fu_leaf_update_ns',
            'exact_distance_ns', 'rerank_topk_ns', 'qu_evict_ns', 'fu_evict_ns'))
        if phases > row['total_latency_ns'] or row['recall_hits'] > k:
            raise ValueError('invalid timing/recall report')
        row['unattributed_ns'] = row['total_latency_ns'] - phases
    return rows


def parse_report_block(data, pos=0, k=10):
    """One report block at pos: (rows, next_pos), or None while data is incomplete."""
    if len(data) - pos < 24:
        return None
    if struct.unpack_from('<Q', data, pos)[0] != REPORT_MAGIC:
        raise ValueError('wrong report version')
    width, count = struct.unpack_from('<QQ', data, pos + 8)
    if not 0 < width < 256 or not 0 < count <= 1000000:
        raise ValueError('report dimension limit')
    cursor = pos + 24
    names = []
    for _ in range(width):
        if len(data) - cursor < 8:
            return None
        length = struct.unpack_from('<Q', data, cursor)[0]
        cursor += 8
        if length > 128:
            raise ValueError('invalid report field name')
        if len(data) - cursor < length:
            return None
        names.append(data[cursor:cursor + length].decode('ascii'))
        cursor += length
    if names != REPORT_FIELDS:
        raise ValueError('report schema/length mismatch')
    if len(data) - cursor < count * width * 8:
        return None
    rows = []
    for _ in range(count):
        rows.append(dict(zip(names, struct.unpack_from(f'<{width}Q', data, cursor))))
        cursor += width * 8
    return finish_report_rows(names, rows, k), cursor


def parse_report(data, k=10):
    """One report block per (nprobe, k2) job; returns a list of row lists."""
    blocks = []
    pos = 0
    while pos < len(data):
        parsed = parse_report_block(data, pos, k)
        if parsed is None:
            raise ValueError('truncated benchmark report')
        rows, pos = parsed
        blocks.append(rows)
    if not blocks:
        raise ValueError('empty benchmark report')
    return blocks


def csv_columns(selection_mode):
    if selection_mode == 'exact':
        return CSV_COLUMNS
    if selection_mode == 'approx':
        return APPROX_CSV_COLUMNS
    raise ValueError('unknown candidate selection mode')


def append_csv(path, rows, selection_mode='exact'):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # Fixed schema, exactly one header, process-safe append. Never truncate an existing result file.
    columns = csv_columns(selection_mode)
    with path.open('a+', newline='') as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        stream.seek(0)
        first = next(csv.reader(stream), None)
        if first is not None and first != columns:
            raise ValueError(f'CSV schema mismatch: {path}; existing file was not changed')
        stream.seek(0, os.SEEK_END)
        end = stream.tell()
        if end and os.pread(stream.fileno(), 1, end - 1) not in (b'\n', b'\r'):
            # Historical CSVs may have a valid final row without a line break.
            # Start the appended row on a new line instead of concatenating it.
            stream.write('\n')
        writer = csv.DictWriter(stream, fieldnames=columns, extrasaction='raise')
        if first is None:
            writer.writeheader()
        for row in rows:
            writer.writerow(row)
        stream.flush()
        os.fsync(stream.fileno())


def check_csv_schema(path, selection_mode='exact'):
    path = Path(path)
    if not path.exists():
        return
    with path.open(newline='') as stream:
        fcntl.flock(stream, fcntl.LOCK_SH)
        first = next(csv.reader(stream), None)
        if first is not None and first != csv_columns(selection_mode):
            raise ValueError(f'CSV schema mismatch: {path}; use a new --csv path for compact summary rows')


def completed_jobs(path):
    """(n_probe, k2, k) rows whose summary already exists in the result CSV."""
    done = set()
    path = Path(path)
    if not path.exists():
        return done
    with path.open(newline='') as stream:
        fcntl.flock(stream, fcntl.LOCK_SH)
        for row in csv.DictReader(stream):
            try:
                done.add((int(row['n_probe']), int(row['k2']), int(row.get('k', 0))))
            except (KeyError, TypeError, ValueError):
                continue
    return done


def dataset_csv_path(path, dataset, mode, selection_tag='exact'):
    """Keep dataset, execution mode and selection mode in the filename."""
    if mode not in ('native', 'sgx-sim', 'sgx-hw'):
        raise ValueError(f'unknown benchmark mode: {mode}')
    path = Path(path)
    safe_dataset = re.sub(r'[^A-Za-z0-9_-]', '_', dataset)
    if not safe_dataset:
        raise ValueError('dataset name cannot form a CSV filename')
    suffix = path.suffix or '.csv'
    stem = path.stem if path.suffix else path.name
    for prior_mode in ('native', 'sgx-sim', 'sgx-hw'):
        ending = re.search(rf'_{re.escape(safe_dataset)}_{prior_mode}(?:_exact|_approx(?:-[0-9a-f]{{10}})?)?$', stem)
        if ending:
            stem = stem[:ending.start()]
            break
    if stem == safe_dataset:
        stem = ''
    elif stem.endswith('_' + safe_dataset):
        stem = stem[:-(len(safe_dataset) + 1)]
    prefix = f'{stem}_' if stem else ''
    if selection_tag not in ('exact', 'approx'):
        raise ValueError('invalid candidate selection filename tag')
    selected = f'_{selection_tag}'
    return path.with_name(f'{prefix}{safe_dataset}_{mode}{selected}{suffix}')


def compact_summary(profile, rows, quotas=None):
    """One mean per measured search, plus each ORAM's maximum stash occupancy."""
    def mean_ms(*fields):
        return statistics.mean(sum(row[name] for name in fields) for row in rows) / 1_000_000
    summary = {'n_probe': profile['query']['nprobe'], 'k2': profile['query']['k2']}
    if quotas:
        summary['alpha'] = sum(quotas) / profile['query']['k2']
    summary['k'] = profile['query']['k']
    summary.update({'recall_at_k': statistics.mean(row['recall_hits'] / profile['query']['k'] for row in rows),
            'online_ms': statistics.mean(
                row['call_wall_ns'] - row['qu_evict_ns'] - row['fu_evict_ns'] for row in rows) / 1_000_000,
            'total_ms': mean_ms('call_wall_ns'),
            'qu_oram_access_ms': mean_ms('qu_read_ns'),
            'fu_oram_access_ms': mean_ms('fu_read_ns'),
            'candidate_clusters_ms': mean_ms('centroid_distance_ns', 'centroid_topk_ns'),
            'quantized_distance_ms': mean_ms('query_rotation_ns', 'quantized_distance_ns'),
            'topk2_ms': mean_ms('candidate_topk_ns'),
            'topk_ms': mean_ms('exact_distance_ns', 'rerank_topk_ns'),
            'qu_oram_evict_ms': mean_ms('qu_evict_ns'),
            'fu_oram_evict_ms': mean_ms('fu_evict_ns'),
            'qu_stash_peak': max(row['qu_stash_peak'] for row in rows),
            'fu_stash_peak': max(row['fu_stash_peak'] for row in rows)})
    return summary


class ReportStreamer:
    """Tail the runner report file and append one CSV summary per finished job."""
    def __init__(self, path, jobs, expected_rows, selection_mode, csv_path, reused, k):
        self.path = Path(path)
        self.jobs = jobs
        self.expected_rows = expected_rows
        self.selection_mode = selection_mode
        self.csv_path = csv_path
        self.reused = reused
        self.k = k
        self.position = 0
        self.appended = 0
        self.error = None

    def drain(self):
        if self.error is not None or self.appended >= len(self.jobs):
            return
        try:
            with self.path.open('rb') as stream:
                stream.seek(self.position)
                data = stream.read()
        except FileNotFoundError:
            return
        cursor = 0
        try:
            while self.appended < len(self.jobs):
                parsed = parse_report_block(data, cursor, self.k)
                if parsed is None:
                    break
                rows, cursor = parsed
                if len(rows) != self.expected_rows:
                    raise ValueError('runner returned wrong query count')
                nprobe, k2, quotas = self.jobs[self.appended]
                summary = compact_summary({'query': {'nprobe': nprobe, 'k2': k2, 'k': self.k}}, rows, quotas)
                append_csv(self.csv_path, [summary], self.selection_mode)
                print(f'Appended one summary for {len(rows)} queries to {self.csv_path}; nprobe={nprobe}, k2={k2}; '
                      f'Recall@{self.k}={summary["recall_at_k"]:.4f}; mean total call={summary["total_ms"]:.3f} ms; '
                      f'cache_reused={self.reused}', flush=True)
                self.appended += 1
        except Exception as error:
            self.error = error
        self.position += cursor

    def tail(self, stop):
        while not stop.is_set():
            self.drain()
            stop.wait(1)
        self.drain()


def sgx_enclave_candidates(build, mode):
    """Signed enclave images in the build tree as (heap_bytes, path), smallest heap first."""
    build = Path(build).resolve()
    # Root builds place PS binaries in a subdirectory but keep one parent cache.
    cache_path = next((directory / 'CMakeCache.txt' for directory in (build, *build.parents)
                       if (directory / 'CMakeCache.txt').is_file()), None)
    if cache_path is None:
        raise ValueError(f'SGX build cache not found for: {build}')
    cache = cache_path.read_text()
    expected_mode = 'SIM' if mode == 'sgx-sim' else 'HW'
    if f'RINGDORAM_SGX_MODE:STRING={expected_mode}' not in cache:
        raise ValueError('requested mode differs from built SGX mode')
    candidates = []
    match = re.search(r'^RINGDORAM_SGX_HEAP_SIZE:STRING=([1-9][0-9]*)([MmGg])$', cache, re.MULTILINE)
    default = build / 'apps/sgx/ringdoram_enclave.signed.so'
    if not default.is_file():
        # SDK helper may emit the signed enclave at the build root; resolve without guessing silently.
        found = list(build.rglob('ringdoram_enclave.signed.so'))
        if len(found) == 1:
            default = found[0]
    if match and default.is_file():
        heap = int(match.group(1)) * (1024 ** (2 if match.group(2).upper() == 'M' else 3))
        candidates.append((heap, default))
    # Heap variants signed from the same enclave: ringdoram_enclave_h<mib>m.signed.so
    for image in build.rglob('ringdoram_enclave_h*m.signed.so'):
        stem = image.name.removeprefix('ringdoram_enclave_h').removesuffix('m.signed.so')
        if stem.isdigit():
            candidates.append((int(stem) * 1024 ** 2, image))
    return sorted(candidates)


def select_sgx_enclave(build, mode, profile):
    """Pick the smallest signed enclave image whose heap covers the dataset profile."""
    required = profile['execution'].get('sgx_heap_bytes')
    if type(required) is not int or required < 128 * 1024 ** 2 or required % (1024 ** 2):
        raise ValueError('execution.sgx_heap_bytes must be an integer multiple of MiB, at least 128 MiB')
    candidates = sgx_enclave_candidates(build, mode)
    if not candidates:
        raise ValueError('signed enclave not found (or heap missing in CMakeCache.txt); '
                         'reconfigure and rebuild the SGX build')
    fits = [candidate for candidate in candidates if candidate[0] >= required]
    if not fits:
        available = ', '.join(f'{heap // 1024 ** 2} MiB' for heap, _ in candidates)
        raise ValueError(f'no signed enclave image covers the dataset heap request '
                         f'{required // 1024 ** 2} MiB (available: {available}); reconfigure with '
                         f'-DRINGDORAM_SGX_HEAP_VARIANTS={required // 1024 ** 2}M and rebuild')
    heap, image = fits[0]
    if len(candidates) > 1:
        print(f'SGX enclave image: {image} (heap {heap // 1024 ** 2} MiB, dataset requirement '
              f'{required // 1024 ** 2} MiB)', file=sys.stderr)
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=ROOT / 'config/dataset_defaults.json')
    parser.add_argument('--dataset', default='SIFT')
    parser.add_argument('--data-root', type=Path, default=ROOT.parent, help='parent of data/ used by JSON paths')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/qcovert-release')
    parser.add_argument('--sgx-build-dir', type=Path, default=ROOT / 'build/qcovert-sgx')
    parser.add_argument('--mode', choices=['native', 'sgx-sim', 'sgx-hw'], default='native')
    parser.add_argument('--candidate-selection', choices=['exact', 'approx'],
                        help='override query.candidate_selection.mode for this run')
    parser.add_argument('--nprobe', type=int, nargs='+',
                        help='override query.nprobe; multiple values pair positionally with --k2')
    parser.add_argument('--k2', type=int, nargs='+',
                        help='override query.k2; multiple values pair positionally with --nprobe')
    parser.add_argument('--k', type=int,
                        help='override query.k (final top-k, must be <= k2); default reads JSON query.k')
    parser.add_argument('--alpha', type=float, help='automatic approximate calibration budget multiplier (default: 2)')
    parser.add_argument('--calibration-queries', type=int,
                        help='plaintext calibration query count (default: up to the first 100 queries)')
    parser.add_argument('--use-config-quotas', action='store_true',
                        help='skip automatic approximate calibration and use JSON rank_fractions')
    parser.add_argument('--uniform-quotas', action='store_true',
                        help='approx: give every probed cluster the same quota ceil(alpha*k2/nprobe), '
                             'skipping calibration and JSON rank_fractions')
    parser.add_argument('--csv', type=Path, default=ROOT / 'results/qcovert.csv',
                        help='result prefix/path; dataset and mode are appended to the filename')
    parser.add_argument('--cache-dir', type=Path, default=ROOT.parent / 'data/prepared/ps/qcovert')
    parser.add_argument('--queries', type=int)
    parser.add_argument('--warmup', type=int)
    parser.add_argument('--repeats', type=int)
    parser.add_argument('--threads', type=int)
    parser.add_argument('--qu-routing-depth', type=int, help='override QuORAM forest depth R')
    parser.add_argument('--qu-evict-batch', type=int, help='override QuORAM paths evicted per subtree E (power of two)')
    parser.add_argument('--fu-routing-depth', type=int, help='override FuORAM forest depth R')
    parser.add_argument('--fu-evict-batch', type=int, help='override FuORAM paths evicted per subtree E (power of two)')
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--require-prepared', action='store_true',
                        help='fail if a compatible prepared cache is unavailable; do not run offline construction')
    parser.add_argument('--rebuild-oram', action='store_true', help='ignore the saved initial ORAM image and replace it')
    parser.add_argument('--resume', action='store_true',
                        help='skip (nprobe, k2) jobs whose summary row already exists in the CSV')
    args = parser.parse_args()
    if not args.config.is_file():
        raise ValueError(f'config file not found: {args.config} (is $QCOVERT_CONFIG set?)')
    cfg = json.loads(args.config.read_text())
    profile = cfg['datasets'][args.dataset]
    for layer in ('qu', 'fu'):
        r = getattr(args, f'{layer}_routing_depth')
        e = getattr(args, f'{layer}_evict_batch')
        if r is not None:
            profile['oram'][layer]['R'] = r
        if e is not None:
            profile['oram'][layer]['E'] = e
    nprobes = args.nprobe if args.nprobe is not None else [profile['query']['nprobe']]
    k2s = args.k2 if args.k2 is not None else [profile['query']['k2']]
    if len(nprobes) != len(k2s):
        raise ValueError('--nprobe and --k2 must be given the same number of values')
    k = args.k if args.k is not None else profile['query'].get('k', 10)
    if type(k) is not int or not 1 <= k <= 256:
        raise ValueError('k must be an integer in [1, 256]')
    profile['query']['k'] = k
    if any(type(v) is not int or v <= 0 for v in nprobes):
        raise ValueError('nprobe must be a positive integer')
    if any(type(v) is not int or v < k for v in k2s):
        raise ValueError('k2 must be an integer >= k')
    profile['query']['nprobe'] = nprobes[0]
    profile['query']['k2'] = k2s[0]
    if args.candidate_selection:
        profile['query'].setdefault('candidate_selection', {})['mode'] = args.candidate_selection
    selection = profile['query'].get('candidate_selection', {'mode': 'exact', 'rank_fractions': []})
    if selection.get('mode') not in ('exact', 'approx'):
        raise ValueError('candidate_selection.mode must be exact or approx')
    automatic = selection['mode'] == 'approx' and not args.use_config_quotas and not args.uniform_quotas
    if args.use_config_quotas and args.uniform_quotas:
        raise ValueError('--uniform-quotas cannot be combined with --use-config-quotas')
    if (args.use_config_quotas or args.uniform_quotas) and selection['mode'] != 'approx':
        raise ValueError('--use-config-quotas/--uniform-quotas require approx selection')
    if args.calibration_queries is not None and not automatic:
        raise ValueError('--calibration-queries require automatic approx selection')
    if args.alpha is not None and not (automatic or args.uniform_quotas):
        raise ValueError('--alpha requires automatic approx selection or --uniform-quotas')
    if args.alpha is not None and (not math.isfinite(args.alpha) or args.alpha < 1 or
                                   not math.isfinite(args.alpha * max(k2s))):
        raise ValueError('alpha must be finite, >= 1, with finite alpha*k2')
    if args.calibration_queries is not None and args.calibration_queries <= 0:
        raise ValueError('calibration-queries must be a positive integer')
    args.csv = dataset_csv_path(args.csv, args.dataset, args.mode, selection['mode'])
    if not args.prepare_only:
        check_csv_schema(args.csv, selection['mode'])
    sgx_enclave = None
    if args.mode != 'native':
        sgx_enclave = select_sgx_enclave(args.sgx_build_dir.resolve(), args.mode, profile)
    defaults = profile['benchmark']
    count = args.queries if args.queries is not None else defaults['queries']
    warmup = args.warmup if args.warmup is not None else defaults['warmup']
    repeats = args.repeats if args.repeats is not None else defaults['repeats']
    threads = args.threads if args.threads is not None else profile['execution']['access_threads']
    if not 0 < count <= 1000000 or not 0 <= warmup <= 1000000 or not 0 < repeats <= 10000 or count * repeats > 1000000 or not 1 <= threads <= 256:
        raise ValueError('invalid query/warmup/repetition/thread count')
    pairs = list(zip(nprobes, k2s))
    if args.resume:
        done = completed_jobs(args.csv)
        skipped = [(n, k2) for n, k2 in pairs if (n, k2, k) in done]
        pairs = [(n, k2) for n, k2 in pairs if (n, k2, k) not in done]
        if skipped:
            print(f'--resume: skipping {len(skipped)} job(s) already recorded in {args.csv}: '
                  + ', '.join(f'(nprobe={n}, k2={k})' for n, k in skipped), file=sys.stderr)
        if not pairs:
            print(f'All requested jobs already recorded in {args.csv}; nothing to run', file=sys.stderr)
            return
    build = args.build_dir.resolve()
    converter = build / 'qcovert_prepare'
    if not converter.is_file():
        raise ValueError(f'build the qcovert_prepare target first: {converter}')
    progress = Progress()
    prepared, meta, reused = prepare(profile, args.data_root.resolve(), args.cache_dir.resolve() / args.dataset,
                                     converter, progress, require_existing=args.require_prepared)
    oram = profile['oram']
    qu_layer, fu_layer = oram.get('qu'), oram.get('fu')
    for layer in (qu_layer, fu_layer):
        if not isinstance(layer, dict) or type(layer.get('R')) is not int or not 0 <= layer['R'] < 32 or \
                type(layer.get('E')) is not int or not layer['E'] or layer['E'] & (layer['E'] - 1):
            raise ValueError('shared_top requires per-layer (qu/fu) 0<=R<32 and power-of-two E')
        if (1 << layer['R']) * layer['E'] > 0xffffffff:
            raise ValueError('ORAM eviction schedule exceeds uint32 capacity')
    if oram['A'] <= 0 or oram['Z'] <= 0 or oram['S'] <= 0 or oram['Z'] + oram['S'] > 255:
        raise ValueError('invalid ORAM geometry')
    if oram['stash_blocks'] < 0:
        raise ValueError('stash_blocks must be nonnegative')
    for capacity, layer in ((meta['qu_capacity'], qu_layer), (meta['N'], fu_layer)):
        paths_per_round = (1 << layer['R']) * layer['E']
        two_n_over_a = max(1, (2 * max(2, capacity) + oram['A'] - 1) // oram['A'])
        height = max(0, (two_n_over_a - 1).bit_length() - 1)
        leaves = 1 << height
        if paths_per_round > leaves:
            raise ValueError('R/E exceed the available leaves in one of the ORAMs')
    if oram['stash_blocks'] == 0:
        z, a = oram['Z'], oram['A']
        gap = z * math.log(2 * z / a) + a / 2 - z - math.log(4)
        if gap <= 0:
            raise ValueError('ORAM parameters do not support automatic stash bound')
    if max(nprobes) > meta['actual_cluster_count']:
        raise ValueError('nprobe exceeds the resolved cluster count')
    if any(k2 > nprobe * meta['tau'] for nprobe, k2 in zip(nprobes, k2s)):
        raise ValueError('k2 exceeds the quantized candidate capacity (nprobe * tau)')
    if args.prepare_only:
        progress.close()
        print(f'Prepared {prepared}; actual clusters={meta["actual_cluster_count"]}, Qu blocks={meta["qu_capacity"]}, cache_reused={reused}')
        return
    mode = args.mode
    if mode == 'native':
        binary = build / 'qcovert_benchmark'
        command = [str(binary)]
    else:
        binary = args.sgx_build_dir.resolve() / 'apps/sgx/ringdoram_sgx_host'
        if not binary.is_file():
            raise ValueError(f'SGX host runner not found: {binary}')
        command = [str(binary), str(sgx_enclave)]
    image_variant = mode
    image_path = args.cache_dir.resolve() / args.dataset / f'initial_oram.{image_variant}.state'
    image_path.parent.mkdir(parents=True, exist_ok=True)
    image_parameters = {'snapshot_format_version': ORAM_SNAPSHOT_FORMAT_VERSION,
                        'prepared_sha256': meta['prepared_sha256'],
                        'scalar': profile['data']['storage_scalar'], 'N': meta['N'],
                        'clusters': meta['actual_cluster_count'], 'tau': meta['tau'],
                        'oram': oram}
    image_id = hashlib.sha256(json.dumps(image_parameters, sort_keys=True).encode()).digest()
    # One runner invocation, one ORAM initialization: the request carries every
    # pending (nprobe, k2) job. The runner streams each finished job's report
    # block to the report file, and the streamer appends its CSV row right away,
    # so an interrupted run keeps every completed result without any extra ORAM
    # snapshot load.
    jobs = []
    for nprobe, k2 in pairs:
        profile['query']['nprobe'] = nprobe
        profile['query']['k2'] = k2
        if automatic:
            quotas = calibrate_quotas(args, profile, prepared, meta['tau'], progress)
        elif args.uniform_quotas:
            alpha = args.alpha if args.alpha is not None else 2.0
            quotas = uniform_quotas(alpha, nprobe, k2, meta['tau'])
            print(f'Uniform approximate quotas: nprobe={nprobe}, k2={k2}, alpha={sum(quotas) / k2:.6g}, '
                  f'quota_per_cluster={quotas[0]}', file=sys.stderr, flush=True)
        else:
            quotas = selection_quotas(profile, meta['tau'])
        jobs.append((nprobe, k2, quotas))
    progress.update('runner/request', 0, 1)
    payload = request_bytes(profile, args.data_root.resolve(), prepared, meta['tau'], count, warmup, repeats,
                            threads, image_id, args.rebuild_oram or not image_path.exists(), jobs)
    progress.update('runner/request', 1, 1)
    with tempfile.TemporaryDirectory(prefix='qcovert-run-') as temp:
        input_path, output_path = Path(temp) / 'input.bin', Path(temp) / 'output.bin'
        input_path.write_bytes(payload)
        progress.update('runner/start', 0, 1)
        streamer = ReportStreamer(output_path, jobs, count * repeats, selection['mode'], args.csv, reused, k)
        stop = threading.Event()
        tail = threading.Thread(target=streamer.tail, args=(stop,), daemon=True)
        tail.start()
        try:
            with (image_path.parent / f'initial_oram.{image_variant}.lock').open('a') as image_lock:
                fcntl.flock(image_lock, fcntl.LOCK_EX)
                process = run_streaming(command + [str(input_path), str(output_path), str(image_path)], progress)
        finally:
            stop.set()
            tail.join()
        if streamer.error is not None:
            raise RuntimeError(f'report streaming failed: {streamer.error}')
        if process.returncode:
            kept = (f'; {streamer.appended} completed job row(s) remain appended in {args.csv}'
                    if streamer.appended else '')
            raise RuntimeError(f'benchmark failed (exit={process.returncode}): {process.stdout[-1500:]}{kept}')
        progress.update('runner/start', 1, 1)
        progress.update('runner/report', 0, 1)
        blocks = parse_report(output_path.read_bytes(), k)
    if len(blocks) != len(jobs) or streamer.appended != len(jobs):
        raise ValueError('runner returned wrong job count')
    image_status = ''
    for line in process.stdout.splitlines():
        if line.startswith('oram_snapshot_status='):
            image_status = line.split('=', 1)[1]
    if image_status not in ('loaded', 'built'):
        raise ValueError('runner did not report a valid ORAM snapshot outcome')
    print(f'Appended {len(jobs)} job summaries to {args.csv}; cache_reused={reused}; oram_snapshot={image_status}')
    progress.update('runner/report', 1, 1)
    progress.close()


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
