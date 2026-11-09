#!/usr/bin/env python3
import subprocess
import re
import sys

# SETTINGS
iteration_values = [5, 6, 7, 8, 9, 10]      # i values to try

low_w = 4                             # lower bound for beamwidth
high_w = 50                           # upper bound for beamwidth

client_exec = "./build/client"
server_exec = "./build/server"
# dataset = "sift10m"
# config_file = "config/rr_oram_build.json"
extra_args = ["nc=0"]            # extra flags for client
target_recall = 90.0                  # target recall threshold

# regex to extract Recall@10
recall_re = re.compile(r"Recall@10:\s*([0-9]+(?:\.[0-9]+)?)")


def ds_to_port(value):
    mapping = {
        "sift10m": 8000,
        "sift50m": 8005,
        "sift100m": 8010,

        "laion100k_R128": 8015,
        "sift_1M_R128": 8020,
        "trip_R128": 8025,
        "trip_R256": 8030,
        "msmarco_R128": 8035,
        "msmarco_R256": 8040
    }

    if value not in mapping:
        raise ValueError(f"Unknown string: {value}")

    return mapping[value]


def run_client_command(i_val, w_val, dataset):
    
    cmd = [
        client_exec,
        f"d={dataset}",
        f"c={config_file}",
        f"i={i_val}",
        f"w={w_val}",
        f"p={ds_to_port(dataset)}"
    ] + extra_args

    proc = subprocess.run(cmd, capture_output=True, text=True)
    output = proc.stdout + proc.stderr

    m = recall_re.search(output)
    if m:
        return float(m.group(1))
    else:
        print(f"Recall not found for i={i_val}, w={w_val}")
        return None

def client(dataset):
    results = []
    for i_val in iteration_values:
        print(f"\n=== Finding minimal w for i={i_val} to reach recall ≥ {target_recall} ===")
        left = low_w
        right = high_w
        best_w = None

        while left <= right:
            mid = (left + right) // 2
            recall = run_client_command(i_val, mid, dataset)
            if recall is None:
                # fallback if parsing failed, just skip
                break

            print(f"Test w={mid}: Recall@10={recall:.2f}")

            if recall >= target_recall:
                best_w = mid
                right = mid - 1  # try smaller w
            else:
                left = mid + 1   # need larger w

        if best_w is not None:
            print(f"✅ Minimal w for i={i_val} to reach recall ≥ {target_recall}: w={best_w}")
            results.append((i_val, best_w, recall))
        else:
            print(f"❌ Could not reach recall ≥ {target_recall} for i={i_val}")
            results.append((i_val, None, None))

    # # summary
    # print("\n========= RESULTS =========")
    # for i_val, w_val, recall in results:
    #     if w_val is not None:
    #         print(f"i={i_val} → w={w_val}, recall={recall:.2f}")
    #     else:
    #         print(f"i={i_val} → No w found reaching {target_recall}% recall")

def server(dataset):
    """Run server in infinite loop to match every client call."""
    print("Starting infinite server loop...")
    while True:
        cmd = [
            server_exec,
            f"d={dataset}",
            f"c={config_file}",
            f"p={ds_to_port(dataset)}"
        ]
        proc = subprocess.run(cmd)
        print(f"Server exited with code {proc.returncode}, restarting...")

def main():
    if len(sys.argv) != 3:
        print("Usage: python3 script.py <1=server, 2=client> dataset")
        sys.exit(1)

    dataset = sys.argv[2]
    choice = sys.argv[1]

    if choice == "1":
        server(dataset)
    elif choice == "2":
        client(dataset)
    else:
        print("Invalid choice. Use 1 for server, 2 for client.")
        sys.exit(1)

if __name__ == "__main__":
    main()

