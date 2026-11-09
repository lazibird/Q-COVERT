#!/usr/bin/env python3

import argparse
import os
import shutil
import struct
import sys
import time
from pathlib import Path


DEFAULT_SUBSETS = (
    ("sift10M", 10_000_000),
    ("sift100M", 100_000_000),
)


def format_count(value):
    if value % 1_000_000 == 0:
        return f"{value // 1_000_000}M"
    if value % 1_000 == 0:
        return f"{value // 1_000}K"
    return str(value)


def parse_count(text):
    value = text.strip().lower().replace("_", "")
    if not value:
        raise ValueError("empty count")
    multiplier = 1
    if value.endswith("m"):
        multiplier = 1_000_000
        value = value[:-1]
    elif value.endswith("k"):
        multiplier = 1_000
        value = value[:-1]
    if not value.isdigit():
        raise ValueError(f"invalid count: {text}")
    return int(value) * multiplier


def parse_subset(text):
    if ":" not in text:
        count = parse_count(text)
        return f"sift{format_count(count)}", count
    name, count_text = text.split(":", 1)
    name = name.strip()
    if not name:
        raise ValueError(f"invalid subset name in: {text}")
    return name, parse_count(count_text)


def log(message):
    print(f"[status] {message}", flush=True)


class Progress:
    def __init__(self, label, total, unit="item", min_interval=2.0):
        self.label = label
        self.total = max(0, int(total))
        self.unit = unit
        self.min_interval = min_interval
        self.start_time = time.time()
        self.last_time = 0.0
        self.last_width = 0

    def _format_amount(self, value):
        if self.unit == "bytes":
            return f"{value / (1024 ** 3):.2f} GiB"
        return f"{value:,} {self.unit}"

    def update(self, done, force=False):
        done = min(max(0, int(done)), self.total)
        now = time.time()
        if not force and done < self.total and now - self.last_time < self.min_interval:
            return
        elapsed = now - self.start_time
        rate = done / elapsed if elapsed > 0 and done > 0 else 0.0
        eta = (self.total - done) / rate if rate > 0 and done < self.total else 0.0
        percent = (100.0 * done / self.total) if self.total else 100.0
        line = (
            f"[status] {self.label}: {self._format_amount(done)} / "
            f"{self._format_amount(self.total)} ({percent:5.1f}%) "
            f"elapsed {format_seconds(elapsed)} eta {format_seconds(eta)}"
        )
        padding = " " * max(0, self.last_width - len(line))
        sys.stdout.write("\r" + line + padding)
        sys.stdout.flush()
        self.last_time = now
        self.last_width = len(line)

    def done(self):
        self.update(self.total, force=True)
        sys.stdout.write("\n")
        sys.stdout.flush()


def format_seconds(seconds):
    seconds = int(max(0.0, seconds) + 0.5)
    hours = seconds // 3600
    minutes = (seconds % 3600) // 60
    secs = seconds % 60
    if hours:
        return f"{hours}h{minutes:02d}m{secs:02d}s"
    if minutes:
        return f"{minutes}m{secs:02d}s"
    return f"{secs}s"


def read_u8bin_header(path):
    with open(path, "rb") as fp:
        header = fp.read(8)
    if len(header) != 8:
        raise ValueError(f"invalid uint8 bin header: {path}")
    rows, dim = struct.unpack("<II", header)
    if rows == 0 or dim == 0:
        raise ValueError(f"invalid uint8 bin shape in {path}: {rows} x {dim}")
    expected = 8 + int(rows) * int(dim)
    actual = os.path.getsize(path)
    if actual != expected:
        raise ValueError(f"uint8 bin size mismatch for {path}: expected {expected}, got {actual}")
    return int(rows), int(dim)


def write_u8bin_header(fp, rows, dim):
    fp.write(struct.pack("<II", int(rows), int(dim)))


def copy_exact_bytes(src_fp, dst_fp, byte_count, label, io_chunk_bytes):
    progress = Progress(label, byte_count, unit="bytes")
    remaining = int(byte_count)
    done = 0
    while remaining > 0:
        chunk = src_fp.read(min(io_chunk_bytes, remaining))
        if not chunk:
            raise IOError("source ended before expected byte count")
        dst_fp.write(chunk)
        done += len(chunk)
        remaining -= len(chunk)
        progress.update(done)
    progress.done()


def truncate_base(source_base, output_base, target_rows, dim, overwrite, io_chunk_bytes):
    if output_base.exists() and not overwrite:
        log(f"base exists; skipping: {output_base}")
        return
    output_base.parent.mkdir(parents=True, exist_ok=True)
    total_bytes = int(target_rows) * int(dim)
    log(f"writing base subset: {output_base} rows={target_rows:,} dim={dim}")
    tmp = output_base.with_suffix(output_base.suffix + ".tmp")
    with open(source_base, "rb") as src, open(tmp, "wb") as dst:
        src.seek(8)
        write_u8bin_header(dst, target_rows, dim)
        copy_exact_bytes(src, dst, total_bytes, f"copying {output_base.parent.name}/base.bin", io_chunk_bytes)
    os.replace(tmp, output_base)


def copy_query(source_query, output_query, overwrite):
    if output_query.exists() and not overwrite:
        log(f"query exists; skipping: {output_query}")
        return
    output_query.parent.mkdir(parents=True, exist_ok=True)
    tmp = output_query.with_suffix(output_query.suffix + ".tmp")
    shutil.copyfile(source_query, tmp)
    os.replace(tmp, output_query)
    log(f"copied query: {output_query}")


def write_gt_header(fp, rows, k):
    fp.write(struct.pack("<II", int(rows), int(k)))


def load_queries_float32(query_path):
    import numpy as np

    rows, dim = read_u8bin_header(query_path)
    raw = np.memmap(query_path, dtype=np.uint8, mode="r", offset=8, shape=(rows, dim))
    return np.asarray(raw, dtype=np.float32)


def require_faiss():
    try:
        import faiss
    except ImportError as exc:
        raise RuntimeError(
            "faiss is required to recompute ground truth. Install it with: "
            "pip install numpy faiss-cpu"
        ) from exc
    return faiss


def compute_groundtruth_for_current_index(index, queries, output_gt, k, query_batch, overwrite):
    import numpy as np

    if output_gt.exists() and not overwrite:
        log(f"groundtruth exists; skipping: {output_gt}")
        return
    if index.ntotal < k:
        raise ValueError(f"cannot compute top-{k} with only {index.ntotal} base vectors")

    output_gt.parent.mkdir(parents=True, exist_ok=True)
    tmp = output_gt.with_suffix(output_gt.suffix + ".tmp")
    log(f"computing exact groundtruth: {output_gt} ntotal={index.ntotal:,} k={k}")
    progress = Progress(f"searching {output_gt.parent.name}/gt.bin", queries.shape[0], unit="queries")
    with open(tmp, "wb") as fp:
        write_gt_header(fp, queries.shape[0], k)
        done = 0
        for start in range(0, queries.shape[0], query_batch):
            end = min(start + query_batch, queries.shape[0])
            _, labels = index.search(np.ascontiguousarray(queries[start:end]), k)
            labels = labels.astype(np.uint32, copy=False)
            fp.write(labels.tobytes())
            done = end
            progress.update(done)
    progress.done()
    os.replace(tmp, output_gt)


def build_faiss_and_write_groundtruth(
        source_base,
        source_query,
        outputs,
        k,
        add_batch,
        query_batch,
        faiss_threads,
        overwrite):
    import numpy as np

    faiss = require_faiss()
    if faiss_threads > 0 and hasattr(faiss, "omp_set_num_threads"):
        faiss.omp_set_num_threads(int(faiss_threads))
        log(f"faiss threads: {faiss_threads}")

    source_rows, dim = read_u8bin_header(source_base)
    pending = [
        (rows, out_dir / "gt.bin")
        for rows, out_dir in sorted(outputs, key=lambda item: item[0])
        if overwrite or not (out_dir / "gt.bin").exists()
    ]
    if not pending:
        log("all groundtruth files exist; skipping FAISS search")
        return

    max_rows = max(rows for rows, _ in pending)
    if max_rows > source_rows:
        raise ValueError(f"requested {max_rows:,} rows but source only has {source_rows:,}")

    queries = load_queries_float32(source_query)
    if queries.shape[1] != dim:
        raise ValueError(f"query dim {queries.shape[1]} does not match base dim {dim}")

    estimated_index_gib = max_rows * dim * 4 / (1024 ** 3)
    log(
        f"building FAISS IndexFlatL2 incrementally up to {max_rows:,} vectors; "
        f"index memory about {estimated_index_gib:.2f} GiB"
    )
    index = faiss.IndexFlatL2(dim)
    base = np.memmap(source_base, dtype=np.uint8, mode="r", offset=8, shape=(source_rows, dim))

    next_target_idx = 0
    added = 0
    while added < max_rows:
        next_target = pending[next_target_idx][0]
        end = min(added + add_batch, max_rows, next_target)
        xb = np.asarray(base[added:end], dtype=np.float32)
        index.add(np.ascontiguousarray(xb))
        added = end
        log(f"FAISS index added {added:,} / {max_rows:,} vectors")

        while next_target_idx < len(pending) and added >= pending[next_target_idx][0]:
            _, output_gt = pending[next_target_idx]
            compute_groundtruth_for_current_index(index, queries, output_gt, k, query_batch, overwrite)
            next_target_idx += 1
            if next_target_idx >= len(pending):
                break


def parse_args():
    parser = argparse.ArgumentParser(
        description="Create sift10M/sift100M BigANN subsets from the prefix of sift1B and recompute exact FAISS ground truth."
    )
    parser.add_argument("--source", default="data/sift1B", help="directory containing sift1B base.bin and query.bin")
    parser.add_argument("--output-root", default="", help="directory where sift10M/sift100M folders are written; default is source parent")
    parser.add_argument(
        "--subsets",
        nargs="+",
        default=[f"{name}:{rows}" for name, rows in DEFAULT_SUBSETS],
        help="subset specs as name:count or count, e.g. sift10M:10M sift100M:100M",
    )
    parser.add_argument("--k", type=int, default=100, help="number of exact neighbors to write to gt.bin")
    parser.add_argument("--add-batch", type=int, default=1_000_000, help="base vectors converted and added to FAISS per batch")
    parser.add_argument("--query-batch", type=int, default=100, help="queries searched per FAISS batch")
    parser.add_argument("--io-chunk-mib", type=int, default=256, help="file copy chunk size in MiB")
    parser.add_argument("--faiss-threads", type=int, default=0, help="FAISS OpenMP thread count; 0 keeps FAISS default")
    parser.add_argument("--overwrite", action="store_true", help="overwrite existing base/query/gt files")
    parser.add_argument("--skip-groundtruth", action="store_true", help="only write base.bin and query.bin")
    return parser.parse_args()


def main():
    args = parse_args()
    source = Path(args.source)
    output_root = Path(args.output_root) if args.output_root else source.parent
    source_base = source / "base.bin"
    source_query = source / "query.bin"
    if not source_base.exists():
        raise FileNotFoundError(f"missing source base: {source_base}")
    if not source_query.exists():
        raise FileNotFoundError(f"missing source query: {source_query}")
    if args.k <= 0 or args.add_batch <= 0 or args.query_batch <= 0 or args.io_chunk_mib <= 0:
        raise ValueError("k, add-batch, query-batch, and io-chunk-mib must be positive")

    source_rows, dim = read_u8bin_header(source_base)
    query_rows, query_dim = read_u8bin_header(source_query)
    if query_dim != dim:
        raise ValueError(f"query dim {query_dim} does not match base dim {dim}")

    subsets = [parse_subset(text) for text in args.subsets]
    seen_names = set()
    for name, rows in subsets:
        if name in seen_names:
            raise ValueError(f"duplicate subset name: {name}")
        seen_names.add(name)
        if rows <= 0:
            raise ValueError(f"subset {name} has non-positive row count")
        if rows > source_rows:
            raise ValueError(f"subset {name} requests {rows:,} rows but source only has {source_rows:,}")
        if args.k > rows:
            raise ValueError(f"subset {name} has fewer rows than k={args.k}")

    log(f"source base: {source_base} rows={source_rows:,} dim={dim}")
    log(f"source query: {source_query} rows={query_rows:,} dim={query_dim}")
    io_chunk_bytes = int(args.io_chunk_mib) * 1024 * 1024

    outputs = []
    for name, rows in subsets:
        out_dir = output_root / name
        outputs.append((rows, out_dir))
        truncate_base(source_base, out_dir / "base.bin", rows, dim, args.overwrite, io_chunk_bytes)
        copy_query(source_query, out_dir / "query.bin", args.overwrite)

    if not args.skip_groundtruth:
        build_faiss_and_write_groundtruth(
            source_base=source_base,
            source_query=source_query,
            outputs=outputs,
            k=args.k,
            add_batch=args.add_batch,
            query_batch=args.query_batch,
            faiss_threads=args.faiss_threads,
            overwrite=args.overwrite,
        )

    log("done")


if __name__ == "__main__":
    main()
