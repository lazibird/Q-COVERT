"""Small offline integration test, invoked explicitly when NumPy/FAISS are installed."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from clustering import cluster_vectors
from prepare_clusters import prepare


def main():
    build = Path(sys.argv[1]).resolve()
    # Repeated points exercise the balanced fallback during oversized splitting.
    for dtype in (np.float32, np.uint8):
        vectors = np.array([[0, 0, 0]] * 7 + [[255, 254, 250], [2, 3, 4]], dtype=dtype)
        clustered = cluster_vectors(vectors, 3, initial_clusters=1, niter=2)
        assert np.bincount(clustered.labels).max() <= 3
        assert len(clustered.labels) == len(vectors)
        with tempfile.TemporaryDirectory(prefix='qcovert-offline-') as directory:
            root = Path(directory)
            np.save(root / 'vectors.npy', vectors)
            prepare(root / 'vectors.npy', root / 'clusters.raw', 3, 8, 1, niter=2)
            subprocess.run([str(build / 'qcovert_prepare'), str(root / 'clusters.raw'),
                            str(root / 'prepared.bin')], check=True)
            plain = (root / 'prepared.bin').read_bytes()
            assert plain[:8] == (0x3150435452564351).to_bytes(8, 'little')
            if dtype == np.uint8:
                output = subprocess.check_output([str(build / 'qcovert_example'),
                                                  str(root / 'prepared.bin')], text=True)
                assert 'id=0 squared_l2=0' in output and 'id=1 squared_l2=0' in output
            (root / 'key').write_bytes(os.urandom(16))
            subprocess.run([str(build / 'qcovert_prepare'), str(root / 'clusters.raw'),
                            str(root / 'encrypted.bin'), str(root / 'key')], check=True)
            cipher = (root / 'encrypted.bin').read_bytes()
            assert len(cipher) == len(plain) + 28 and cipher[12:28] != plain[:16]
    print('Offline clustering/quantization/reload passed')


if __name__ == '__main__':
    main()
