#!/usr/bin/env bash
# Ubuntu 24.04: extract development packages into this checkout without sudo.
set -euo pipefail
QCOVERT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QCOVERT_DEPS="$QCOVERT_ROOT/.deps"
mkdir -p "$QCOVERT_DEPS/debs"
cd "$QCOVERT_DEPS/debs"
apt-get download libboost-program-options1.83-dev libboost-program-options1.83.0 \
  libaio-dev libaio1t64 liburing-dev liburing2 libgoogle-perftools-dev \
  libgoogle-perftools4t64 libtcmalloc-minimal4t64 libunwind-dev libunwind8 \
  libblas-dev libblas3 liblapack-dev liblapack3 libgfortran5 libquadmath0
for package in ./*.deb; do dpkg-deb -x "$package" "$QCOVERT_DEPS"; done
python3 - "$QCOVERT_DEPS" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
lib = root / 'usr/lib/x86_64-linux-gnu'
# Debian alternatives refer to /etc; local installations need direct links.
for name in ('blas', 'lapack'):
    for suffix in ('.so', '.so.3', '.a'):
        link = lib / ('lib' + name + suffix)
        target = lib / name / ('lib' + name + suffix)
        if target.exists():
            if link.is_symlink(): link.unlink()
            if not link.exists(): link.symlink_to(target.relative_to(lib))
for pc in root.rglob('*.pc'):
    text = pc.read_text().replace('prefix=/usr', 'prefix=' + str(root / 'usr'))
    text = text.replace('libdir=/usr/', 'libdir=' + str(root) + '/usr/').replace('includedir=/usr/', 'includedir=' + str(root) + '/usr/')
    pc.write_text(text)
PY
printf 'Local dependency prefix: %s\n' "$QCOVERT_DEPS/usr"
