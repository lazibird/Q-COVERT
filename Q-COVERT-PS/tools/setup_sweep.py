#!/usr/bin/env python3
"""Run each Setup size in a fresh process and record its peak resident memory."""

import argparse
import csv
import os
import resource
import subprocess
import sys
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/qcovert-release/ringdoram_setup_benchmark"))
    parser.add_argument("--csv", type=Path, default=Path("results/setup-comparison.csv"))
    parser.add_argument("--memory-csv", type=Path, default=Path("results/setup-memory.csv"))
    parser.add_argument("--start", type=int, default=4096)
    parser.add_argument("--stop", type=int, default=8388608)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--memory-gib", type=int, default=16)
    return parser.parse_args()


def main():
    args = parse_args()
    if min(args.start, args.stop, args.repetitions, args.memory_gib) <= 0 or args.start > args.stop:
        raise ValueError("start, stop, repetitions and memory-gib must be positive; start <= stop")
    sizes = []
    n = args.start
    while n <= args.stop:
        sizes.append(n)
        n *= 2
    binary = args.binary.resolve()
    if not binary.is_file():
        raise FileNotFoundError(binary)
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    args.memory_csv.parent.mkdir(parents=True, exist_ok=True)
    memory_exists = args.memory_csv.is_file() and args.memory_csv.stat().st_size > 0

    with args.memory_csv.open("a", newline="") as memory_file:
        writer = csv.writer(memory_file)
        if not memory_exists:
            writer.writerow(["N", "peak_rss_mib", "status"])
        for n in sizes:
            print(f"setup sweep: N={n}", flush=True)

            def limit_address_space():
                maximum = args.memory_gib * 1024**3
                resource.setrlimit(resource.RLIMIT_AS, (maximum, maximum))

            command = [str(binary), "--repetitions", str(args.repetitions),
                       "--csv", str(args.csv), str(n)]
            process = subprocess.Popen(command, preexec_fn=limit_address_space)
            _, status, usage = os.wait4(process.pid, 0)
            process.returncode = os.waitstatus_to_exitcode(status)
            peak_mib = usage.ru_maxrss / 1024  # Linux reports ru_maxrss in KiB.
            writer.writerow([n, f"{peak_mib:.1f}", process.returncode])
            memory_file.flush()
            print(f"setup sweep: N={n}, peak RSS={peak_mib:.1f} MiB, exit={process.returncode}", flush=True)
            if process.returncode != 0:
                return process.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
