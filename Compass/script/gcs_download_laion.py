#!/usr/bin/env python3
"""Download the LAION dataset and ORAM snapshots from the public compass_osdi GCS bucket.

Usage (from the Compass directory):
  pip3 install google-cloud-storage
  python3 script/gcs_download_laion.py

After downloading, run:
  ./script/run_compass_ring_local.sh laion 8000 100
"""

from __future__ import annotations

import sys

from gcs_download_common import run_downloader


PREFIXES = (
    "dataset/laion1m/",
    "snap/server/laion/",
    "snap/client/laion/",
)


if __name__ == "__main__":
    sys.exit(
        run_downloader(
            "Download LAION dataset + snap files from compass_osdi GCS bucket.",
            PREFIXES,
        )
    )
