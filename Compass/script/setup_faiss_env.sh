#!/usr/bin/env bash
# Create a local Python environment for building Faiss HNSW indexes.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENV_DIR="${FAISS_VENV:-${REPO_ROOT}/.venv-faiss}"
PYTHON_BIN="${PYTHON_BIN:-python3}"

echo ">>> Creating virtualenv: ${VENV_DIR}"
"${PYTHON_BIN}" -m venv "${VENV_DIR}"

echo ">>> Installing numpy + faiss-cpu"
"${VENV_DIR}/bin/python" -m pip install --upgrade pip
"${VENV_DIR}/bin/python" -m pip install numpy faiss-cpu

echo ">>> Done."
echo "Use:"
echo "  ${VENV_DIR}/bin/python script/build_bigann_hnsw.py sift10M"
echo "  ${VENV_DIR}/bin/python script/build_bigann_pq.py sift10M"
