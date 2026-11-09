#!/usr/bin/env python3
"""Shared helpers for downloading public Compass GCS data safely."""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
from pathlib import Path
import sys
from dataclasses import dataclass
from typing import Iterable

try:
    from google.cloud import storage
except ModuleNotFoundError:
    storage = None


BUCKET_NAME = "compass_osdi"
DEFAULT_OUTPUT = str(Path(__file__).resolve().parents[2] / "data/compass/upstream")
HASH_CHUNK_SIZE = 8 * 1024 * 1024
FOUR_DATASET_PREFIXES = (
    "dataset/sift/",
    "snap/server/sift/",
    "snap/client/sift/",
    "dataset/laion1m/",
    "snap/server/laion/",
    "snap/client/laion/",
    "dataset/trip_distilbert/",
    "snap/server/trip/",
    "snap/client/trip/",
    "dataset/msmarco_bert/",
    "snap/server/msmarco/",
    "snap/client/msmarco/",
)


@dataclass
class DownloadStats:
    downloaded: int = 0
    resumed: int = 0
    skipped: int = 0
    failed: int = 0
    objects_seen: int = 0

    def add(self, other: "DownloadStats") -> None:
        self.downloaded += other.downloaded
        self.resumed += other.resumed
        self.skipped += other.skipped
        self.failed += other.failed
        self.objects_seen += other.objects_seen


def human_size(size: int | None) -> str:
    if size is None:
        return "unknown"
    value = float(size)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < 1024.0 or unit == "TiB":
            return f"{value:.2f} {unit}"
        value /= 1024.0
    return f"{value:.2f} TiB"


def local_size(path: str) -> int:
    try:
        return os.path.getsize(path)
    except FileNotFoundError:
        return 0


def unique_bad_path(path: str) -> str:
    candidate = path + ".bad"
    idx = 1
    while os.path.exists(candidate):
        candidate = f"{path}.bad.{idx}"
        idx += 1
    return candidate


def blob_size(blob: storage.Blob) -> int | None:
    if blob.size is None:
        blob.reload()
    return blob.size


def blob_md5_b64(blob: storage.Blob) -> str | None:
    md5_hash = getattr(blob, "md5_hash", None)
    if md5_hash is None:
        try:
            blob.reload()
        except Exception:
            pass
        md5_hash = getattr(blob, "md5_hash", None)
    return md5_hash


def file_md5_b64(path: str) -> str:
    digest = hashlib.md5()
    with open(path, "rb") as in_file:
        while True:
            chunk = in_file.read(HASH_CHUNK_SIZE)
            if not chunk:
                break
            digest.update(chunk)
    return base64.b64encode(digest.digest()).decode("ascii")


def download_range_to_file(
    blob: storage.Blob,
    file_obj,
    start: int | None = None,
    end: int | None = None,
) -> None:
    kwargs = {}
    if start is not None:
        kwargs["start"] = start
    if end is not None:
        kwargs["end"] = end

    # checksum=None avoids checksum warnings/errors for ranged downloads.
    try:
        blob.download_to_file(file_obj, checksum=None, **kwargs)
    except TypeError:
        blob.download_to_file(file_obj, **kwargs)


def check_complete_file(
    path: str,
    expected_size: int | None,
    expected_md5: str | None = None,
    verify_hash: bool = True,
) -> tuple[bool, str]:
    if not os.path.exists(path):
        return False, "missing"

    actual_size = local_size(path)
    if expected_size is not None and actual_size != expected_size:
        return (
            False,
            f"size is {human_size(actual_size)}, expected {human_size(expected_size)}",
        )

    if verify_hash and expected_md5:
        actual_md5 = file_md5_b64(path)
        if actual_md5 != expected_md5:
            return False, f"md5 is {actual_md5}, expected {expected_md5}"

    if expected_size is None and not (verify_hash and expected_md5):
        return False, "remote size/hash unavailable"

    return True, "complete"


def complete_file(
    path: str,
    expected_size: int | None,
    expected_md5: str | None = None,
    verify_hash: bool = True,
) -> bool:
    ok, _ = check_complete_file(path, expected_size, expected_md5, verify_hash)
    return ok


def verification_label(verify_hash: bool, expected_md5: str | None) -> str:
    if not verify_hash:
        return "size only, hash check disabled"
    if expected_md5:
        return "size + md5"
    return "size only, remote md5 unavailable"


def move_bad_file(path: str, reason: str) -> str:
    bad_path = unique_bad_path(path)
    print(f"{reason}; renaming to {bad_path} and redownloading.")
    os.replace(path, bad_path)
    return bad_path


def download_full_blob(
    blob: storage.Blob,
    part_path: str,
    dest_path: str,
    expected_size: int | None,
    expected_md5: str | None,
    verify_hash: bool,
) -> None:
    if os.path.exists(part_path):
        move_bad_file(part_path, f"{blob.name}.part exists before full download")

    print(
        f"Downloading {blob.name} ({human_size(expected_size)}, "
        f"{verification_label(verify_hash, expected_md5)})..."
    )
    with open(part_path, "wb") as out:
        download_range_to_file(blob, out)

    ok, reason = check_complete_file(
        part_path, expected_size, expected_md5, verify_hash=verify_hash
    )
    if not ok:
        bad_path = unique_bad_path(part_path)
        os.replace(part_path, bad_path)
        raise RuntimeError(
            f"{blob.name}: downloaded file failed verification ({reason}); "
            f"saved as {bad_path}"
        )

    os.replace(part_path, dest_path)


def download_blob(
    blob: storage.Blob,
    destination_root: str,
    resume: bool = True,
    verify_hash: bool = True,
) -> str:
    expected_size = blob_size(blob)
    expected_md5 = blob_md5_b64(blob) if verify_hash else None
    dest_path = os.path.join(destination_root, blob.name)
    part_path = dest_path + ".part"
    os.makedirs(os.path.dirname(dest_path), exist_ok=True)

    if complete_file(dest_path, expected_size, expected_md5, verify_hash=verify_hash):
        print(
            f"{blob.name} complete ({human_size(expected_size)}, "
            f"{verification_label(verify_hash, expected_md5)}), skipping."
        )
        return "skipped"

    if os.path.exists(dest_path):
        current_size = local_size(dest_path)
        if expected_size is not None and current_size < expected_size and resume:
            print(
                f"Resuming {blob.name}: {human_size(current_size)} / {human_size(expected_size)}"
            )
            with open(dest_path, "ab") as out:
                download_range_to_file(blob, out, start=current_size, end=expected_size - 1)
            ok, reason = check_complete_file(
                dest_path, expected_size, expected_md5, verify_hash=verify_hash
            )
            if ok:
                return "resumed"
            if local_size(dest_path) == expected_size:
                move_bad_file(
                    dest_path,
                    f"{blob.name} resumed file failed verification ({reason})",
                )
            else:
                raise RuntimeError(
                    f"{blob.name}: resumed file size is {local_size(dest_path)}, "
                    f"expected {expected_size}"
                )
        elif expected_size is not None and current_size == expected_size:
            _, reason = check_complete_file(
                dest_path, expected_size, expected_md5, verify_hash=verify_hash
            )
            move_bad_file(
                dest_path,
                f"{blob.name} local file failed verification ({reason})",
            )
        elif expected_size is not None and current_size > expected_size:
            move_bad_file(
                dest_path,
                f"{blob.name} local file is larger than remote "
                f"({human_size(current_size)} > {human_size(expected_size)})",
            )
        else:
            move_bad_file(
                dest_path,
                f"{blob.name} exists but is incomplete/cannot be resumed",
            )

    if os.path.exists(part_path) and expected_size is not None and resume:
        part_size = local_size(part_path)
        if part_size == expected_size:
            ok, reason = check_complete_file(
                part_path, expected_size, expected_md5, verify_hash=verify_hash
            )
            if ok:
                os.replace(part_path, dest_path)
                print(f"{blob.name} restored verified .part file, skipping download.")
                return "resumed"
            move_bad_file(
                part_path,
                f"{blob.name}.part failed verification ({reason})",
            )
        elif part_size < expected_size:
            print(
                f"Resuming {blob.name}.part: {human_size(part_size)} / {human_size(expected_size)}"
            )
            with open(part_path, "ab") as out:
                download_range_to_file(blob, out, start=part_size, end=expected_size - 1)
            ok, reason = check_complete_file(
                part_path, expected_size, expected_md5, verify_hash=verify_hash
            )
            if ok:
                os.replace(part_path, dest_path)
                return "resumed"
            if local_size(part_path) == expected_size:
                move_bad_file(
                    part_path,
                    f"{blob.name}.part resumed file failed verification ({reason})",
                )
            else:
                raise RuntimeError(
                    f"{blob.name}: resumed .part size is {local_size(part_path)}, "
                    f"expected {expected_size}"
                )
        else:
            move_bad_file(
                part_path,
                f"{blob.name}.part is larger than remote "
                f"({human_size(part_size)} > {human_size(expected_size)})",
            )

    download_full_blob(
        blob,
        part_path,
        dest_path,
        expected_size,
        expected_md5,
        verify_hash=verify_hash,
    )
    return "downloaded"


def download_prefix(
    bucket: storage.Bucket,
    prefix: str,
    destination_root: str,
    resume: bool = True,
    verify_hash: bool = True,
) -> DownloadStats:
    stats = DownloadStats()
    for blob in bucket.list_blobs(prefix=prefix):
        if blob.name.endswith("/"):
            continue
        stats.objects_seen += 1
        try:
            result = download_blob(
                blob, destination_root, resume=resume, verify_hash=verify_hash
            )
        except Exception as exc:
            stats.failed += 1
            print(f"ERROR downloading {blob.name}: {exc}", file=sys.stderr)
            continue

        if result == "downloaded":
            stats.downloaded += 1
        elif result == "resumed":
            stats.resumed += 1
        elif result == "skipped":
            stats.skipped += 1
    return stats


def make_parser(description: str) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument(
        "-o",
        "--output",
        default=DEFAULT_OUTPUT,
        help=f"Local output root (default: {DEFAULT_OUTPUT})",
    )
    parser.add_argument(
        "--no-resume",
        action="store_true",
        help="Restart incomplete downloads instead of resuming them.",
    )
    parser.add_argument(
        "--no-hash-check",
        action="store_true",
        help="Check completeness using file size only; skip MD5 computation and comparison.",
    )
    return parser


def run_downloader(description: str, prefixes: Iterable[str]) -> int:
    parser = make_parser(description)
    args = parser.parse_args()

    if storage is None:
        print(
            "Missing dependency: google-cloud-storage. Install it with:\n"
            "  pip3 install google-cloud-storage",
            file=sys.stderr,
        )
        return 1

    output = os.path.abspath(args.output)
    prefix_list = tuple(prefixes)
    client = storage.Client.create_anonymous_client()
    bucket = client.bucket(BUCKET_NAME)

    print(f"Bucket: {BUCKET_NAME}")
    print(f"Destination root: {output}")
    print(f"Resume: {not args.no_resume}")
    print(f"Hash check: {not args.no_hash_check}")
    print(f"Prefixes: {', '.join(prefix_list)}")

    total = DownloadStats()
    for prefix in prefix_list:
        print(f"\n>>> {prefix}")
        stats = download_prefix(
            bucket,
            prefix,
            output,
            resume=not args.no_resume,
            verify_hash=not args.no_hash_check,
        )
        total.add(stats)
        print(
            "    prefix done: "
            f"downloaded={stats.downloaded} resumed={stats.resumed} "
            f"skipped={stats.skipped} failed={stats.failed}"
        )

    print(
        "\nAll done. "
        f"downloaded={total.downloaded} resumed={total.resumed} "
        f"skipped={total.skipped} failed={total.failed}"
    )
    if total.objects_seen == 0:
        print(
            "Warning: no objects matched any prefix; check bucket layout or network.",
            file=sys.stderr,
        )
        return 1
    if total.failed > 0:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(
        run_downloader(
            "Download SIFT, LAION, TripClick, and MSMARCO Compass data from compass_osdi.",
            FOUR_DATASET_PREFIXES,
        )
    )
