#!/usr/bin/env bash
# Restore pinned optional upstream sources without creating nested Git repositories.
set -euo pipefail
COMPASS_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
python3 - "$COMPASS_ROOT" <<'PY'
import io, json, pathlib, sys, tarfile, urllib.request
root = pathlib.Path(sys.argv[1]) / 'third_party'
for name, pin in json.loads((root / 'dependencies.json').read_text()).items():
    destination = root / name
    if (destination / 'CMakeLists.txt').exists():
        print(f'{name}: vendored sources present; pinned revision {pin["commit"]}')
        continue
    if destination.exists() and any(destination.iterdir()):
        raise SystemExit(f'Refusing to overwrite nonempty directory: {destination}')
    url = pin['repository'].replace('https://github.com/', 'https://codeload.github.com/') + '/tar.gz/' + pin['commit']
    with urllib.request.urlopen(url, timeout=120) as response:
        archive = response.read()
    destination.mkdir(exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
        for member in tar.getmembers():
            parts = pathlib.PurePosixPath(member.name).parts[1:]
            if not parts: continue
            member.name = str(pathlib.PurePosixPath(*parts))
            tar.extract(member, destination, filter='data')
    print(f'{name}: restored {pin["commit"]}')
PY
