#!/usr/bin/env python3
"""Prepare and benchmark BiSON datasets. See README.md."""

import argparse
import csv
import json
import math
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
PATH_KEYS = {"base_path", "--data_path", "--query_file", "--gt_file", "--index_path_prefix", "--result_path"}

def resolve_entry_paths(entry):
    for key, value in entry.items():
        if isinstance(value, str) and (key.endswith("_path") or key in PATH_KEYS):
            path = Path(value)
            if not path.is_absolute():
                entry[key] = str((REPOSITORY / path).resolve())
    return entry

def binary_path(args, name):
    return getattr(args, "build_dir", ROOT / "build/bin") / name

def raw_profile(dataset):
    profiles = json.loads((REPOSITORY / "Q-COVERT-PS/config/dataset_defaults.json").read_text())["datasets"]
    return profiles[dataset.upper()]["files"]

PAPER_PQ = {"sift": 16, "deep1m": 16, "sift10m": 32, "msmarco1m": 64}
PAPER_CACHE_NODES = {"sift": 12854, "deep1m": 12126, "sift10m": 15912, "msmarco1m": 14304}
SPECS = {
    "sift": (128, "float", "l2", 1_000_000),
    "sift10m": (128, "uint8", "l2", 10_000_000),
    "deep1m": (96, "float", "l2", 1_000_000),
    "msmarco1m": (768, "float", "l2", 1_000_000),
}
PAIR_LATENCY = re.compile(r"Mean Latency:\s*([\d.]+)")


def recall_pattern(k):
    return re.compile(rf"Recall@{k}\s*:\s*([\d.]+)")


NETWORK_MARKER = re.compile(r"BENCH_NET total_bytes=(\d+) direction_changes=(\d+)(?:\s+rtt_count=\d+)?\s+queries=(\d+)")
NETWORKS = {"fast": (3_000_000_000, 1.0), "slow": (400_000_000, 80.0)}
CSV_FIELDS = (
    "dataset", "mode", "k", "T", "W", "L", "queries", "threads", "cache_nodes",
    "cached_levels", "integrity", "recall_at_k_pct", "measured_total_latency_ms",
    "measured_network_latency_ms", "local_latency_ms", "communication_bytes_per_query",
    "communication_mib_per_query", "direction_changes_per_query", "round_trips_per_query",
    "fast_network_ms", "fast_total_ms", "slow_network_ms", "slow_total_ms",
)


def network_cost_ms(bytes_per_query, direction_changes_per_query, bandwidth_bps, rtt_ms):
    """NetIO counts send/receive direction changes; two changes are one RTT."""
    return bytes_per_query * 8 * 1000 / bandwidth_bps + direction_changes_per_query * rtt_ms / 2


def result_row(*, dataset, mode, t, w, l, queries, recall, measured_ms,
               measured_network_ms, bytes_per_query, direction_changes_per_query,
               threads="", cache_nodes=0, cached_levels="", integrity="", k=10):
    local_ms = max(0.0, measured_ms - max(0.0, measured_network_ms))
    row = {"dataset": dataset, "mode": mode, "k": k, "T": t, "W": w, "L": l,
           "queries": queries, "threads": threads, "cache_nodes": cache_nodes,
           "cached_levels": cached_levels, "integrity": integrity,
           "recall_at_k_pct": recall, "measured_total_latency_ms": measured_ms,
           "measured_network_latency_ms": measured_network_ms, "local_latency_ms": local_ms,
           "communication_bytes_per_query": bytes_per_query,
           "communication_mib_per_query": bytes_per_query / (1024 * 1024),
           "direction_changes_per_query": direction_changes_per_query,
           "round_trips_per_query": direction_changes_per_query / 2}
    for name, (bandwidth_bps, rtt_ms) in NETWORKS.items():
        cost = network_cost_ms(bytes_per_query, direction_changes_per_query, bandwidth_bps, rtt_ms)
        row[f"{name}_network_ms"] = cost
        row[f"{name}_total_ms"] = local_ms + cost
    return row


def write_dataset_rows(result_dir, dataset, rows, path=None):
    """One CSV per dataset; replace an existing parameter row on rerun."""
    path = path or result_dir / f"{dataset}.csv"
    key = lambda row: tuple(str(row[field]) for field in
                            ("mode", "k", "T", "W", "L", "queries", "threads", "cache_nodes", "cached_levels", "integrity"))
    existing = {}
    if path.is_file():
        with path.open(newline="") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames != list(CSV_FIELDS):
                raise ValueError(f"{path}: unexpected CSV columns; move the old file before rerunning")
            for row in reader:
                existing[key(row)] = row
    for row in rows:
        existing[key(row)] = row
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(".csv.tmp")
    with temp.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(existing.values())
    temp.replace(path)
    return path


def parse_client_results(output, expected, queries, k=10):
    recalls = [float(value) for value in recall_pattern(k).findall(output)]
    latencies = [float(value) for value in re.findall(r"Total Latency:\s*([\d.]+)", output)]
    network_times = [float(value) for value in re.findall(r"Mean Network Time:\s*([-\d.]+)", output)]
    counters = [(int(total_bytes), int(changes), int(count))
                for total_bytes, changes, count in NETWORK_MARKER.findall(output)]
    if not all(len(values) == expected for values in (recalls, latencies, network_times, counters)):
        raise ValueError("client log is missing recall, latency, network-time or BENCH_NET counters")
    if any(count != queries for _, _, count in counters):
        raise ValueError("BENCH_NET query count differs from the requested query count")
    return recalls, latencies, network_times, counters


def path_arg(value):
    return Path(value).expanduser().resolve()


def dataset_dir(name, data_root):
    return data_root / name


def inspect_bin(path, dim, item_size):
    with path.open("rb") as handle:
        header = handle.read(8)
    if len(header) != 8:
        raise ValueError(f"{path}: missing 8-byte n,d header")
    n, d = struct.unpack("<II", header)
    if d != dim or n < 1 or path.stat().st_size != 8 + n * d * item_size:
        raise ValueError(f"{path}: expected n x {dim} vectors of {item_size} bytes")
    return n


def inspect_truth(path, nq):
    with path.open("rb") as source:
        header = source.read(8)
    if len(header) != 8:
        raise ValueError(f"{path}: missing ground truth header")
    n, width = struct.unpack("<II", header)
    if n != nq or width < 10:
        raise ValueError(f"{path}: expected {nq} queries and at least 10 neighbors")
    size = path.stat().st_size
    if size not in (8 + n * width * 4, 8 + n * width * 8):
        raise ValueError(f"{path}: invalid ground truth file size")
    return width, size == 8 + n * width * 8


def convert_vectors(src, dst, dim, data_type, limit=None):
    suffix = src.suffix.lower()
    item_size = 1 if data_type == "uint8" else 4
    if suffix in (".fbin", ".u8bin", ".bin"):
        n = inspect_bin(src, dim, item_size)
        wanted = limit or n
        if wanted > n:
            raise ValueError(f"{src}: only {n} vectors, requested {wanted}")
        if wanted == n:
            if src != dst:
                shutil.copyfile(src, dst)
        else:
            if src == dst:
                raise ValueError("source and destination coincide while taking a vector prefix")
            with src.open("rb") as source, dst.open("wb") as target:
                source.seek(8)
                target.write(struct.pack("<II", wanted, dim))
                remaining = wanted * dim * item_size
                while remaining:
                    chunk = source.read(min(remaining, 1024 * 1024))
                    target.write(chunk)
                    remaining -= len(chunk)
        return wanted
    if suffix not in (".fvecs", ".bvecs"):
        raise ValueError(f"{src}: expected .fvecs/.bvecs or DiskANN binary")
    if suffix == ".bvecs" and data_type != "uint8":
        raise ValueError(f"{src}: incompatible vector type for {data_type}")
    source_item_size = 4 if suffix == ".fvecs" else 1
    record_size = 4 + dim * source_item_size
    size = src.stat().st_size
    if size == 0 or size % record_size:
        raise ValueError(f"{src}: invalid vector record size")
    available = size // record_size
    n = limit or available
    if n > available:
        raise ValueError(f"{src}: only {available} vectors, requested {n}")
    if src == dst:
        raise ValueError("source and destination coincide during vector conversion")
    with src.open("rb") as source, dst.open("wb") as target:
        target.write(struct.pack("<II", n, dim))
        for row in range(n):
            header = source.read(4)
            if len(header) != 4 or struct.unpack("<I", header)[0] != dim:
                raise ValueError(f"{src}: dimension mismatch at vector {row}")
            payload = source.read(dim * source_item_size)
            if len(payload) != dim * source_item_size:
                raise ValueError(f"{src}: truncated vector {row}")
            if suffix == ".fvecs" and data_type == "uint8":
                values = struct.unpack(f"<{dim}f", payload)
                if any(not math.isfinite(v) or v < 0 or v > 255 or v != int(v) for v in values):
                    raise ValueError(f"{src}: query values cannot be losslessly represented as uint8")
                payload = bytes(int(v) for v in values)
            target.write(payload)
    return n


def convert_truth(src, dst, nq):
    if src.suffix == ".ivecs":
        with src.open("rb") as source, dst.open("wb") as target:
            first = source.read(4)
            if len(first) != 4:
                raise ValueError(f"{src}: missing ground truth for query 0")
            width = struct.unpack("<I", first)[0]
            if width < 1 or width > 100_000:
                raise ValueError(f"{src}: invalid ground truth width {width}")
            target.write(struct.pack("<II", nq, width))
            for row in range(nq):
                if row:
                    header = source.read(4)
                    if len(header) != 4 or struct.unpack("<I", header)[0] != width:
                        raise ValueError(f"{src}: ground truth width mismatch at query {row}")
                values = source.read(width * 4)
                if len(values) != width * 4:
                    raise ValueError(f"{src}: truncated ground truth at query {row}")
                target.write(values)
            if source.read(1):
                raise ValueError(f"{src}: ground truth count differs from query count")
    else:
        inspect_truth(src, nq)
        if src == dst:
            raise ValueError("source and destination coincide while copying ground truth")
        shutil.copyfile(src, dst)


def require_truth_width(config, k):
    item_size = 1 if config["--data_type"] == "uint8" else 4
    nq = inspect_bin(Path(config["--query_file"]), config["dim"], item_size)
    width, _ = inspect_truth(Path(config["--gt_file"]), nq)
    if k > width:
        raise ValueError(
            f"--k={k} exceeds ground-truth width {width}; "
            "rerun groundtruth with a larger K or pass a wider --groundtruth"
        )
    return width


def require_file(path, label):
    if not path.is_file():
        raise FileNotFoundError(f"{label} missing: {path}")


def run(command, log=None):
    print("$", " ".join(map(str, command)), flush=True)
    if log:
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("w") as stream:
            result = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f"command failed ({result.returncode}), see {log}")
        return log.read_text(errors="replace")
    subprocess.run(command, cwd=ROOT, check=True)


def load_config(args):
    config_path = args.config or ROOT / "config" / f"bench_{args.dataset}.json"
    require_file(config_path, "benchmark config; run prepare first")
    config = json.loads(config_path.read_text())
    entry = resolve_entry_paths(config[args.dataset])
    dim, data_type, metric, _ = SPECS[args.dataset]
    if (entry["dim"], entry["--data_type"], entry["--dist_fn"]) != (dim, data_type, metric):
        raise ValueError("config dimension/type/metric disagree with dataset specification")
    return config_path, entry


def cmd_prepare(args):
    dim, data_type, metric, nominal_n = SPECS[args.dataset]
    source = args.source or (REPOSITORY / raw_profile(args.dataset)["base"]).parent
    source = source.resolve()
    target = dataset_dir(args.dataset, args.data_root)
    target.mkdir(parents=True, exist_ok=True)
    ext = "fbin" if data_type == "float" else "u8bin"
    files = raw_profile(args.dataset)
    base = args.base or source / Path(files["base"]).name
    query = args.query or source / Path(files["query"]).name
    if base is None or query is None:
        raise FileNotFoundError("base/query vectors not found; pass --base and --query")
    base_dst, query_dst = target / f"base.{ext}", target / f"query.{ext}"
    n = convert_vectors(base, base_dst, dim, data_type, args.base_count or nominal_n)
    nq = convert_vectors(query, query_dst, dim, data_type)
    if n != nominal_n:
        print(f"Using {n} base vectors (dataset label normally denotes {nominal_n})")
    truth = args.groundtruth or source / Path(files["groundtruth"]).name
    gt_dst = target / "gt10.bin"
    if truth and truth.is_file():
        convert_truth(truth, gt_dst, nq)
    else:
        print("Ground truth missing. Run the 'groundtruth' command after building binaries.")
    config_path = args.config or ROOT / "config" / f"bench_{args.dataset}.json"
    existing = json.loads(config_path.read_text()).get(args.dataset, {}) if config_path.exists() else {}
    config = existing.copy() if existing.get("low_client_storage") else json.loads((ROOT / "config" / "config_small.json").read_text())["sift"].copy()
    config.update({
        "base_size": n, "dim": dim, "block_size": 2 + 128 + (dim if data_type == "float" else (dim + 3) // 4),
        "num_levels": max(2, math.ceil(math.log2(2 * n / 32 + 1))), "oram_cached_levels": 6,
        "large_number": 1, "--data_type": data_type, "--dist_fn": metric,
        "-L": 125, "-K": getattr(args, "k", 10),
        "--query_nums": nq, "--num_nodes_to_cache": PAPER_CACHE_NODES[args.dataset],
        "base_path": str(base_dst), "--data_path": str(base_dst),
        "--query_file": str(query_dst), "--gt_file": str(gt_dst),
        "--index_path_prefix": str(target / "index" / "disk_index"),
        "--result_path": str(target / "results" / "oram"),
    })
    for key, filename in {
        "buckets_path": "buckets.bin", "metadata_path": "metadata.bin", "pos_map_path": "position_map.bin",
        "block_map_path": "block_mapping.bin", "hash_path": "hash.bin", "graph_cache_path": "graph_cache.bin",
    }.items():
        config[key] = str(target / "oram" / filename)
    if config.get("low_client_storage"):
        config["oram_cached_levels"] = 0
        config["--num_nodes_to_cache"] = 0
        for key, value in list(config.items()):
            if key.endswith("_path") and key.startswith(("pq_oram_", "posmap_oram_")):
                config[key] = str(target / "oram" / Path(value).name)
    config_path.parent.mkdir(parents=True, exist_ok=True)
    config_path.write_text(json.dumps({args.dataset: config}, indent=2) + "\n")
    print(f"Prepared {n} x {dim} {data_type} base vectors and {nq} queries; config: {config_path}")


def cmd_groundtruth(args):
    _, config = load_config(args)
    binary = binary_path(args, "compute_groundtruth")
    require_file(binary, "compute_groundtruth binary")
    run([binary, "--data_type", config["--data_type"], "--dist_fn", config["--dist_fn"],
         "--base_file", config["--data_path"], "--query_file", config["--query_file"],
         "--gt_file", config["--gt_file"], "--K", str(args.k)])
    require_file(Path(config["--gt_file"]), "computed ground truth")


def cmd_build(args):
    _, config = load_config(args)
    binary = binary_path(args, "build_disk_index")
    require_file(binary, "build_disk_index binary")
    require_file(Path(config["--data_path"]), "base vectors")
    prefix = Path(config["--index_path_prefix"])
    prefix.parent.mkdir(parents=True, exist_ok=True)
    pq_bytes = args.pq_bytes or PAPER_PQ[args.dataset]
    budget = config["base_size"] * pq_bytes / 1e9
    run([binary, "--data_type", config["--data_type"], "--dist_fn", config["--dist_fn"],
         "--data_path", config["--data_path"], "--index_path_prefix", prefix,
         "-R", str(config["R"]), "-L", str(args.build_l or config["-L"]),
         "-B", str(budget), "-M", str(args.ram_gb), "-A", "1.2", "--QD", str(pq_bytes)],
        prefix.parent / "build.log")


def query_subset(config, count, output_dir):
    query_file, gt_file = Path(config["--query_file"]), Path(config["--gt_file"])
    require_file(query_file, "query vectors")
    require_file(gt_file, "ground truth")
    item_size = 1 if config["--data_type"] == "uint8" else 4
    nq = inspect_bin(query_file, config["dim"], item_size)
    gt_width, has_dists = inspect_truth(gt_file, nq)
    if not 1 <= count <= nq:
        raise ValueError(f"--queries must be between 1 and {nq}")
    output_dir.mkdir(parents=True, exist_ok=True)
    result = []
    for src, width, size, label in ((query_file, config["dim"], item_size, "query"), (gt_file, gt_width, 4, "gt")):
        dst = output_dir / f"{label}_{count}.bin"
        with src.open("rb") as source, dst.open("wb") as target:
            source.seek(8)
            target.write(struct.pack("<II", count, width))
            remaining = count * width * size
            while remaining:
                chunk = source.read(min(remaining, 1024 * 1024))
                if not chunk:
                    raise ValueError(f"truncated {src}")
                target.write(chunk)
                remaining -= len(chunk)
            if label == "gt" and has_dists:
                source.seek(8 + nq * width * 4)
                remaining = count * width * 4
                while remaining:
                    chunk = source.read(min(remaining, 1024 * 1024))
                    if not chunk:
                        raise ValueError(f"truncated {src}")
                    target.write(chunk)
                    remaining -= len(chunk)
        result.append(dst)
    return result


def pairs(args):
    if not args.t or not args.w:
        raise ValueError("plain/client require both --t and --w")
    ts = [int(value) for value in args.t.split(",")]
    ws = [int(value) for value in args.w.split(",")]
    if any(x < 1 for x in ts + ws):
        raise ValueError("T and W must be positive")
    if args.pairing == "zip":
        if len(ts) != len(ws):
            raise ValueError("zip pairing needs equally many T and W values")
        return list(zip(ts, ws))
    return [(t, w) for t in ts for w in ws]


def cmd_plain(args):
    _, config = load_config(args)
    binary = binary_path(args, "search_disk_index")
    require_file(binary, "search_disk_index binary")
    require_file(Path(config["--index_path_prefix"] + "_disk.index"), "DiskANN index")
    require_truth_width(config, args.k)
    dataset_results = dataset_dir(args.dataset, args.data_root) / "results"
    result_dir = dataset_results / "plain"
    query, gt = query_subset(config, args.queries, result_dir)
    rows = []
    for t, w in pairs(args):
        prefix = result_dir / f"T{t}_W{w}_Q{args.queries}"
        command = [binary, "--data_type", config["--data_type"], "--dist_fn", config["--dist_fn"],
                   "--index_path_prefix", config["--index_path_prefix"], "--query_file", query,
                   "--gt_file", gt, "--result_path", prefix, "--num_nodes_to_cache", str(args.cache_nodes or 0),
                   "-T", str(args.threads), "-L", str(args.l or config["-L"]), "-W", str(w),
                   "--search_io_limit", str(t), "-K", str(args.k)]
        output = run(command, prefix.with_suffix(".log"))
        recall = recall_pattern(args.k).search(output)
        latency = PAIR_LATENCY.search(output)
        if not recall or not latency:
            raise RuntimeError(f"Recall@{args.k} or Mean Latency missing from {prefix.with_suffix('.log')}")
        row = result_row(dataset=args.dataset, mode="plain", t=t, w=w,
                         l=args.l or config["-L"], queries=args.queries,
                         recall=float(recall.group(1)), measured_ms=float(latency.group(1)) / 1000,
                         measured_network_ms=0, bytes_per_query=0,
                         direction_changes_per_query=0, threads=args.threads,
                         cache_nodes=args.cache_nodes or 0, k=args.k)
        rows.append(row)
        print(row)
    csv_path = getattr(args, "csv", None) or (dataset_results / f"{args.dataset}.csv")
    print(f"CSV: {write_dataset_rows(dataset_results, args.dataset, rows, path=csv_path)}")


def cmd_oram_init(args):
    config_path, config = load_config(args)
    require_file(Path(config["--index_path_prefix"] + "_disk.index"), "DiskANN index")
    Path(config["buckets_path"]).parent.mkdir(parents=True, exist_ok=True)
    run([binary_path(args, "oram_initializer"), f"d={args.dataset}", f"c={config_path}", "type=2"])
    if config.get("low_client_storage"):
        run([binary_path(args, "oram_initializer_low"), f"d={args.dataset}", f"c={config_path}"])


def cmd_server(args):
    config_path, config = load_config(args)
    role = getattr(args, "role", "graph")
    key = {"graph": "buckets_path", "pq": "pq_oram_buckets_path", "posmap": "posmap_oram_buckets_path"}[role]
    if role != "graph" and not config.get("low_client_storage"):
        raise ValueError("auxiliary roles require low_client_storage")
    if role == "posmap" and not config.get("recursive_posmap"):
        raise ValueError("posmap role requires recursive_posmap")
    require_file(Path(config[key]), "ORAM buckets")
    run([binary_path(args, "server"), f"d={args.dataset}", f"c={config_path}", f"p={args.port}",
         f"m={int(args.memory)}", "t=2", f"io={args.io_events}", f"dp={config[key]}", f"role={role}"])


def cmd_client(args):
    config_path, config = load_config(args)
    all_pairs = pairs(args)
    if args.queries > inspect_bin(Path(config["--query_file"]), config["dim"], 1 if config["--data_type"] == "uint8" else 4):
        raise ValueError("--queries exceeds query count")
    require_truth_width(config, args.k)
    low_client = bool(config.get("low_client_storage", False))
    if low_client:
        cache_nodes = 0
    elif args.cache_nodes is not None:
        cache_nodes = args.cache_nodes
    else:
        cache_nodes = int(config.get("--num_nodes_to_cache", PAPER_CACHE_NODES[args.dataset]))
    dataset_results = dataset_dir(args.dataset, args.data_root) / "results"
    mode = "oram_low" if low_client else "oram"
    cached_levels = 0 if low_client else args.cached_levels
    result_dir = dataset_results / mode
    result_dir.mkdir(parents=True, exist_ok=True)
    private_config = json.loads(config_path.read_text())
    private_config[args.dataset].update(config)
    private_config[args.dataset]["--query_nums"] = args.queries
    private_config[args.dataset]["-L"] = args.l or config["-L"]
    private_config[args.dataset]["-K"] = args.k
    private_config[args.dataset]["-T"] = args.threads
    private_config[args.dataset]["--num_nodes_to_cache"] = cache_nodes
    private_config[args.dataset]["--result_path"] = str(result_dir / f"Q{args.queries}")
    run_config = result_dir / f"client_Q{args.queries}.json"
    run_config.write_text(json.dumps(private_config, indent=2) + "\n")
    output = run([binary_path(args, "client"), f"d={args.dataset}", f"c={run_config}",
         f"sip={args.server_ip}", f"p={args.port}",
         "i_list=" + ",".join(str(t) for t, _ in all_pairs),
         "w_list=" + ",".join(str(w) for _, w in all_pairs),
         f"ocl={cached_levels}", f"nc={cache_nodes}", f"mal={int(args.integrity)}"],
        result_dir / f"client_Q{args.queries}.log")
    recalls, latencies, network_times, counters = parse_client_results(output, len(all_pairs), args.queries, args.k)
    rows = []
    for index, (t, w) in enumerate(all_pairs):
        total_bytes, changes, _ = counters[index]
        rows.append(result_row(dataset=args.dataset, mode=mode, t=t, w=w,
                               l=args.l or config["-L"], queries=args.queries,
                               recall=recalls[index], measured_ms=latencies[index],
                               measured_network_ms=network_times[index],
                               bytes_per_query=total_bytes / args.queries,
                               direction_changes_per_query=changes / args.queries,
                               threads=args.threads,
                               cache_nodes=cache_nodes, cached_levels=cached_levels,
                               integrity=int(args.integrity), k=args.k))
    csv_path = getattr(args, "csv", None) or (dataset_results / f"{args.dataset}.csv")
    print(f"CSV: {write_dataset_rows(dataset_results, args.dataset, rows, path=csv_path)}")
    for row in rows:
        print(row)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", choices=SPECS)
    parser.add_argument("command", choices=("prepare", "groundtruth", "build", "plain", "oram-init", "server", "client"))
    parser.add_argument("--data-root", type=path_arg, default=REPOSITORY / "data/prepared/bison")
    parser.add_argument("--build-dir", type=path_arg, default=ROOT / "build/bin", help="directory containing BiSON binaries")
    parser.add_argument("--csv", type=path_arg, help="output summary CSV path; default data/<dataset>/results/<dataset>.csv")
    parser.add_argument("--config", type=path_arg)
    parser.add_argument("--role", choices=("graph", "pq", "posmap"), default="graph", help="server ORAM role; set its port explicitly")
    parser.add_argument("--source", type=path_arg)
    parser.add_argument("--base", type=path_arg)
    parser.add_argument("--query", type=path_arg)
    parser.add_argument("--groundtruth", type=path_arg)
    parser.add_argument("--base-count", type=int, help="base prefix length; default dataset size")
    parser.add_argument("--queries", type=int, default=100)
    parser.add_argument("--k", type=int, default=10, help="number of search results to return (top-K)")
    parser.add_argument("--t", help="comma separated search iterations / IO limits")
    parser.add_argument("--w", help="comma separated beam widths")
    parser.add_argument("--pairing", choices=("zip", "product"), default="zip")
    parser.add_argument("--l", type=int, help="search list size; default 125")
    parser.add_argument("--threads", type=int, default=16, help="search CPU threads (plain -T, client num_threads)")
    parser.add_argument("--cache-nodes", type=int, default=None,
                        help="graph cache nodes; native ORAM defaults to the paper value, low-client forces 0")
    parser.add_argument("--cached-levels", type=int, default=6)
    parser.add_argument("--pq-bytes", type=int)
    parser.add_argument("--ram-gb", type=float, default=8)
    parser.add_argument("--build-l", type=int)
    parser.add_argument("--server-ip", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--io-events", type=int, default=1024)
    parser.add_argument("--memory", action="store_true", help="keep server ORAM buckets in RAM")
    parser.add_argument("--integrity", action="store_true", help="enable malicious security")
    args = parser.parse_args()
    try:
        {"prepare": cmd_prepare, "groundtruth": cmd_groundtruth, "build": cmd_build,
         "plain": cmd_plain, "oram-init": cmd_oram_init, "server": cmd_server, "client": cmd_client}[args.command](args)
    except (ValueError, FileNotFoundError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
