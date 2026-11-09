#!/usr/bin/env python3
"""Render Table 5-style comparator counts from the geometry-only CSV."""

import argparse
import csv
from pathlib import Path


def format_count(value):
    return f"{int(value) / 1000:.1f}K"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, default=Path("results/table5-auto-stash.csv"))
    parser.add_argument("--tex", type=Path, default=Path("results/table5-auto-stash.tex"))
    args = parser.parse_args()
    with args.csv.open(newline="") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise ValueError("empty comparator CSV")
    for row in rows:
        if not all(row.get(field) for field in
                   ("sonic_evict_ms", "qcovert_evict_ms", "time_reduction_percent", "timing_repetitions")):
            raise ValueError("run tools/table5_times.py before rendering")
        sonic = int(row["sonic_comparators"])
        shared = int(row["qcovert_comparators"])
        reduction = 100.0 * (sonic - shared) / sonic
        if abs(reduction - float(row["reduction_percent"])) > 0.01:
            raise ValueError("inconsistent reduction percentage")
        sonic_time = float(row["sonic_evict_ms"])
        shared_time = float(row["qcovert_evict_ms"])
        if sonic_time <= 0 or shared_time <= 0 or abs(
            100.0 * (sonic_time - shared_time) / sonic_time - float(row["time_reduction_percent"])
        ) > 0.01:
            raise ValueError("inconsistent measured time or time reduction")
    capacities = sorted({int(row["Cs"]) for row in rows})
    repetitions = sorted({int(row["timing_repetitions"]) for row in rows})
    caption = (r"Comparator counts for batched eviction with $Z=A=S=8$, $R=0$, "
               r"and automatically derived stash capacity $C_s$ "
               r"($\lambda=80$).")
    lines = [
        r"\documentclass[border=8pt]{standalone}",
        r"\usepackage{booktabs}",
        r"\usepackage{multirow}",
        r"\begin{document}",
        r"\begin{minipage}{18cm}",
        r"\centering",
        r"\textbf{Table 5-style comparison of sorting comparators}\\[5pt]",
        r"\begin{tabular}{rrrrrrrr}",
        r"\toprule",
        r"$N$ & $E$ & $C_s$ & SONIC & Q-COVERT$_{\mathrm{CP}}$ & \shortstack{Comparator\\reduction} & \shortstack{Evict time (ms)\\SONIC / CP} & \shortstack{Time\\reduction} \\",
        r"\midrule",
    ]
    groups = {}
    for row in rows:
        groups.setdefault(int(row["N"]), []).append(row)
    for group_index, (n, group) in enumerate(groups.items()):
        if group_index:
            lines.append(r"\midrule")
        exponent = n.bit_length() - 1
        n_label = rf"\multirow{{{len(group)}}}{{*}}{{$2^{{{exponent}}}$}}"
        for index, row in enumerate(group):
            label = n_label if index == 0 else ""
            reduction = float(row["reduction_percent"])
            lines.append(
                f"{label} & {row['E']} & {row['Cs']} & "
                f"{format_count(row['sonic_comparators'])} & "
                f"{format_count(row['qcovert_comparators'])} & "
                f"{reduction:.1f}\\% & "
                f"{float(row['sonic_evict_ms']):.3f} / {float(row['qcovert_evict_ms']):.3f} & "
                f"{float(row['time_reduction_percent']):.1f}\\% " + r"\\"
            )
    lines += [
        r"\bottomrule",
        r"\end{tabular}\\[5pt]",
        r"{\footnotesize " + caption + r"\par}",
        r"{\footnotesize Only bitonic OSort comparators are counted; "
        r"OCompact and bucket shuffles are excluded. Times are median pure-Evict "
        "wall times over " + ", ".join(map(str, repetitions)) +
        r" runs after one warm-up, excluding Setup.\par}",
        r"\end{minipage}",
        r"\end{document}",
    ]
    args.tex.parent.mkdir(parents=True, exist_ok=True)
    args.tex.write_text("\n".join(lines) + "\n")
    print(f"wrote {args.tex}: {len(rows)} configurations, Cs={capacities}")


if __name__ == "__main__":
    main()
