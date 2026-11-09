#!/usr/bin/env python3
"""Download all Compass data from the public compass_osdi GCS bucket."""

from __future__ import annotations

import sys

from gcs_download_common import run_downloader


PREFIXES = ("",)


if __name__ == "__main__":
    sys.exit(
        run_downloader(
            "Download all files from compass_osdi GCS bucket.",
            PREFIXES,
        )
    )
