#!/usr/bin/env python3
"""Sweep ef on the plaintext reference path (no ORAM) and log recall@k to CSV.

Uses the same deterministic plaintext oracle as diagnose_recall.py
(plaintext.reference_scan), so recall matches the ORAM/SGX benchmark for the
same prepared data; only timing must be measured on the real path. ef starts
at --ef-start and increases in steps of --ef-spec until recall@k reaches
--target (or --ef-max is hit). All swept rows are appended to the CSV.
"""
import argparse
import csv
import sys
import time
from multiprocessing import Pool
from pathlib import Path

import numpy as np

from benchmark import prepared_full, recall
from plaintext import reference_scan
from dataio import add_dataset_arguments, resolve_paths, read_vectors, read_groundtruth

CTX = {}


def _run(job):
    qi, ef = job
    output = reference_scan(CTX['queries'][qi], CTX['nodes'], CTX['upper'], CTX['scan'],
                            CTX['scan_level'], CTX['book'], CTX['metric'],
                            ef, CTX['ef_spec'], CTX['ef_n'], CTX['k'])
    return qi, output


def main():
    p = argparse.ArgumentParser(description=__doc__)
    add_dataset_arguments(p)
    for name in ('prepared', 'query', 'groundtruth'):
        p.add_argument('--' + name, type=Path)
    p.add_argument('--query-format', choices=('auto', 'fvecs', 'bin'), default='auto')
    p.add_argument('--query-dtype', choices=('uint8', 'float32'))
    p.add_argument('--gt-format', choices=('auto', 'ivecs', 'bin'), default='auto')
    p.add_argument('--queries', type=int, default=1000)
    p.add_argument('--ef-start', type=int, default=12)
    p.add_argument('--ef-max', type=int, default=400)
    p.add_argument('--ef-step', type=int, help='sweep stride (default: --ef-spec)')
    p.add_argument('--ef-spec', type=int, default=4)
    p.add_argument('--ef-n', type=int, default=12)
    p.add_argument('--k', type=int, default=10)
    p.add_argument('--target', type=float, default=0.99,
                   help='stop the sweep once recall@k reaches this value')
    p.add_argument('--workers', type=int, default=24)
    p.add_argument('--csv', type=Path, required=True)
    args = p.parse_args()

    resolve_paths(args)
    _, header, book, nodes, upper, scan, scan_level = prepared_full(args.prepared)
    dim, degree, pc, centers, scalar, metric, count, entry = header
    queries = read_vectors(args.query, args.query_format, args.query_dtype)
    truth = read_groundtruth(args.groundtruth, args.gt_format)
    nq = min(args.queries, len(queries), len(truth))
    if nq <= 0 or truth.shape[1] < args.k or args.k > args.ef_start:
        raise ValueError('invalid query/groundtruth shape or ef range')
    print(f'loaded {args.prepared}: n={count} dim={dim} degree={degree} '
          f'queries={nq}', file=sys.stderr)

    CTX.update(queries=np.asarray(queries[:nq]), nodes=nodes, upper=upper, scan=scan,
               scan_level=scan_level, book=book, metric=metric,
               ef_spec=args.ef_spec, ef_n=args.ef_n, k=args.k)
    truth = truth[:nq]

    fresh = not args.csv.exists()
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    with args.csv.open('a', newline='') as out, Pool(args.workers) as pool:
        writer = csv.writer(out)
        if fresh:
            writer.writerow(['dataset', 'ef', 'ef_spec', 'ef_n', 'k', 'queries', 'recall_at_k'])
        ef = args.ef_start
        while ef <= args.ef_max:
            begin = time.time()
            outputs = [None] * nq
            for qi, result in pool.imap_unordered(_run, ((qi, ef) for qi in range(nq)), chunksize=4):
                outputs[qi] = result
            value = recall(outputs, truth, args.k)
            writer.writerow([args.dataset, ef, args.ef_spec, args.ef_n, args.k, nq, f'{value:.6f}'])
            out.flush()
            print(f'{args.dataset} ef={ef} recall@{args.k}={value:.6f} '
                  f'({time.time() - begin:.0f}s)', file=sys.stderr)
            if value >= args.target:
                break
            ef += args.ef_step or args.ef_spec
        else:
            print(f'warning: target {args.target} not reached at ef_max={args.ef_max}', file=sys.stderr)


if __name__ == '__main__':
    main()
