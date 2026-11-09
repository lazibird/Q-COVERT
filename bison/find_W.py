#!/usr/bin/env python3
import subprocess
import re
import sys
import json
from pathlib import Path

# ===================== SETTINGS =====================
iteration_values = [5,6,7,8]   # values for --search_io_limit
low_w = 1
high_w = 125
target_recall = 90.0

exec_path = "./build/search_disk_index"

# Regex to extract Recall@10 from output
recall_re = re.compile(r"Recall@10:\s*([0-9]+(?:\.[0-9]+)?)")

# =====================================================

def run_search_command(config, dataset_name, io_limit, beamwidth):
    """Run search_disk_index with given params and return Recall@10 (float)."""
    ds_cfg = config[dataset_name]

    cmd = [
        exec_path,
        f"--data_type={ds_cfg['--data_type']}",
        f"--dist_fn={ds_cfg['--dist_fn']}",
        f"--index_path_prefix={ds_cfg['--index_path_prefix']}",
        f"--query_file={ds_cfg['--query_file']}",
        f"--gt_file={ds_cfg['--gt_file']}",
        f"--result_path={ds_cfg['--result_path']}",
        "--num_nodes_to_cache=0",
        "-T", "16",
        "-L", "125",
        "-W", str(beamwidth),
        "--search_io_limit", str(io_limit),
        "-K", "10",
    ]

    print(f"\rRunning io_limit={io_limit}, beamwidth={beamwidth} ...", end="\n")

    proc = subprocess.run(cmd, capture_output=True, text=True)
    output = proc.stdout + proc.stderr

    # Extract Recall@10
    m = recall_re.search(output)
    if not m:
        print(f"\n⚠️ Recall not found for io_limit={io_limit}, beamwidth={beamwidth}")
        return None
    return float(m.group(1))


def find_best_beamwidth(config, dataset_name):
    results = []

    for io_limit in iteration_values:
        print(f"\n=== Finding minimal beamwidth (-W) for io_limit={io_limit} to reach recall ≥ {target_recall}% ===")

        left, right = low_w, high_w
        best_w = None
        best_recall = None

        while left <= right:
            mid = (left + right) // 2
            recall = run_search_command(config, dataset_name, io_limit, mid)

            if recall is None:
                break

            print(f"\r  Tested W={mid}, Recall@10={recall:.2f}          ", end="\n")

            if recall >= target_recall:
                best_w = mid
                best_recall = recall
                right = mid - 1  # try smaller W
            else:
                left = mid + 1   # need larger W

        if best_w is not None:
            print(f"\n✅ io_limit={io_limit}: minimal W={best_w} gives Recall@10={best_recall:.2f}")
        else:
            print(f"\n❌ io_limit={io_limit}: could not reach Recall ≥ {target_recall}%")

        results.append((io_limit, best_w, best_recall))

    print("\n========= SUMMARY =========")
    for io_limit, w_val, recall in results:
        if w_val is not None:
            print(f"io_limit={io_limit} → W={w_val}, Recall={recall:.2f}")
        else:
            print(f"io_limit={io_limit} → No W found reaching {target_recall}% recall")

    return results


def main():
    if len(sys.argv) != 3:
        print("Usage: python3 find_W.py <config.json> <dataset_name>")
        sys.exit(1)

    config_file = Path(sys.argv[1])
    dataset_name = sys.argv[2]

    if not config_file.exists():
        print(f"Error: Config file '{config_file}' not found.")
        sys.exit(1)

    with open(config_file, "r") as f:
        config = json.load(f)

    if dataset_name not in config:
        print(f"Error: Dataset '{dataset_name}' not found in config file.")
        sys.exit(1)

    find_best_beamwidth(config, dataset_name)


if __name__ == "__main__":
    main()





