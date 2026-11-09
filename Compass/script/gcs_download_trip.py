#!/usr/bin/env python3
"""Download the TripClick dataset and ORAM snapshots from the public compass_osdi GCS bucket.

Usage (from the Compass directory):
  pip3 install google-cloud-storage
  python3 script/gcs_download_trip.py

After downloading, run:
  ./script/run_compass_ring_local.sh trip 8000 100
"""

from __future__ import annotations

import sys

from gcs_download_common import run_downloader


PREFIXES = (
    "dataset/trip_distilbert/",
    "snap/server/trip/",
    "snap/client/trip/",
)


if __name__ == "__main__":
    sys.exit(
        run_downloader(
            "Download TripClick dataset + snap files from compass_osdi GCS bucket.",
            PREFIXES,
        )
    )
