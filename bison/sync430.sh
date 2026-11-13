#!/usr/bin/env bash
set -euo pipefail

: "${REMOTE_USER:?Set REMOTE_USER to the SSH login name}"
: "${REMOTE_HOST:?Set REMOTE_HOST to the destination host}"

make
scp -i ~/.ssh/id_rsa -r /home/nsd/secure_diskann/build "${REMOTE_USER}@${REMOTE_HOST}:/home/nsd"

# scp -i ~/.ssh/id_rsa -r /data/small/new_data "${REMOTE_USER}@${REMOTE_HOST}:/data/small/"


# scp -i ~/.ssh/id_rsa -r /data/small/new_oram_data "${REMOTE_USER}@${REMOTE_HOST}:/data/small/"



# scp -i ~/.ssh/id_rsa -r /data/oram_data/sift10m "${REMOTE_USER}@${REMOTE_HOST}:/data/large/"


# scp -i ~/.ssh/id_rsa -r /data/oram_data/sift50m/new_R128_L100_PQ32 "${REMOTE_USER}@${REMOTE_HOST}:/data/large/sift50m/"

# scp -i ~/.ssh/id_rsa -r /data/oram_data/sift100m/new_R128_L100_PQ32 "${REMOTE_USER}@${REMOTE_HOST}:/data/large/sift100m/"
