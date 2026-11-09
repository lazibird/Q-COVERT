#!/usr/bin/env python3
"""
scan sgx edl, enumerate dependencies
used from cmake to produce a depfile for edl
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from typing import Iterable, List, Set


_IMPORT_RE = re.compile(r"\b(from|import|include)\s+\"([^\"]+)\"")


def _strip_comments(text: str) -> str:
    """Remove line and block comments to avoid false-positive matches."""

    without_block = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", without_block)


def _find_imports(text: str) -> List[str]:
    cleaned = _strip_comments(text)
    return [match.group(2) for match in _IMPORT_RE.finditer(cleaned)]


def _resolve(name: str, search_paths: Iterable[pathlib.Path]) -> pathlib.Path:
    for base in search_paths:
        candidate = (base / name).resolve()
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        f"unable to resolve '{name}' in search paths: {search_paths}"
    )


def collect_dependencies(
    root: pathlib.Path, search_paths: Iterable[pathlib.Path]
) -> List[pathlib.Path]:
    """Return a flattened list of the root EDL and all recursively imported EDLs."""

    search_list = [p.resolve() for p in search_paths]
    root_path = root.resolve()

    seen: Set[pathlib.Path] = set()
    order: List[pathlib.Path] = []
    stack: List[pathlib.Path] = [root_path]

    while stack:
        current = stack.pop()
        if current in seen:
            continue
        seen.add(current)
        order.append(current)

        try:
            content = current.read_text()
        except OSError as exc:  # pragma: no cover - handled by build system
            raise RuntimeError(f"failed to read EDL '{current}': {exc}") from exc

        # Prefer the current file's directory first, then the provided search paths.
        current_search = [current.parent, *search_list]
        for inc in _find_imports(content):
            resolved = _resolve(inc, current_search)
            if resolved not in seen:
                stack.append(resolved)

    return order


def _escape(path: pathlib.Path) -> str:
    # Simple make-style escaping for spaces and backslashes.
    text = str(path)
    text = text.replace("\\", "\\\\")
    text = text.replace(" ", "\\ ")
    return text


def write_depfile(
    target: str, deps: Iterable[pathlib.Path], depfile: pathlib.Path
) -> None:
    depfile.parent.mkdir(parents=True, exist_ok=True)
    escaped_deps = " ".join(_escape(d) for d in deps)
    depfile.write_text(f"{target}: {escaped_deps}\n")


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="SGX EDL dependency scanner")
    parser.add_argument("--edl", required=True, help="root EDL file")
    parser.add_argument(
        "--search",
        action="append",
        default=[],
        help="additional search path (repeatable)",
    )
    parser.add_argument("--depfile", help="write depfile to this path")
    parser.add_argument("--target", help="depfile target (defaults to the root EDL)")
    parser.add_argument(
        "--list", action="store_true", help="print dependencies to stdout"
    )

    args = parser.parse_args(argv)
    if not args.depfile and not args.list:
        parser.error("must specify at least one of --depfile or --list")
    return args


def main(argv: List[str]) -> int:
    args = parse_args(argv)

    edl_path = pathlib.Path(args.edl)
    search_paths = [pathlib.Path(p) for p in args.search]

    deps = collect_dependencies(edl_path, search_paths)

    if args.depfile:
        depfile_path = pathlib.Path(args.depfile)
        target = args.target or str(edl_path)
        write_depfile(target, deps, depfile_path)

    if args.list:
        for dep in deps:
            print(dep)

    return 0


if __name__ == "__main__":  # pragma: no cover - build-time utility
    sys.exit(main(sys.argv[1:]))
