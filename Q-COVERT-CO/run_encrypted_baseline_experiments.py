#!/usr/bin/env python3
import csv
import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed

ROOT = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(ROOT, "build", "test_ivf_rabitq_encrypted_baseline")
PARAMS = os.path.join(ROOT, "results", "params.csv")
RESULTS = os.path.join(ROOT, "results")
LOG_DIR = "/tmp/doram_baseline_logs"
NETS = ["fast", "slow"]
NQUERIES = 1000

DATASETS = [
    ("sift", "sift_nprobe", "sift_k2"),
    ("deep1M", "deep1m_nprobe", "deep1m_k2"),
    ("msmarco1m", "msmarco1m_nprobe", "msmaroc1m_k2"),
    ("sift10M", "sift10m_nprobe", "sift10m_k2"),
    ("sift100M", "sift100m_nprobe", "sift100m_k2"),
]


def norm(name):
    return name.strip().lower()


def parse_params(path):
    with open(path, newline="") as fh:
        rows = list(csv.reader(fh))
    if not rows:
        raise RuntimeError("params.csv is empty")
    header = [norm(h) for h in rows[0]]
    jobs = []
    for dataset, nprobe_col, k2_col in DATASETS:
        if nprobe_col not in header:
            raise RuntimeError(f"missing column {nprobe_col}")
        if k2_col not in header:
            raise RuntimeError(f"missing column {k2_col}")
        nprobe_idx = header.index(nprobe_col)
        k2_idx = header.index(k2_col)
        pairs = []
        for row in rows[1:]:
            if len(row) <= max(nprobe_idx, k2_idx):
                continue
            nprobe = row[nprobe_idx].strip()
            k2 = row[k2_idx].strip()
            if nprobe and k2:
                pairs.append((int(nprobe), int(k2)))
        if not pairs:
            raise RuntimeError(f"no parameter rows for {dataset}")
        jobs.append((dataset, pairs))
    return jobs


def command(dataset, pairs, net):
    nprobes = [str(n) for n, _ in pairs]
    k2s = [str(k) for _, k in pairs]
    max_nprobe = max(int(n) for n in nprobes)
    return [
        BIN,
        "--dataset", dataset,
        "--source", os.path.join(os.path.dirname(ROOT), "data", dataset),
        "--assignment", "split",
        "--nprobes", ",".join(nprobes),
        "--k2", ",".join(k2s),
        "--nqueries", str(NQUERIES),
        "--path-batch-clusters", str(max_nprobe),
        "--net-sim", net,
        "--results-dir", RESULTS,
        "--no-config",
    ]


def run_one(cmd, log_path):
    with open(log_path, "wb") as fh:
        return subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT)


def run_dataset(item):
    dataset, pairs = item
    os.makedirs(LOG_DIR, exist_ok=True)
    for net in NETS:
        cmd = command(dataset, pairs, net)
        log_path = os.path.join(LOG_DIR, f"{dataset}_{net}.log")
        print(f"[start] {dataset} {net}", flush=True)
        proc = run_one(cmd, log_path)
        if proc.returncode != 0:
            print(f"[FAIL] {dataset} {net} rc={proc.returncode} log={log_path}", flush=True)
            with open(log_path, "r", errors="replace") as fh:
                tail = fh.read()[-2000:]
            print(tail, flush=True)
            return (dataset, net, False)
        print(f"[done] {dataset} {net}", flush=True)
    return (dataset, None, True)


def main():
    dry_run = "--dry-run" in sys.argv
    jobs = parse_params(PARAMS)
    if dry_run:
        for dataset, pairs in jobs:
            for net in NETS:
                print(" ".join(command(dataset, pairs, net)))
        return 0

    os.makedirs(LOG_DIR, exist_ok=True)
    with ProcessPoolExecutor(max_workers=len(jobs)) as pool:
        futures = [pool.submit(run_dataset, job) for job in jobs]
        for future in as_completed(futures):
            dataset, net, ok = future.result()
            print(f"[finished] {dataset} {net or ''} ok={ok}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
