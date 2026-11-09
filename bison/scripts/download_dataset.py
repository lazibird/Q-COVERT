#!/usr/bin/env python3
"""Download raw datasets from the public compass_osdi GCS bucket (anonymous access).

Only downloads the raw vector files BiSON needs (base/query vectors),
NOT the huge Compass ORAM snapshots in the same bucket.

Usage:
    source venv/bin/activate
    python scripts/download_dataset.py <dataset> <dest_dir>
    dataset: sift | laion100k | trip | msmarco | all
"""

import os
import sys

from google.cloud import storage

FILES = {
    "sift": [
        "dataset/sift/base.fvecs",
        "dataset/sift/query.fvecs",
        "dataset/sift/learn.fvecs",
    ],
    "laion100k": [
        "dataset/laion1m/100k/laion_base.fvecs",
        "dataset/laion1m/laion_query.fvecs",
    ],
    "trip": [
        "dataset/trip_distilbert/passages.fvecs",
        "dataset/trip_distilbert/queries.fvecs",
    ],
    "msmarco": [
        "dataset/msmarco_bert/passages.fvecs",
        "dataset/msmarco_bert/queries.fvecs",
    ],
}

BUCKET = "compass_osdi"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    dataset, dest_dir = sys.argv[1], sys.argv[2]
    names = list(FILES) if dataset == "all" else [dataset]
    wanted = [f for n in names for f in FILES[n]]

    client = storage.Client.create_anonymous_client()
    bucket = client.bucket(BUCKET)

    for name in wanted:
        dest = os.path.join(dest_dir, os.path.basename(name))
        if os.path.exists(dest):
            print(f"skip (exists): {dest}")
            continue
        os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
        blob = bucket.blob(name)
        print(f"downloading {name} ({blob.size / 1e9:.2f} GB) -> {dest}")
        blob.download_to_filename(dest)
    print("done.")


if __name__ == "__main__":
    main()
