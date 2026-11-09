#!/usr/bin/env python3
"""Build a Faiss HNSW index for BigANN-style or fvecs datasets.

The default parameters match the prebuilt Compass SIFT graph index:
M=64, efConstruction=80, efSearch=16, upper_beam=1, metric=L2.

Example:
  ./.venv-faiss/bin/python script/build_bigann_hnsw.py sift10M
  ./.venv-faiss/bin/python script/build_bigann_hnsw.py sift100M --batch-size 200000
  ./.venv-faiss/bin/python script/build_bigann_hnsw.py deep1M
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Optional, Tuple


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def resolve_config_path(repo: Path, rel_path: str) -> Path:
    return (repo.parent / rel_path).resolve()


def hnsw_params_from_name(path: Path) -> Tuple[Optional[int], Optional[int]]:
    match = re.search(r"hnsw_(\d+)_(\d+)_", path.name)
    if not match:
        return None, None
    return int(match.group(1)), int(match.group(2))


def load_dataset_config(repo: Path, dataset: str, config_path: Optional[Path] = None) -> dict:
    cfg_path = config_path or repo / "tests" / "config" / "config_ring.json"
    with cfg_path.open() as in_file:
        cfg = json.load(in_file)
    if dataset not in cfg:
        raise SystemExit(f"dataset '{dataset}' not found in {cfg_path}")
    return cfg[dataset]


def human_size(size: int) -> str:
    value = float(size)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < 1024.0 or unit == "TiB":
            return f"{value:.2f} {unit}"
        value /= 1024.0
    return f"{value:.2f} TiB"


def read_bigann_header(path: Path) -> Tuple[int, int, str, int]:
    if path.suffix == ".fvecs":
        with path.open("rb") as in_file:
            header = in_file.read(4)
        if len(header) != 4:
            raise SystemExit(f"{path} is too small to contain an fvecs header")
        d = int.from_bytes(header, "little", signed=True)
        if d <= 0 or d >= 1000000:
            raise SystemExit(f"{path} has invalid fvecs dimension: d={d}")
        row_bytes = (d + 1) * 4
        size = path.stat().st_size
        if size % row_bytes != 0:
            raise SystemExit(
                f"{path} has invalid fvecs size {size}; row size for d={d} is {row_bytes}"
            )
        n = size // row_bytes
        return n, d, "fvecs", n

    with path.open("rb") as in_file:
        header = in_file.read(8)
    if len(header) != 8:
        raise SystemExit(f"{path} is too small to contain a BigANN header")

    n = int.from_bytes(header[:4], "little", signed=False)
    d = int.from_bytes(header[4:], "little", signed=False)
    if n <= 0 or d <= 0:
        raise SystemExit(f"{path} has invalid BigANN header: n={n} d={d}")

    payload = path.stat().st_size - 8
    count = n * d
    if payload == count:
        return n, d, "uint8", n
    if payload == count * 4:
        return n, d, "float32", n
    if payload % d == 0:
        return n, d, "uint8", payload // d
    if payload % (d * 4) == 0:
        return n, d, "float32", payload // (d * 4)
    raise SystemExit(
        f"{path} has unsupported payload size {payload}; "
        f"expected {count} bytes for uint8 or {count * 4} bytes for float32"
    )


def iter_bigann_batches(np, path: Path, available_n: int, n: int, d: int, dtype_name: str, batch_size: int):
    if dtype_name == "fvecs":
        mmap = np.memmap(path, dtype=np.float32, mode="r", shape=(available_n, d + 1))
        for start in range(0, n, batch_size):
            end = min(start + batch_size, n)
            yield start, end, np.ascontiguousarray(mmap[start:end, 1:], dtype=np.float32)
        return

    dtype = np.uint8 if dtype_name == "uint8" else np.float32
    mmap = np.memmap(path, dtype=dtype, mode="r", offset=8, shape=(available_n, d))
    for start in range(0, n, batch_size):
        end = min(start + batch_size, n)
        yield start, end, np.ascontiguousarray(mmap[start:end], dtype=np.float32)


def faiss_metric(faiss, metric: str):
    if metric == "l2":
        return faiss.METRIC_L2
    if metric == "ip":
        return faiss.METRIC_INNER_PRODUCT
    raise ValueError(metric)


def normalize_l2_np(np, xb):
    if not xb.flags.writeable:
        xb = xb.copy()
    norms = np.linalg.norm(xb, axis=1, keepdims=True)
    norms[norms == 0.0] = 1.0
    xb /= norms
    return xb


def build_index(args: argparse.Namespace) -> None:
    try:
        import numpy as np
    except ModuleNotFoundError as exc:
        raise SystemExit("Missing dependency: numpy. Install it with faiss-cpu.") from exc

    try:
        import faiss
    except ModuleNotFoundError as exc:
        raise SystemExit(
            "Missing dependency: faiss. Run:\n"
            "  ./script/setup_faiss_env.sh\n"
            "  ./.venv-faiss/bin/python script/build_bigann_hnsw.py sift10M"
        ) from exc

    base_path = Path(args.base).resolve()
    output_path = Path(args.output).resolve()
    if not base_path.is_file():
        raise SystemExit(f"base file does not exist: {base_path}")
    if output_path.exists() and not args.force:
        raise SystemExit(f"output already exists: {output_path} (use --force to overwrite)")

    header_n, d, dtype_name, available_n = read_bigann_header(base_path)
    if args.expected_d is not None and d != args.expected_d:
        raise SystemExit(f"dimension mismatch: file has {d}, expected {args.expected_d}")
    n = args.expected_n if args.expected_n is not None else available_n
    if n > available_n:
        raise SystemExit(f"base file has only {available_n} available rows, expected {n}")
    if args.expected_n is not None and header_n != args.expected_n:
        print(
            f"Warning: BigANN header says n={header_n}, config base_size={args.expected_n}; "
            f"using the first {n} available rows."
        )

    if args.threads > 0:
        faiss.omp_set_num_threads(args.threads)

    metric = faiss_metric(faiss, args.metric)
    try:
        index = faiss.IndexHNSWFlat(d, args.m, metric)
    except TypeError:
        if args.metric != "l2":
            raise SystemExit("This Faiss build does not support metric in IndexHNSWFlat constructor")
        index = faiss.IndexHNSWFlat(d, args.m)

    index.hnsw.efConstruction = args.ef_construction
    index.hnsw.efSearch = args.ef_search
    if hasattr(index.hnsw, "upper_beam"):
        index.hnsw.upper_beam = args.upper_beam
    elif args.upper_beam != 1:
        raise SystemExit("This Faiss Python module does not expose hnsw.upper_beam")

    if args.normalize:
        if args.metric != "ip":
            raise SystemExit("--normalize is only useful with --metric ip")
        print("Normalizing each batch before adding to IP index.")

    print(f"Base: {base_path}")
    print(f"Output: {output_path}")
    print(f"Vectors: header_n={header_n} available_n={available_n} using_n={n} d={d} dtype={dtype_name}")
    print(
        "HNSW: "
        f"M={args.m} efConstruction={args.ef_construction} "
        f"efSearch={args.ef_search} upper_beam={args.upper_beam} metric={args.metric}"
    )
    print(f"Batch size: {args.batch_size}")
    print(
        "Estimated flat vector storage inside index: "
        f"{human_size(n * d * 4)} (plus HNSW graph memory)"
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    added = 0
    for start, end, xb in iter_bigann_batches(np, base_path, available_n, n, d, dtype_name, args.batch_size):
        if args.normalize:
            xb = normalize_l2_np(np, xb)
        index.add(xb)
        added = end
        elapsed = time.time() - t0
        rate = added / elapsed if elapsed > 0 else 0.0
        print(
            f"[{elapsed:.1f}s] added {added}/{n} "
            f"({100.0 * added / n:.2f}%, {rate:.1f} vec/s)",
            flush=True,
        )

    tmp_path = output_path.with_suffix(output_path.suffix + ".tmp")
    if tmp_path.exists():
        tmp_path.unlink()
    print(f"Writing temporary index: {tmp_path}")
    faiss.write_index(index, str(tmp_path))

    print("Verifying written index header with Faiss...")
    verify = faiss.read_index(str(tmp_path))
    if verify.d != d or verify.ntotal != n:
        raise SystemExit(
            f"written index mismatch: d={verify.d}, ntotal={verify.ntotal}; expected d={d}, ntotal={n}"
        )

    os.replace(tmp_path, output_path)
    print(f"Done: {output_path}")


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Build hnsw_64_80_2_ip.index for configured BigANN or fvecs data."
    )
    parser.add_argument(
        "dataset",
        nargs="?",
        default="sift10M",
        help="Dataset key in tests/config/config_ring.json (default: sift10M).",
    )
    parser.add_argument("--config", type=Path, help="Override dataset configuration JSON.")
    parser.add_argument("--base", help="Override base vector path.")
    parser.add_argument("--output", help="Override output index path.")
    parser.add_argument(
        "--metric",
        choices=("l2", "ip"),
        default="l2",
        help="Faiss metric. Default is l2 to match the existing SIFT index header.",
    )
    parser.add_argument("--m", type=int, help="HNSW M. Default is inferred from output name, then config M.")
    parser.add_argument(
        "--ef-construction",
        type=int,
        help="HNSW efConstruction. Default is inferred from output name, then 80.",
    )
    parser.add_argument("--ef-search", type=int, default=16, help="Stored efSearch value (default: 16).")
    parser.add_argument("--upper-beam", type=int, default=1, help="Stored hnsw.upper_beam (default: 1).")
    parser.add_argument("--batch-size", type=int, default=100000, help="Vectors per add() batch.")
    parser.add_argument("--threads", type=int, default=0, help="Faiss OpenMP threads; 0 keeps Faiss default.")
    parser.add_argument("--normalize", action="store_true", help="Normalize vectors before adding; use with --metric ip.")
    parser.add_argument("--force", action="store_true", help="Overwrite an existing output file.")
    return parser


def main() -> int:
    parser = make_parser()
    args = parser.parse_args()

    repo = repo_root()
    cfg = load_dataset_config(repo, args.dataset, args.config)

    base_path = Path(args.base).resolve() if args.base else resolve_config_path(repo, cfg["base_path"])
    output_path = Path(args.output).resolve() if args.output else resolve_config_path(repo, cfg["index_path"])
    name_m, name_ef = hnsw_params_from_name(output_path)

    args.base = str(base_path)
    args.output = str(output_path)
    args.m = args.m if args.m is not None else (name_m if name_m is not None else int(cfg["M"]))
    args.ef_construction = (
        args.ef_construction
        if args.ef_construction is not None
        else (name_ef if name_ef is not None else 80)
    )
    args.expected_n = int(cfg["base_size"])
    args.expected_d = int(cfg["dim"])

    if args.batch_size <= 0:
        raise SystemExit("--batch-size must be positive")
    if args.m <= 0:
        raise SystemExit("--m must be positive")
    if args.ef_construction <= 0:
        raise SystemExit("--ef-construction must be positive")

    build_index(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
