#!/usr/bin/env python3
"""Plaintext (no-ORAM) recall evaluation with the exact online search logic.

Replicates the C++ online pipeline on the decoded prepared cache: raw-space
centroid top-nprobe (ties by cluster id), RaBitQ quantized estimates, then
exact top-k2 (ties by vector id) or approximate per-rank quota selection
(partition_topk), full-vector L2 re-rank, top-k recall against the
groundtruth. Deterministic, so recall matches the ORAM benchmark for the same
prepared data; only timing must be measured on the real path.

Approx quotas come from the same sources as the online driver: plaintext
calibration (rank_counts + allocate from calibrate_topk.py, on the first
--calibration-queries queries, default 100) or the JSON rank_fractions via
--use-config-quotas.
"""
import argparse
import csv
import json
import math
import sys
import time
from pathlib import Path

import numpy as np

from benchmark import ROOT, resolve_path, selection_quotas, vecs
from calibrate_topk import allocate, estimate_cluster, load_prepared, prepared_file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=ROOT / 'config/dataset_defaults.json')
    parser.add_argument('--dataset', default='SIFT')
    parser.add_argument('--data-root', type=Path, default=ROOT.parent, help='parent of data/ used by JSON paths')
    parser.add_argument('--cache-dir', type=Path, default=ROOT.parent / 'data/prepared/ps/qcovert')
    parser.add_argument('--prepared', type=Path)
    parser.add_argument('--candidate-selection', choices=['exact', 'approx'])
    parser.add_argument('--nprobe', type=int, nargs='+', required=True)
    parser.add_argument('--k2', type=int, nargs='+', required=True)
    parser.add_argument('--k', type=int, help='final top-k (default: config query.k)')
    parser.add_argument('--alpha', type=float, default=2.0)
    parser.add_argument('--calibration-queries', type=int,
                        help='approx quota calibration query count (default: 100)')
    parser.add_argument('--use-config-quotas', action='store_true')
    parser.add_argument('--uniform-quotas', action='store_true',
                        help='give every probed cluster the same quota ceil(alpha*k2/nprobe)')
    parser.add_argument('--shuffle-seed', type=int, default=42,
                        help='seed for the approx intra-cluster slot permutations')
    parser.add_argument('--queries', type=int, help='first N queries (default: all)')
    parser.add_argument('--csv', type=Path, help='append recall rows')
    args = parser.parse_args()
    if len(args.nprobe) != len(args.k2):
        raise ValueError('--nprobe and --k2 must have the same length')

    profile = json.loads(args.config.read_text())['datasets'][args.dataset]
    root = args.data_root.resolve()
    k = args.k if args.k is not None else profile['query'].get('k', 10)
    if not 0 < k <= 256:
        raise ValueError('k must be an integer in [1, 256]')
    mode = args.candidate_selection or profile['query']['candidate_selection'].get('mode', 'exact')
    if args.use_config_quotas and args.uniform_quotas:
        raise ValueError('--uniform-quotas cannot be combined with --use-config-quotas')
    if (args.use_config_quotas or args.uniform_quotas) and mode != 'approx':
        raise ValueError('--use-config-quotas/--uniform-quotas require approx selection')
    path = args.prepared.resolve() if args.prepared else \
        prepared_file(profile, root, args.cache_dir.resolve(), args.dataset, None)
    rotation, offsets, centroids, entries, tau = load_prepared(path, profile['data']['storage_scalar'])
    all_queries = np.asarray(vecs(resolve_path(profile['files']['query'], root), 'float'), dtype=np.float32)
    truth = np.asarray(vecs(resolve_path(profile['files']['groundtruth'], root), 'int'), dtype=np.int64)
    count = min(args.queries, len(all_queries)) if args.queries else len(all_queries)
    if count <= 0 or len(truth) < count or truth.shape[1] < k:
        raise ValueError('query/groundtruth shape or requested count invalid')
    queries, truth = all_queries[:count], truth[:count, :k]

    nc = len(centroids)
    jobs = sorted(zip(args.nprobe, args.k2))
    for nprobe, k2 in jobs:
        if not 0 < nprobe <= nc or k2 < k:
            raise ValueError('require 0<nprobe<=clusters and k2>=k')
    max_nprobe = jobs[-1][0]

    # Group candidate fields once by cluster so per-cluster slices are contiguous.
    # The vector ID of an entry is its row index in the entries array.
    print(f'Load and group candidates by cluster: {path}', file=sys.stderr)
    t0 = time.time()
    order = np.argsort(entries['cid'], kind='stable')
    starts = np.concatenate(([0], np.cumsum(np.bincount(entries['cid'].astype(np.int64), minlength=nc))))
    grouped = entries[order]
    grouped_ids = order.astype(np.int64)
    del order
    centroid_values = np.asarray(centroids['values'], dtype=np.float32)
    centroid_valid = np.asarray(centroids['valid'])
    centroid_norm2 = np.sum(centroid_values.astype(np.float64) ** 2, axis=1)
    print(f'Candidates grouped: {len(grouped)} entries, {nc} clusters, {time.time() - t0:.1f}s', file=sys.stderr)

    def scan(query):
        """Per-cluster quantized scores for the probe, in probe order."""
        q64 = query.astype(np.float64)
        centroid_d2 = centroid_norm2 - 2 * (centroid_values @ q64) + q64 @ q64
        centroid_d2 = np.where(centroid_valid == 1, centroid_d2, np.inf)
        probe = np.lexsort((np.arange(nc), centroid_d2))[:max_nprobe]
        rotated = query @ rotation
        parts = []
        for cid in probe:
            lo, hi = starts[cid], starts[cid + 1]
            scores = (estimate_cluster(rotated, centroids[cid], offsets, grouped[lo:hi])
                      if hi > lo else np.empty(0, dtype=np.float32))
            parts.append((scores, grouped_ids[lo:hi]))
        return q64, parts, probe

    quotas = {}
    if mode == 'approx':
        if args.uniform_quotas:
            for nprobe, k2 in jobs:
                quota = math.ceil(args.alpha * k2 / nprobe)
                if quota > tau:
                    raise ValueError('uniform quota exceeds the padded cluster capacity')
                quotas[(nprobe, k2)] = [quota] * nprobe
                print(f'Uniform quotas: nprobe={nprobe} k2={k2} alpha={quota * nprobe / k2:.6g} quota_per_cluster={quota}',
                      file=sys.stderr)
        elif args.use_config_quotas:
            for nprobe, k2 in jobs:
                profile['query']['nprobe'], profile['query']['k2'] = nprobe, k2
                quotas[(nprobe, k2)] = selection_quotas(profile, tau)
            print(f'Use quotas from JSON rank_fractions', file=sys.stderr)
        else:
            cal_count = min(args.calibration_queries or 100, len(all_queries))
            print(f'Plaintext quota calibration: first {cal_count} queries, alpha={args.alpha}', file=sys.stderr)
            counts = {job: np.zeros(job[0], dtype=np.int64) for job in jobs}
            for query in all_queries[:cal_count]:
                _, parts, _ = scan(query)
                scores = np.concatenate([s for s, _ in parts])
                ids = np.concatenate([i for _, i in parts])
                ranks = np.concatenate([np.full(len(s), r, dtype=np.int64)
                                        for r, (s, _) in enumerate(parts)])
                sel = np.lexsort((ids, scores))
                for nprobe, k2 in jobs:
                    take = sel[ranks[sel] < nprobe][:k2]
                    counts[(nprobe, k2)] += np.bincount(ranks[take], minlength=nprobe)
            for nprobe, k2 in jobs:
                quotas[(nprobe, k2)] = allocate(counts[(nprobe, k2)], math.ceil(args.alpha * k2), tau)
                print(f'  nprobe={nprobe} k2={k2} alpha={sum(quotas[(nprobe, k2)]) / k2:.6g}',
                      file=sys.stderr)

    def rerank_hits(cand, q64, gt):
        """Full-vector L2 re-rank, top-k by (distance, id), count groundtruth hits."""
        if not len(cand):
            return 0
        exact = np.sum((np.asarray(entries['values'][cand], dtype=np.float64) - q64) ** 2, axis=1)
        topk = cand[np.lexsort((cand, exact))[:k]]
        return len(set(topk.tolist()) & set(gt.tolist()))

    # partition_topk splits each probed cluster's padded tau slots (storage order,
    # i.e. the setup-time shuffle interspersing real entries and dummies) into
    # quota equal segments and takes the best entry per segment. The intra-cluster
    # permutation is fixed per ORAM snapshot; here each cluster gets one fixed
    # seeded permutation, which matches in expectation over queries.
    perm_cache = {}

    def cluster_perm(cid):
        perm = perm_cache.get(cid)
        if perm is None:
            perm = np.random.default_rng((args.shuffle_seed, int(cid))).permutation(tau)
            perm_cache[cid] = perm
        return perm

    invalid_id = np.iinfo(np.int64).max

    def segment_representatives(parts, quota):
        """Best (score,id) per equal segment of each cluster's shuffled tau slots."""
        rep_scores, rep_ids = [], []
        for rank, (scores, ids) in enumerate(parts):
            q = quota[rank]
            if not q:
                continue
            scores_tau = np.full(tau, np.inf, dtype=np.float64)
            ids_tau = np.full(tau, invalid_id, dtype=np.int64)
            perm = cluster_perm(int(probe_cids[rank]))
            scores_tau[perm[:len(scores)]] = scores
            ids_tau[perm[:len(ids)]] = ids
            for p in range(q):
                seg_s = scores_tau[p * tau // q:(p + 1) * tau // q]
                seg_i = ids_tau[p * tau // q:(p + 1) * tau // q]
                best = np.lexsort((seg_i, seg_s))[0]
                rep_scores.append(seg_s[best])
                rep_ids.append(seg_i[best])
        return np.asarray(rep_scores), np.asarray(rep_ids)

    hits = {job: 0 for job in jobs}
    begin = time.time()
    for qi, (query, gt) in enumerate(zip(queries, truth)):
        q64, parts, probe_cids = scan(query)
        if mode == 'exact':
            scores = np.concatenate([s for s, _ in parts])
            ids = np.concatenate([i for _, i in parts])
            ranks = np.concatenate([np.full(len(s), r, dtype=np.int64)
                                    for r, (s, _) in enumerate(parts)])
            sel = np.lexsort((ids, scores))  # exact_topk order: score asc, ties by id asc
            for nprobe, k2 in jobs:
                cand = ids[sel[ranks[sel] < nprobe][:k2]]
                hits[(nprobe, k2)] += rerank_hits(cand, q64, gt)
        else:
            for nprobe, k2 in jobs:
                rep_scores, rep_ids = segment_representatives(parts[:nprobe], quotas[(nprobe, k2)])
                cand = rep_ids[np.lexsort((rep_ids, rep_scores))[:k2]]
                cand = cand[cand != invalid_id][:k2]
                hits[(nprobe, k2)] += rerank_hits(cand, q64, gt)
        if (qi + 1) % 50 == 0 or qi + 1 == count:
            print(f'\rPlaintext queries: {qi + 1}/{count}, elapsed {time.time() - begin:.0f}s',
                  end='', flush=True, file=sys.stderr)
    print(file=sys.stderr)

    columns = ['n_probe', 'k2', 'k', 'mode', 'alpha', 'recall_at_k', 'queries']
    out = None
    writer = None
    if args.csv:
        path = args.csv
        fresh = not path.exists() or path.stat().st_size == 0
        if not fresh:
            with path.open(newline='') as existing:
                first = next(csv.reader(existing), None)
            if first is not None and first != columns:
                raise ValueError(f'CSV schema mismatch: {path}; use a new --csv path')
        out = path.open('a', newline='')
        writer = csv.writer(out)
        if fresh:
            writer.writerow(columns)
    for nprobe, k2 in jobs:
        recall = hits[(nprobe, k2)] / (k * count)
        alpha = f'{sum(quotas[(nprobe, k2)]) / k2:.6g}' if mode == 'approx' else ''
        print(f'nprobe={nprobe} k2={k2} k={k} mode={mode} alpha={alpha} recall@k={recall:.6f} (queries={count})')
        if writer:
            writer.writerow([nprobe, k2, k, mode, alpha, f'{recall:.6f}', count])
    if out:
        out.close()


if __name__ == '__main__':
    main()
