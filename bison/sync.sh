#!/usr/bin/env bash
set -euo pipefail

: "${REMOTE_USER:?Set REMOTE_USER to the SSH login name}"
: "${REMOTE_HOST:?Set REMOTE_HOST to the destination host}"

scp -i ~/.ssh/id_rsa -r /home/nsd/new_secure_diskann/build "${REMOTE_USER}@${REMOTE_HOST}:/home/nsd"


scp -i ~/.ssh/id_rsa -r /home/nsd/secure_diskann/build "${REMOTE_USER}@${REMOTE_HOST}:/home/bigann"
