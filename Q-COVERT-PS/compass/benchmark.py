#!/usr/bin/env python3
"""Run the same prepared Compass index in Native or SGX and append summary CSV."""
import argparse
import csv
from contextlib import ExitStack
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import time
import numpy as np
from prepare import INVALID
from prepared import prepared_full, prepared
from dataio import ROOT, add_dataset_arguments, resolve_paths, read_vectors, read_groundtruth

FIELDS = ['mode', 'metric', 'ef', 'ef_spec', 'ef_n', 'k', 'rounds', 'batch_size', 'logical_reads',
          'recall_at_k', 'total_ms', 'oram_ms', 'eviction_ms', 'sets_ms', 'pq_ms', 'exact_ms', 'position_ms',
          'setup_ms', 'node_block_bytes', 'index_bytes', 'oram_storage_bytes', 'position_map_bytes', 'peak_host_rss_bytes', 'reference_recall_at_k',
          'reference_ms', 'reference_result_match', 'search_variant', 'upper_steps', 'upper_reads',
          'upper_scan_ms', 'scan_count', 'trusted_routing_bytes', 'upper_node_block_bytes', 'upper_oram_storage_bytes', 'upper_position_map_bytes', 'candidate_capacity']

class RunnerUnavailableError(RuntimeError):
    """The selected build directory does not contain a usable Compass runner."""

def require_runner(command, args, layout):
    runner = Path(command[0])
    target = 'compass_benchmark' if args.mode == 'native' else 'compass_sgx_host'
    configure = ['cmake', '-S', str(ROOT), '-B', str(args.build_dir),
                 '-DCMAKE_BUILD_TYPE=Release', '-DRINGDORAM_BUILD_COMPASS=ON',
                 '-DRINGDORAM_BUILD_EXAMPLES=ON',
                 f'-DCOMPASS_DIM={layout["dimension"]}', f'-DCOMPASS_DEGREE={layout["degree"]}',
                 f'-DCOMPASS_PQ_SUBSPACES={layout["pq_subspaces"]}',
                 f'-DCOMPASS_PQ_CENTROIDS={layout["pq_centroids"]}',
                 f'-DCOMPASS_SCALAR={layout["scalar"]}']
    if args.mode != 'native':
        configure += ['-DRINGDORAM_ENABLE_SGX=ON', f'-DRINGDORAM_SGX_MODE={args.mode.split("-")[1].upper()}']
    build = ['cmake', '--build', str(args.build_dir), '--target', target, '-j2']
    instructions = (f'Build the runner on this machine:\n  {shlex.join(configure)}\n  {shlex.join(build)}\n'
                    'If already built elsewhere, pass --build-dir <that build directory>.')
    if not runner.is_file():
        raise RunnerUnavailableError(f'Compass runner not found: {runner}\n{instructions}')
    if not os.access(runner, os.X_OK):
        raise RunnerUnavailableError(f'Compass runner is not executable: {runner}\n{instructions}')
    if args.mode != 'native' and not Path(command[1]).is_file():
        raise RunnerUnavailableError(f'Signed Compass enclave not found: {command[1]}\n{instructions}')
    return instructions

def snapshot_identity(prepared_digest, layout, args):
    # Query parameters and threads configure only the workspace/epoch window.
    parameters = {'format': 'compass-initial-v3', 'oram_format': 5,
                  'prepared_sha256': prepared_digest, 'layout': layout, 'mode': args.mode,
                  'oram': [args.z, args.s, args.a, args.evict_batch, args.stash]}
    return hashlib.sha256(json.dumps(parameters, sort_keys=True).encode()).digest()

def recall(ids, gt, k):
    return sum(len((set(map(int, row)) - {INVALID}) & set(map(int, truth[:k]))) / k
               for row, truth in zip(ids, gt)) / len(ids)

def report_row(output, ef, args, count, data, header, book, nodes, upper, scan, scan_level, queries, truth, rss):
    metric = header[5]
    if len(output) < 144:
        raise ValueError('truncated Compass report')
    magic, rows, k, setup_ns, rounds, batch, block_bytes, storage_bytes, position_bytes, reported_reads, upper_reads, scan_count, routing_bytes, reported_ef, upper_block_bytes, upper_storage_bytes, upper_map_bytes, candidate_capacity = struct.unpack_from('<18Q', output)
    if magic != int.from_bytes(b'CMPRT005', 'little') or rows != count * args.repeats or k != args.k or reported_ef != ef:
        raise ValueError('invalid Compass report')
    row_bytes = 72 + k * 16
    if len(output) != 144 + rows * row_bytes:
        raise ValueError('report length mismatch')
    times = []
    ids = []
    for i in range(rows):
        offset = 144 + i * row_bytes
        times.append(struct.unpack_from('<9Q', output, offset))
        ids.append([struct.unpack_from('<Qd', output, offset + 72 + j * 16)[0] for j in range(k)])
    expected_upper = args.ef_n if len(upper) else 0
    expected_rounds = (ef + args.ef_spec - 1) // args.ef_spec
    expected_reads = expected_upper + 1 + expected_rounds * args.ef_spec * args.ef_n
    if candidate_capacity != expected_reads - expected_upper:
        raise ValueError('candidate capacity differs from full fetched-node budget')
    if reported_reads != expected_reads or upper_reads != expected_upper or rounds != expected_rounds or batch != args.ef_spec * args.ef_n or scan_count != len(scan):
        raise ValueError('runner search budget differs from requested variant')
    if any(row[7] != expected_reads for row in times):
        raise ValueError('variable ORAM operation count')
    mean = np.mean(np.asarray(times, dtype='f8'), axis=0) / 1e6
    scores = recall(ids, [truth[i % count] for i in range(rows)], k)
    reference_recall = reference_ms = reference_match = ''
    if args.reference:
        from plaintext import reference_scan
        start = time.perf_counter()
        reference_ids = [reference_scan(q, nodes, upper, scan, scan_level, book, metric,
                                        ef, args.ef_spec, args.ef_n, k, args.upper_steps)
                         for q in queries[:count]]
        reference_ms = (time.perf_counter() - start) * 1000 / count
        reference_recall = recall(reference_ids, truth[:count], k)
        reference_match = all(row == reference_ids[i % count] for i, row in enumerate(ids))
        if not reference_match:
            raise ValueError('Compass output differs from plaintext search reference')
    row = dict(zip(FIELDS, [args.mode, 'l2' if metric == 0 else 'ip', ef, args.ef_spec, args.ef_n, k,
                           rounds, batch, expected_reads, scores, *mean[:7], setup_ns / 1e6,
                           block_bytes, len(data), storage_bytes, position_bytes, rss, reference_recall, reference_ms, reference_match,
                           'full-c-exact', args.upper_steps,
                           upper_reads, mean[8], scan_count, routing_bytes, upper_block_bytes, upper_storage_bytes, upper_map_bytes, candidate_capacity]))
    return row

def run(args):
    resolve_paths(args)
    efs = args.ef if isinstance(args.ef, list) else [args.ef]
    if not 0 < len(efs) <= 1024 or any(not 0 < ef < (1 << 64) for ef in efs):
        raise ValueError('provide 1..1024 positive uint64 ef values')
    if args.k <= 0 or args.upper_steps != 1:
        raise ValueError('Compass requires positive k and --upper-steps 1')
    if args.no_oram_snapshot and (args.oram_snapshot is not None or args.rebuild_oram):
        raise ValueError('--no-oram-snapshot cannot be combined with --oram-snapshot/--rebuild-oram')
    data, header, book, nodes, upper, scan, scan_level = prepared_full(args.prepared)
    dim, degree, p, centers, scalar, metric, n, entry = header
    queries = read_vectors(args.query, args.query_format, args.query_dtype)
    truth = read_groundtruth(args.groundtruth, args.gt_format)
    count = min(args.queries, len(queries), len(truth))
    if count <= 0 or queries.shape[1] != dim or truth.shape[1] < args.k:
        raise ValueError('query/groundtruth/search dimensions mismatch')
    if min(args.ef_spec, args.ef_n, args.k, args.threads, args.repeats, args.upper_steps) <= 0 or args.warmup < 0:
        raise ValueError('invalid search parameters')
    if args.threads > 256 or args.repeats > 10000 or count * args.repeats * len(efs) > 1000000 or args.warmup * len(efs) > 1000000:
        raise ValueError('Compass ef batch query limits exceeded')
    command = [str(args.build_dir / 'compass/compass_benchmark')]
    if args.mode != 'native':
        # The separate Compass host shares the existing signed enclave image.
        command = [str(args.build_dir / 'compass/compass_sgx_host'),
                   str(args.enclave or args.build_dir / 'apps/sgx/ringdoram_enclave.signed.so')]
    # Reject mismatched binaries before copying a potentially hundred-GB index
    # into a request file or enclave. The runner reports its compiled layout.
    expected = {'dimension': dim, 'degree': degree, 'pq_subspaces': p, 'pq_centroids': centers,
                'scalar': 'uint8' if scalar == 2 else 'float32'}
    instructions = require_runner(command, args, expected)
    probe = subprocess.run([command[0], '--describe-layout'], text=True, capture_output=True, check=True)
    layout = json.loads(probe.stdout)
    if layout.get('search_protocol') != 5:
        raise RunnerUnavailableError(f'Compass runner lacks ef batch search support; rebuild it.\n{instructions}')
    if any(layout.get(key) != value for key, value in expected.items()):
        raise ValueError(f'runner layout {layout} does not match prepared index {expected}; '
                         f'configure -DCOMPASS_DIM={dim} -DCOMPASS_DEGREE={degree} '
                         f'-DCOMPASS_PQ_SUBSPACES={p} -DCOMPASS_PQ_CENTROIDS={centers} '
                         f'-DCOMPASS_SCALAR={expected["scalar"]} and pass --build-dir')
    if layout.get('mode', '').lower() != args.mode:
        raise ValueError(f'requested mode {args.mode} differs from runner mode {layout.get("mode")}')
    image_path = None
    if not args.no_oram_snapshot:
        if layout.get('snapshot_protocol') != 4:
            raise RunnerUnavailableError(f'Compass runner lacks snapshot support; rebuild it.\n{instructions}')
        image_path = (args.oram_snapshot or args.prepared.with_name(
            f'{args.prepared.stem}.initial_oram.{args.mode}.state')).resolve()
        if image_path in (args.prepared.resolve(), args.query.resolve(), args.groundtruth.resolve(), args.csv.resolve()):
            raise ValueError('ORAM snapshot path must differ from input and CSV paths')
    fields = [int.from_bytes(b'CMPRQ005', 'little'), max(efs), args.ef_spec, args.ef_n, args.k,
              args.z, args.s, args.a, args.evict_batch, args.stash, args.threads, args.warmup, count, args.repeats, len(data)]
    with tempfile.TemporaryDirectory(prefix='compass-benchmark-') as temporary, ExitStack() as resources:
        request = Path(temporary) / 'request.bin'
        report = Path(temporary) / 'report.bin'
        if image_path:
            image_path.parent.mkdir(parents=True, exist_ok=True)
            lock = resources.enter_context(Path(str(image_path) + '.lock').open('a'))
            fcntl.flock(lock, fcntl.LOCK_EX)
        digest = hashlib.sha256()
        with request.open('wb') as out:
            out.write(struct.pack('<15Q', *fields))
            out.write(struct.pack('<2Q', args.upper_steps, (3 if args.rebuild_oram or not image_path.exists() else 1) if image_path else 0))
            out.write(bytes(32))
            out.write(struct.pack('<Q', len(efs)))
            out.write(struct.pack(f'<{len(efs)}Q', *efs))
            for offset in range(0, len(data), 1 << 20):
                chunk = data[offset:offset + (1 << 20)]
                if image_path:
                    digest.update(chunk)
                out.write(chunk)
            out.write(np.ascontiguousarray(queries[:count], dtype='<f4').tobytes())
            if image_path:
                out.seek(136)
                out.write(snapshot_identity(digest.hexdigest(), expected, args))
        process = subprocess.run(command + [str(request), str(report)] + ([str(image_path)] if image_path else []),
                                 text=True, stdout=subprocess.PIPE)
        print(process.stdout, end='')
        process.check_returncode()
        output = report.read_bytes()
    rss = 0
    image_status = 'none'
    for line in process.stdout.splitlines():
        if line.startswith('peak_host_rss_bytes='):
            rss = int(line.partition('=')[2])
        if line.startswith('sgx_mode=') and line.partition('=')[2].lower() != args.mode.split('-')[1]:
            raise ValueError('requested SGX mode does not match built host')
        if line.startswith('oram_snapshot_status='):
            image_status = line.partition('=')[2]
    if image_path and image_status not in ('loaded', 'built'):
        raise ValueError('runner did not report a valid ORAM snapshot outcome')
    frame_bytes = 144 + count * args.repeats * (72 + args.k * 16)
    if len(output) != len(efs) * frame_bytes:
        raise ValueError('Compass ef batch report length mismatch')
    view = memoryview(output)
    summaries = [report_row(view[i * frame_bytes:(i + 1) * frame_bytes], ef, args, count,
                            data, header, book, nodes, upper, scan, scan_level, queries, truth, rss)
                 for i, ef in enumerate(efs)]
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    with args.csv.open('a+', newline='') as out:
        fcntl.flock(out, fcntl.LOCK_EX)
        out.seek(0)
        existing = next(csv.reader(out), None)
        if existing is not None and existing != FIELDS:
            raise ValueError('CSV header mismatch; choose a new --csv')
        out.seek(0, os.SEEK_END)
        writer = csv.DictWriter(out, FIELDS)
        if existing is None:
            writer.writeheader()
        writer.writerows(summaries)
    for row in summaries:
        row['oram_snapshot_status'] = image_status
        row['oram_snapshot_path'] = str(image_path) if image_path else ''
    result = summaries[0] if len(summaries) == 1 else summaries
    print(json.dumps(result, indent=2))
    return result

def parser():
    p = argparse.ArgumentParser(description=__doc__)
    add_dataset_arguments(p)
    p.add_argument('--prepared', type=Path, help='override the dataset prepared file')
    p.add_argument('--query', type=Path, help='override the dataset query path')
    p.add_argument('--groundtruth', type=Path, help='override the dataset groundtruth path')
    p.add_argument('--query-format', choices=['auto', 'fvecs', 'bin'], default='auto', help='bin: uint32 shape header + vectors in --query-dtype')
    p.add_argument('--query-dtype', choices=['uint8', 'float32'], help='bin payload type (sift/deep1m: float32; sift10m/sift100m: uint8); fvecs is always float32')
    p.add_argument('--gt-format', choices=['auto', 'ivecs', 'bin'], default='auto', help='bin: uint32 header + uint32 IDs')
    p.add_argument('--build-dir', type=Path, default=ROOT / 'build/compass-release')
    p.add_argument('--enclave', type=Path, help='override the signed enclave image (default: <build-dir>/apps/sgx/ringdoram_enclave.signed.so)')
    p.add_argument('--mode', choices=['native', 'sgx-sim', 'sgx-hw'], default='native')
    p.add_argument('--csv', type=Path, help='override CSV path (default results/compass_full_c_<dataset>_<mode>.csv)')
    p.add_argument('--oram-snapshot', type=Path, help='initial ORAM snapshot (default: next to prepared index, separated by mode)')
    p.add_argument('--rebuild-oram', action='store_true', help='force fresh BulkLoad and replace the initial snapshot')
    p.add_argument('--no-oram-snapshot', action='store_true', help='disable snapshot load/save; rebuild ORAM each run')
    p.add_argument('--ef', '--ef-list', dest='ef', type=int, nargs='+', default=[20],
                   help='one or more ef values, run in order using one ORAM initialization (default: 20)')
    for name, default in [('ef-spec', 4), ('ef-n', 12), ('k', 10), ('threads', 32),
                          ('queries', 100), ('warmup', 10), ('repeats', 1), ('z', 32), ('s', 58),
                          ('a', 46), ('evict-batch', 8), ('stash', 0)]:
        p.add_argument('--' + name, type=int, default=default)
    p.add_argument('--reference', action='store_true', help='compare every output with the full-C exact plaintext reference')
    p.add_argument('--upper-steps', type=int, default=1, help='compatibility option; only 1 is supported')
    return p

if __name__ == '__main__':
    cli = parser()
    try:
        run(cli.parse_args())
    except (RunnerUnavailableError, ValueError, OSError) as error:
        cli.exit(2, f'error: {error}\n')
