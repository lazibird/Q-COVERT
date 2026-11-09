"""Compatibility imports for the shared dataset I/O implementation."""
import sys
from pathlib import Path

_ROOT = str(Path(__file__).resolve().parents[2])
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)
from data.vector_io import (read_vector_bin, read_groundtruth_bin, read_vecs,
                            write_vecs, read_labels_vecs)
