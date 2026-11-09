#!/usr/bin/env python3
"""Learn public per-rank partition fractions from exact RaBitQ top-k2 on plaintext queries."""
import argparse
import json
import math
from pathlib import Path
import struct

import numpy as np

from benchmark import ROOT, PROGRESS_PREFIX, resolve_path, sha256, vecs, load_base, \
    preparation_identity_with_sources, matching_prepared_manifests

PREPARED_MAGIC = 0x3150435452564351


def prepared_file(profile, data_root, cache_dir, dataset, explicit):
    if explicit:
        return explicit.resolve()
    base = resolve_path(profile['files']['base'], data_root)
    values = load_base(base)
    identity = preparation_identity_with_sources(profile, data_root, values)
    for manifest, meta in matching_prepared_manifests(cache_dir / dataset, identity):
        path = manifest.parent / 'prepared.bin'
        if path.is_file() and sha256(path) == meta['prepared_sha256']:
            return path
    raise ValueError('no matching prepared cache; run benchmark.py --prepare-only or pass --prepared')


def load_prepared(path, scalar):
    header = np.memmap(path, dtype='<u8', mode='r', shape=(6,))
    magic, dim, kind, n, nc, tau = map(int, header)
    if magic != PREPARED_MAGIC or dim not in (96, 128, 768) or kind != (2 if scalar == 'uint8' else 1):
        raise ValueError('prepared dataset type/version mismatch')
    padded = (dim + 63) // 64 * 64
    pos = 48
    rotation = np.memmap(path, dtype='<f4', mode='r', offset=pos, shape=(dim, padded))
    pos += dim * padded * 4
    offsets = np.memmap(path, dtype='<f4', mode='r', offset=pos, shape=(padded,))
    pos += padded * 4
    centroid_dtype = np.dtype([('valid', '<u8'), ('values', '<f4', (dim,)), ('rotated', '<f4', (padded,))])
    centroids = np.memmap(path, dtype=centroid_dtype, mode='r', offset=pos, shape=(nc,))
    pos += nc * centroid_dtype.itemsize
    value_type = 'u1' if scalar == 'uint8' else '<f4'
    entry_dtype = np.dtype([('cid', '<u8'), ('values', value_type, (dim,)),
                            ('signs', '<u8', (padded // 64,)), ('norm', '<f4'), ('x0', '<f4')])
    if path.stat().st_size != pos + n * entry_dtype.itemsize:
        raise ValueError('prepared dataset length mismatch')
    entries = np.memmap(path, dtype=entry_dtype, mode='r', offset=pos, shape=(n,))
    if np.any(entries['cid'] >= nc):
        raise ValueError('prepared dataset cluster ID out of range')
    return rotation, offsets, centroids, entries, tau


def estimate_cluster(rotated, centroid, offsets, entries):
    residual = rotated - centroid['rotated']
    lo, hi = np.min(residual), np.max(residual)
    width = np.float32((hi - lo) / np.float32(15))
    if width > 0:
        codes = np.clip((residual - lo) / width + offsets, 0, 15).astype(np.uint8).astype(np.float32)
    else:
        codes = np.zeros_like(residual)
    norm2 = np.sum(residual * residual, dtype=np.float32)
    # Same little-endian bit order and 1-bit signs as the C++ estimate().
    signs = np.unpackbits(entries['signs'].view('u1').reshape(len(entries), -1), axis=1, bitorder='little')
    signs = signs.astype(np.float32) * 2 - 1
    sign_sum = np.sum(signs, axis=1, dtype=np.float32)
    code_sum = signs @ codes
    dot = (lo * sign_sum + width * code_sum) / np.sqrt(np.float32(len(codes)))
    norm, x0 = entries['norm'], entries['x0']
    score = norm * norm + norm2 - 2 * norm * dot / np.where(x0 > 0, x0, 1)
    return np.where(norm == 0, norm2, score)


def rank_counts(queries, rotation, offsets, centroids, entries, nprobe, k2, progress=None):
    if progress:
        progress(0, len(queries))
    nc = len(centroids)
    order = np.argsort(entries['cid'], kind='stable')
    starts = np.concatenate(([0], np.cumsum(np.bincount(entries['cid'].astype(np.int64), minlength=nc))))
    counts = np.zeros(nprobe, dtype=np.int64)
    for qi, query in enumerate(queries):
        query = np.asarray(query, dtype=np.float32)
        centroid_scores = np.sum((centroids['values'].astype(np.float64) - query.astype(np.float64)) ** 2,
                                 axis=1)
        ranks = np.lexsort((np.arange(nc), centroid_scores))[:nprobe]
        rotated = query @ rotation
        scores, ids, owners = [], [], []
        for rank, cid in enumerate(ranks):
            index = order[starts[cid]:starts[cid + 1]]
            if len(index):
                scores.append(estimate_cluster(rotated, centroids[cid], offsets, entries[index]))
                ids.append(index)
                owners.append(np.full(len(index), rank, dtype=np.int64))
        if scores:
            selected = np.lexsort((np.concatenate(ids), np.concatenate(scores)))[:k2]
            counts += np.bincount(np.concatenate(owners)[selected], minlength=nprobe)
        if progress:
            progress(qi + 1, len(queries))
        else:
            print(f'\rCalibrate exact quantized top-k2: {(qi + 1) * 100 // len(queries)}% ({qi + 1}/{len(queries)})',
                  end='', flush=True)
    if not progress:
        print()
    return counts


def allocate(counts, budget, capacity):
    nprobe = len(counts)
    if budget < 1 or budget > nprobe * capacity:
        raise ValueError('alpha*k2 exceeds the available partition capacity')
    # One smoothed observation per rank; a public minimum of one partition per
    # probed cluster when the budget allows it.
    weights = counts.astype(np.float64) + 1
    baseline = 1 if budget >= nprobe else 0
    result = np.full(nprobe, baseline, dtype=np.int64)
    ideal = weights / weights.sum() * budget
    remaining = budget - int(result.sum())
    while remaining:
        eligible = result < capacity
        if not eligible.any():
            raise ValueError('partition capacity exhausted')
        gain = np.where(eligible, ideal - result, -np.inf)
        index = int(np.argmax(gain))
        result[index] += 1
        remaining -= 1
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=ROOT / 'config/dataset_defaults.json')
    parser.add_argument('--dataset', default='SIFT')
    parser.add_argument('--data-root', type=Path, default=ROOT.parent, help='parent of data/ used by JSON paths')
    parser.add_argument('--cache-dir', type=Path, default=ROOT.parent / 'data/prepared/ps/qcovert')
    parser.add_argument('--prepared', type=Path)
    parser.add_argument('--nprobe', type=int, required=True)
    parser.add_argument('--k2', type=int, required=True)
    parser.add_argument('--alpha', type=float, required=True)
    parser.add_argument('--queries', type=int, default=100)
    parser.add_argument('--output-json', type=Path, help='write integer quotas and fractions for the benchmark driver')
    parser.add_argument('--progress-protocol', action='store_true', help='emit benchmark progress records')
    parser.add_argument('--write-config', action='store_true', help='set nprobe, k2, approximate mode and fractions in JSON')
    args = parser.parse_args()
    if not 0 < args.nprobe or not 0 < args.k2 or not math.isfinite(args.alpha) or args.alpha < 1:
        raise ValueError('require nprobe>0, k2>0, finite alpha>=1')
    if not math.isfinite(args.alpha * args.k2):
        raise ValueError('alpha*k2 must be finite')
    cfg = json.loads(args.config.read_text())
    profile = cfg['datasets'][args.dataset]
    path = prepared_file(profile, args.data_root.resolve(), args.cache_dir.resolve(), args.dataset, args.prepared)
    rotation, offsets, centroids, entries, tau = load_prepared(path, profile['data']['storage_scalar'])
    if args.nprobe > len(centroids) or args.k2 > args.nprobe * tau or not 0 < args.queries:
        raise ValueError('query parameters exceed prepared dataset capacity')
    available = vecs(resolve_path(profile['files']['query'], args.data_root.resolve()), 'float')
    if args.queries > len(available):
        raise ValueError('requested calibration queries exceed dataset')
    budget = math.ceil(args.alpha * args.k2)
    if budget > args.nprobe * tau:
        raise ValueError('alpha*k2 exceeds the available partition capacity')
    progress = (lambda done, total: print(
        f'{PROGRESS_PREFIX}calibration/queries\t{done}\t{total}', flush=True)) if args.progress_protocol else None
    counts = rank_counts(available[:args.queries], rotation, offsets, centroids, entries, args.nprobe, args.k2, progress)
    quota = allocate(counts, budget, tau)
    fractions = [round(int(value) / args.k2, 12) for value in quota]
    if args.output_json:
        args.output_json.write_text(json.dumps({'quotas': quota.tolist(), 'rank_fractions': fractions,
                                               'queries': args.queries}) + '\n')
    else:
        print(json.dumps(fractions, separators=(',', ':')))
    if args.write_config:
        profile['query'].update(nprobe=args.nprobe, k2=args.k2)
        profile['query']['candidate_selection'] = {'mode': 'approx', 'rank_fractions': fractions}
        pending = args.config.with_name(args.config.name + '.pending')
        pending.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + '\n')
        pending.replace(args.config)
        print(f'Updated {args.config}; total partition representatives={int(quota.sum())}, alpha={quota.sum() / args.k2:.4f}')


if __name__ == '__main__':
    main()
