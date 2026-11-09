#!/usr/bin/env python3
"""Build a Faiss IndexPQ file for BigANN-style or fvecs datasets.

The default output matches the prebuilt Compass SIFT PQ format:
IndexPQ magic=IxPq, d=128, M=8, nbits=8, metric=L2, codes=ntotal*M.

Example:
  ./.venv-faiss/bin/python script/build_bigann_pq.py sift10M
  ./.venv-faiss/bin/python script/build_bigann_pq.py sift100M --train-size 1000000
  ./.venv-faiss/bin/python script/build_bigann_pq.py deep1M
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Optional


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def resolve_config_path(repo: Path, rel_path: str) -> Path:
    return (repo.parent / rel_path).resolve()


def load_dataset_config(repo: Path, dataset: str, config_path: Optional[Path] = None) -> dict:
    cfg_path = config_path or repo / "tests" / "config" / "config_ring.json"
    with cfg_path.open() as in_file:
        cfg = json.load(in_file)
    if dataset not in cfg:
        raise SystemExit(f"dataset '{dataset}' not found in {cfg_path}")
    return cfg[dataset]


def pq_m_from_name(path: Path) -> Optional[int]:
    match = re.search(r"pq_full_(\d+)", path.name)
    if not match:
        return None
    return int(match.group(1))


def human_size(size: int) -> str:
    value = float(size)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < 1024.0 or unit == "TiB":
            return f"{value:.2f} {unit}"
        value /= 1024.0
    return f"{value:.2f} TiB"


def read_bigann_header(path: Path):
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


def bigann_memmap(np, path: Path, n: int, d: int, dtype_name: str):
    if dtype_name == "fvecs":
        return np.memmap(path, dtype=np.float32, mode="r", shape=(n, d + 1))

    dtype = np.uint8 if dtype_name == "uint8" else np.float32
    return np.memmap(path, dtype=dtype, mode="r", offset=8, shape=(n, d))


def as_float32_rows(np, mmap, rows, dtype_name: str):
    if dtype_name == "fvecs":
        return np.ascontiguousarray(mmap[rows, 1:], dtype=np.float32)
    return np.ascontiguousarray(mmap[rows], dtype=np.float32)


def iter_bigann_batches(np, mmap, n: int, batch_size: int, dtype_name: str):
    for start in range(0, n, batch_size):
        end = min(start + batch_size, n)
        if dtype_name == "fvecs":
            yield start, end, np.ascontiguousarray(mmap[start:end, 1:], dtype=np.float32)
        else:
            yield start, end, np.ascontiguousarray(mmap[start:end], dtype=np.float32)


def faiss_metric(faiss, metric: str):
    if metric == "l2":
        return faiss.METRIC_L2
    if metric == "ip":
        return faiss.METRIC_INNER_PRODUCT
    raise ValueError(metric)


def training_indices(np, n: int, train_size: int, mode: str, seed: int):
    train_size = min(train_size, n)
    if train_size == n:
        return slice(None)
    if mode == "first":
        return slice(0, train_size)
    if mode == "even":
        return np.linspace(0, n - 1, train_size, dtype=np.int64)
    if mode == "random":
        rng = np.random.default_rng(seed)
        return np.sort(rng.integers(0, n, size=train_size, dtype=np.int64))
    raise ValueError(mode)


def build_training_set(np, mmap, n: int, train_size: int, mode: str, seed: int, dtype_name: str):
    idx = training_indices(np, n, train_size, mode, seed)
    return as_float32_rows(np, mmap, idx, dtype_name)


def normalize_l2_np(np, xb):
    if not xb.flags.writeable:
        xb = xb.copy()
    norms = np.linalg.norm(xb, axis=1, keepdims=True)
    norms[norms == 0.0] = 1.0
    xb /= norms
    return xb


def build_pq(args: argparse.Namespace) -> None:
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
            "  ./.venv-faiss/bin/python script/build_bigann_pq.py sift10M"
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
    if d % args.pq_m != 0:
        raise SystemExit(f"dimension {d} must be divisible by pq_m={args.pq_m}")

    if args.threads > 0:
        faiss.omp_set_num_threads(args.threads)

    metric = faiss_metric(faiss, args.metric)
    try:
        index = faiss.IndexPQ(d, args.pq_m, args.nbits, metric)
    except TypeError:
        if args.metric != "l2":
            raise SystemExit("This Faiss build does not support metric in IndexPQ constructor")
        index = faiss.IndexPQ(d, args.pq_m, args.nbits)

    mmap = bigann_memmap(np, base_path, available_n, d, dtype_name)
    train_size = min(args.train_size, n)

    print(f"Base: {base_path}")
    print(f"Output: {output_path}")
    print(f"Vectors: header_n={header_n} available_n={available_n} using_n={n} d={d} dtype={dtype_name}")
    print(
        "PQ: "
        f"M={args.pq_m} nbits={args.nbits} code_size={args.pq_m * args.nbits // 8} "
        f"metric={args.metric}"
    )
    print(
        f"Training: size={train_size} mode={args.train_mode} "
        f"({human_size(train_size * d * 4)} float32 working set)"
    )
    print(f"Batch size: {args.batch_size}")
    print(f"Expected codes size: {human_size(n * args.pq_m * args.nbits // 8)}")

    t0 = time.time()
    xt = build_training_set(np, mmap, n, train_size, args.train_mode, args.seed, dtype_name)
    if args.normalize:
        if args.metric != "ip":
            raise SystemExit("--normalize is only useful with --metric ip")
        print("Normalizing training vectors for IP.")
        xt = normalize_l2_np(np, xt)

    print(f"[{time.time() - t0:.1f}s] Training PQ...")
    index.train(xt)
    del xt

    print(f"[{time.time() - t0:.1f}s] Encoding all base vectors...")
    added = 0
    for start, end, xb in iter_bigann_batches(np, mmap, n, args.batch_size, dtype_name):
        if args.normalize:
            xb = normalize_l2_np(np, xb)
        index.add(xb)
        added = end
        elapsed = time.time() - t0
        rate = added / elapsed if elapsed > 0 else 0.0
        print(
            f"[{elapsed:.1f}s] encoded {added}/{n} "
            f"({100.0 * added / n:.2f}%, {rate:.1f} vec/s)",
            flush=True,
        )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = output_path.with_suffix(output_path.suffix + ".tmp")
    if tmp_path.exists():
        tmp_path.unlink()

    print(f"Writing temporary PQ index: {tmp_path}")
    faiss.write_index(index, str(tmp_path))

    print("Verifying written PQ index with Faiss...")
    verify = faiss.read_index(str(tmp_path))
    if verify.d != d or verify.ntotal != n:
        raise SystemExit(
            f"written index mismatch: d={verify.d}, ntotal={verify.ntotal}; expected d={d}, ntotal={n}"
        )
    if hasattr(verify, "pq") and (verify.pq.M != args.pq_m or verify.pq.nbits != args.nbits):
        raise SystemExit(
            f"written PQ mismatch: M={verify.pq.M}, nbits={verify.pq.nbits}; "
            f"expected M={args.pq_m}, nbits={args.nbits}"
        )

    os.replace(tmp_path, output_path)
    print(f"Done: {output_path}")


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Build pq_full_8.index for configured BigANN or fvecs data."
    )
    parser.add_argument(
        "dataset",
        nargs="?",
        default="sift10M",
        help="Dataset key in tests/config/config_ring.json (default: sift10M).",
    )
    parser.add_argument("--config", type=Path, help="Override dataset configuration JSON.")
    parser.add_argument("--base", help="Override base vector path.")
    parser.add_argument("--output", help="Override output PQ index path.")
    parser.add_argument("--pq-m", type=int, help="PQ subquantizers. Default is inferred from pq_full_<M>.index.")
    parser.add_argument("--nbits", type=int, default=8, help="Bits per PQ subquantizer (default: 8).")
    parser.add_argument(
        "--metric",
        choices=("l2", "ip"),
        default="l2",
        help="Faiss metric. Default is l2 to match the existing SIFT PQ file.",
    )
    parser.add_argument(
        "--train-size",
        type=int,
        default=1000000,
        help="Number of vectors used to train PQ (default: 1,000,000 or all if smaller).",
    )
    parser.add_argument(
        "--train-mode",
        choices=("even", "random", "first"),
        default="even",
        help="How to sample training vectors (default: even).",
    )
    parser.add_argument("--seed", type=int, default=12345, help="Random seed for --train-mode random.")
    parser.add_argument("--batch-size", type=int, default=200000, help="Vectors per add() batch.")
    parser.add_argument("--threads", type=int, default=0, help="Faiss OpenMP threads; 0 keeps Faiss default.")
    parser.add_argument("--normalize", action="store_true", help="Normalize vectors before train/add; use with --metric ip.")
    parser.add_argument("--force", action="store_true", help="Overwrite an existing output file.")
    return parser


def main() -> int:
    parser = make_parser()
    args = parser.parse_args()

    repo = repo_root()
    cfg = load_dataset_config(repo, args.dataset, args.config)

    base_path = Path(args.base).resolve() if args.base else resolve_config_path(repo, cfg["base_path"])
    output_path = Path(args.output).resolve() if args.output else resolve_config_path(repo, cfg["pq_path"])

    args.base = str(base_path)
    args.output = str(output_path)
    args.pq_m = args.pq_m if args.pq_m is not None else pq_m_from_name(output_path)
    if args.pq_m is None:
        raise SystemExit("Could not infer --pq-m from output name; pass --pq-m explicitly.")
    args.expected_n = int(cfg["base_size"])
    args.expected_d = int(cfg["dim"])

    if args.pq_m <= 0:
        raise SystemExit("--pq-m must be positive")
    if args.nbits <= 0:
        raise SystemExit("--nbits must be positive")
    if args.train_size <= 0:
        raise SystemExit("--train-size must be positive")
    if args.batch_size <= 0:
        raise SystemExit("--batch-size must be positive")

    build_pq(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
