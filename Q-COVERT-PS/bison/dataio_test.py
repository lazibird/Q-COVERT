from pathlib import Path
import json
import tempfile
import numpy as np
from dataio import ROOT, resolve_paths, read_vectors, read_groundtruth
from prepare import parser, DiskGraph, read_model, node_dtype, INVALID
from benchmark import parser as benchmark_parser, jobs_for
from testsupport import write_bin, write_vecs, fixture

def main():
    config=json.loads((ROOT/'config/dataset_defaults.json').read_text())
    for dataset in config['datasets']:
        args=parser().parse_args(['--dataset',dataset]); resolve_paths(args,True)
        assert args.base==ROOT.parent/config['datasets'][dataset]['files']['base']
        expected='float32' if dataset=='SIFT' else config['datasets'][dataset]['data']['storage_scalar']
        assert args.scalar==expected
        b=benchmark_parser().parse_args(['--dataset',dataset]); resolve_paths(b)
        assert b.query==ROOT.parent/config['datasets'][dataset]['files']['query']
        assert b.groundtruth==ROOT.parent/config['datasets'][dataset]['files']['groundtruth']
        assert b.threads==config['datasets'][dataset]['execution']['access_threads']
        assert b.query_dtype==expected
    with tempfile.TemporaryDirectory() as d:
        work=Path(d)
        for dtype in ('uint8','float32'):
            vectors=np.arange(21).reshape(3,7).astype('u1' if dtype=='uint8' else 'f4')
            if dtype=='float32': vectors=vectors/8-.25
            write_bin(work/'vectors.bin',vectors,'u1' if dtype=='uint8' else '<f4')
            np.testing.assert_array_equal(read_vectors(work/'vectors.bin',vector_dtype=dtype),vectors)
            write_vecs(work/'vectors.fvecs',vectors)
            np.testing.assert_array_equal(read_vectors(work/'vectors.fvecs'),vectors)
        ids=np.asarray([[1,2,3],[4,5,6]],'<u4'); write_bin(work/'gt.bin',ids,'<u4')
        np.testing.assert_array_equal(read_groundtruth(work/'gt.bin'),ids)
        for old in (False,True):
            base=fixture(work/f'index{old}',7,5,3,True,old_pivots=old)
            graph=DiskGraph(work/f'index{old}','float32')
            np.testing.assert_array_equal(graph.read(17)[0],base[17])
            book,mean,chunks,rotation,rotated,codes=read_model(work/f'index{old}')
            assert list(chunks)==[0,1,4,7] and codes.shape==(48,3) and rotated==1
        base=fixture(work/'multisector',128,1000,16,multi_sector=True)
        graph=DiskGraph(work/'multisector','float32')
        assert not graph.per_sector
        np.testing.assert_array_equal(graph.read(47)[0],base[47])
        Path(str(work/'multisector')+'_disk.index_labels.txt').write_text('unsupported')
        try: DiskGraph(work/'multisector','float32')
        except ValueError: pass
        else: raise AssertionError('filtered graph accepted')
        row=np.zeros(1,node_dtype(7,5,3,'float32'))
        row['id']=(1<<24)+1
        assert int(np.frombuffer(row.tobytes(),dtype=row.dtype)['id'][0])==(1<<24)+1
        path=work/'bad.bin';path.write_bytes(b'\0'*7)
        try: read_vectors(path)
        except ValueError: pass
        else: raise AssertionError('truncated shape accepted')
    args=benchmark_parser().parse_args(['--iterations','3','5','--beamwidth','2','--l-search','9','11','--k','5'])
    assert jobs_for(args)==[(3,2,9),(5,2,11)]
    args.beamwidth=[1,2,3]
    try: jobs_for(args)
    except ValueError: pass
    else: raise AssertionError('mismatched jobs accepted')
    print('BiSON five JSON profiles, bin/vecs, DiskANN layouts and PQ formats passed')
if __name__=='__main__': main()
