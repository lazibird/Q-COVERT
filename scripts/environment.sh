#!/usr/bin/env bash
# Source from Bash to use the shared Python/MKL and optional local C++ libraries.
QCOVERT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$QCOVERT_ROOT/.venv/bin:$PATH"
export LD_LIBRARY_PATH="$QCOVERT_ROOT/.venv/lib:$QCOVERT_ROOT/.deps/usr/lib/x86_64-linux-gnu:$QCOVERT_ROOT/.deps/usr/lib/x86_64-linux-gnu/blas:$QCOVERT_ROOT/.deps/usr/lib/x86_64-linux-gnu/lapack${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PKG_CONFIG_PATH="$QCOVERT_ROOT/.deps/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export QCOVERT_PYTHON="$QCOVERT_ROOT/.venv/bin/python"
