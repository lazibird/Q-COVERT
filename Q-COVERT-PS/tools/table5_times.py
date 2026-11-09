#!/usr/bin/env python3
"""Add measured pure-Evict times to the Table 5-style comparator CSV."""

import argparse
import csv
import os
import re
import subprocess
from pathlib import Path


TIME_PATTERN = re.compile(r"\bpure_evict_median_ms=([0-9.]+)\b")
TIME_FIELDS = ("sonic_evict_ms", "qcovert_evict_ms", "time_reduction_percent", "timing_repetitions")


def save_rows(path, fieldnames, rows):
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    os.replace(temporary, path)


def run(binary, n, e, backend, repetitions):
    command = [str(binary), str(n), str(e), backend, str(repetitions), "--pure-only"]
    completed = subprocess.run(command, text=True, capture_output=True)
    if completed.returncode:
        raise RuntimeError(f"{command!r} failed ({completed.returncode}):\n{completed.stderr}\n{completed.stdout}")
    match = TIME_PATTERN.search(completed.stdout)
    if not match:
        raise RuntimeError(f"missing pure-Evict median in output:\n{completed.stdout}")
    return float(match.group(1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, default=Path("results/table5-auto-stash.csv"))
    parser.add_argument("--binary", type=Path, default=Path("build/qcovert-release/ringdoram_evict_benchmark"))
    parser.add_argument("--repetitions", type=int, default=11)
    parser.add_argument("--force", action="store_true", help="rerun even rows with both timings")
    args = parser.parse_args()
    if args.repetitions < 1:
        raise ValueError("repetitions must be positive")
    binary = args.binary.resolve()
    if not binary.is_file():
        raise FileNotFoundError(binary)
    with args.csv.open(newline="") as source:
        reader = csv.DictReader(source)
        fieldnames = list(reader.fieldnames or [])
        rows = list(reader)
    if not rows or not {"N", "E", "Cs", "sonic_comparators", "qcovert_comparators"}.issubset(fieldnames):
        raise ValueError("CSV is not a Table 5 comparator result")
    for field in TIME_FIELDS:
        if field not in fieldnames:
            fieldnames.append(field)

    for row in rows:
        if not args.force and all(row.get(field) for field in TIME_FIELDS):
            print(f"N={row['N']} E={row['E']}: cached", flush=True)
            continue
        n, e = int(row["N"]), int(row["E"])
        print(f"N={n} E={e}: SONIC", flush=True)
        sonic = run(binary, n, e, "sonic", args.repetitions)
        print(f"N={n} E={e}: Q-COVERT-PS", flush=True)
        qcovert = run(binary, n, e, "shared", args.repetitions)
        row["sonic_evict_ms"] = f"{sonic:.6f}"
        row["qcovert_evict_ms"] = f"{qcovert:.6f}"
        row["time_reduction_percent"] = f"{100.0 * (sonic - qcovert) / sonic:.6f}"
        row["timing_repetitions"] = str(args.repetitions)
        save_rows(args.csv, fieldnames, rows)
        print(f"N={n} E={e}: {sonic:.6f}/{qcovert:.6f} ms, "
              f"reduction={float(row['time_reduction_percent']):.2f}%", flush=True)


if __name__ == "__main__":
    main()
