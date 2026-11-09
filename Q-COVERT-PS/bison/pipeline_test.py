from pathlib import Path
import csv
import hashlib
import json
import stat
import os
import struct
from types import SimpleNamespace
from benchmark import identity
import subprocess
import sys
import tempfile
import numpy as np
from dataio import ROOT
from prepare import prepared_full
from testsupport import fixture, write_vecs, write_bin

def test(build,mode='native'):
    target='bison_benchmark' if mode=='native' else 'bison_sgx_host'
    layout=json.loads(subprocess.check_output([str(build/'bison'/target),'--describe-layout'],text=True))
    dim,degree,p=layout['dimension'],layout['degree'],layout['pq_subspaces']
    scalar=layout['scalar']
    def run(script,*args,good=True):
        proc=subprocess.run([sys.executable,str(ROOT/'bison'/script),*map(str,args)],capture_output=True,text=True)
        if (proc.returncode==0)!=good: raise AssertionError(proc.stdout+proc.stderr)
        return proc
    with tempfile.TemporaryDirectory(prefix='bison-pipeline-') as directory:
        work=Path(directory)
        base=fixture(work/'diskann',dim,degree,p,scalar=='float32',old_pivots=True,scalar=scalar)
        queries=base[[1,17,31]]
        truth=np.argsort(np.sum((queries[:,None].astype('f8')-base[None].astype('f8'))**2,axis=2),axis=1)[:,:10]
        write_vecs(work/'base.fvecs',base);write_vecs(work/'query.fvecs',queries);write_vecs(work/'gt.ivecs',truth,True)
        prepare=['--import-only','--index-prefix',work/'diskann','--scalar',scalar,'--base',work/'base.fvecs','--cache-depth',0]
        run('prepare.py',*prepare,'--output',work/'prepared.bin')
        raw,_,model,medoids,nodes,cache,h=prepared_full(work/'prepared.bin')
        assert h==0 and len(cache)==0
        np.testing.assert_array_equal(nodes['embedding'],base)
        assert layout['search_protocol']==2
        # An old BISDO001 file can contain BFS payloads. Online code must skip
        # them and still issue TW real/dummy requests, with no trusted cache.
        legacy=work/'legacy-cache.bin'
        legacy_bytes=bytearray(raw)
        struct.pack_into('<Q',legacy_bytes,7*8,len(nodes))
        struct.pack_into('<Q',legacy_bytes,8*8,2)
        legacy_bytes.extend(nodes.tobytes()); legacy.write_bytes(legacy_bytes)
        common=['--prepared',work/'prepared.bin','--query',work/'query.fvecs','--groundtruth',work/'gt.ivecs',
            '--build-dir',build,'--mode',mode,'--iterations',3,5,2,3,'--beamwidth',2,3,7,2,'--l-search',9,11,8,9,
            '--k',5,'--queries',3,'--warmup',1,'--repeats',2,'--threads',2,'--z',8,'--s',16,'--a',4,'--evict-batch',1,
            '--reference','--csv',work/'result.csv']
        missing=run('benchmark.py',*common,'--build-dir',work/'missing',good=False)
        assert 'runner/enclave not found' in missing.stderr and '-DBISON_' in missing.stderr and 'Traceback' not in missing.stderr
        first=run('benchmark.py',*common)
        assert first.stdout.count('oram_snapshot_status=built')==1
        assert first.stderr.count('bison/bulk_setup\t1\t1')==1
        image=work/f'prepared.initial_oram.{mode}.state'
        assert stat.S_IMODE(image.stat().st_mode)==0o600
        digest=hashlib.sha256(image.read_bytes()).digest()
        second=run('benchmark.py',*common)
        assert second.stdout.count('oram_snapshot_status=loaded')==1 and 'bulk_setup' not in second.stderr
        assert second.stderr.count('bison/restore\t1\t1')==1
        assert hashlib.sha256(image.read_bytes()).digest()==digest
        rows=list(csv.DictReader((work/'result.csv').open()))
        assert len(rows)==8 and [int(r['logical_reads']) for r in rows[:4]]==[6,15,14,6]
        assert all(r['reference_result_match']=='True' and r['search_variant']=='appendix-f' for r in rows)
        assert all(int(r['cached_rounds'])==int(r['cache_nodes'])==int(r['trusted_cache_bytes'])==0 for r in rows)
        assert all(int(r['visited_capacity'])==int(r['rerank_vectors'])==int(r['logical_reads']) for r in rows)
        assert len({r['setup_ms'] for r in rows[:4]})==1
        changed=run('benchmark.py',*common,'--iterations',8,'--beamwidth',4,'--l-search',13,'--threads',1)
        assert 'oram_snapshot_status=loaded' in changed.stdout
        run('benchmark.py',*common,'--no-oram-snapshot','--csv',work/'no-image.csv')
        legacy_args=[*common,'--prepared',legacy,'--csv',work/'legacy.csv']
        # Optional native regression against an actual prior-protocol runner.
        # Physical tree/map snapshots are compatible across this semantic change.
        old_runner=os.environ.get('BISON_LEGACY_RUNNER') if mode=='native' else None
        legacy_image=work/'legacy.initial.state'
        old_snapshot=False
        if old_runner:
            old_layout=json.loads(subprocess.check_output([old_runner,'--describe-layout'],text=True))
            if all(old_layout[key]==layout[key] for key in ('dimension','degree','pq_subspaces','scalar')):
                old_request=work/'old-request.bin'; old_report=work/'old-report.bin'
                old_raw,old_shape,*_=prepared_full(legacy)
                fields=[int.from_bytes(b'BISRQ001','little'),3,2,9,5,8,16,4,1,0,2,0,len(old_raw),0,3,1,3]
                image_id=identity(hashlib.sha256(old_raw).hexdigest(),old_shape,
                                  SimpleNamespace(mode=mode,z=8,s=16,a=4,evict_batch=1,stash=0))
                with old_request.open('wb') as out:
                    out.write(struct.pack('<17Q',*fields));out.write(image_id)
                    out.write(struct.pack('<4Q',1,3,2,9));out.write(old_raw)
                    out.write(np.asarray(queries,dtype='<f4').tobytes())
                result=subprocess.run([old_runner,str(old_request),str(old_report),str(legacy_image)],capture_output=True,text=True)
                assert result.returncode==0,result.stderr
                assert 'oram_snapshot_status=built' in result.stdout
                old_snapshot=True
        legacy_run=run('benchmark.py',*legacy_args,'--oram-snapshot',legacy_image)
        if old_snapshot:
            assert 'oram_snapshot_status=loaded' in legacy_run.stdout
        legacy_rows=list(csv.DictReader((work/'legacy.csv').open()))
        assert [int(r['logical_reads']) for r in legacy_rows]==[6,15,14,6]
        assert all(r['reference_result_match']=='True' and int(r['cache_nodes'])==0 for r in legacy_rows)
        # Old search protocol is rejected; failures cannot append CSV.
        if old_snapshot:
            rejected=subprocess.run([str(build/'bison'/target),str(old_request),str(work/'reject.bin')],capture_output=True,text=True) if mode=='native' else None
            if rejected is not None: assert rejected.returncode!=0 and 'version mismatch' in rejected.stderr
        # Different metric/layout content invalidates and rebuilds an image.
        image.write_bytes(image.read_bytes()[:-13])
        rebuilt=run('benchmark.py',*common)
        assert 'oram_snapshot_status=built' in rebuilt.stdout and 'snapshot_invalid' in rebuilt.stderr
        forced=run('benchmark.py',*common,'--rebuild-oram')
        assert 'oram_snapshot_status=built' in forced.stdout
        # Configuration defaults choose actual profile filenames and raw scalar.
        config=json.loads((ROOT/'config/dataset_defaults.json').read_text())
        applicable=['SIFT','DEEP1M'] if scalar=='float32' else ['SIFT10M','SIFT100M']
        for dataset in applicable:
            profile=config['datasets'][dataset]
            for key,values in [('base',base),('query',queries),('groundtruth',truth)]:
                dest=work/'datasets'/Path(profile['files'][key]).relative_to('data')
                dest.parent.mkdir(parents=True,exist_ok=True)
                if dest.suffix=='.bin': write_bin(dest,values,'<u4' if key=='groundtruth' else ('u1' if scalar=='uint8' else '<f4'))
                else: write_vecs(dest,values,key=='groundtruth')
            output=work/(dataset+'.bin')
            run('prepare.py','--dataset',dataset,'--data-root',work/'datasets','--index-prefix',work/'diskann',
                '--import-only','--cache-depth',0,'--output',output)
            outcome=run('benchmark.py','--dataset',dataset,'--data-root',work/'datasets','--prepared',output,
                '--mode',mode,'--build-dir',build,'--queries',2,'--warmup',0,'--threads',1,'--k',5,
                '--iterations',3,'--beamwidth',2,'--l-search',9,'--z',8,'--s',16,'--a',4,'--evict-batch',1,
                '--reference','--no-oram-snapshot','--csv',work/(dataset+'.csv'))
            assert '"reference_result_match": true' in outcome.stdout
        # All-bin overrides remain supported (float32 for DEEP1M).
        write_bin(work/'query.bin',queries,'u1' if scalar=='uint8' else '<f4');write_bin(work/'gt.bin',truth,'<u4')
        run('benchmark.py',*common,'--query',work/'query.bin','--query-dtype',scalar,'--groundtruth',work/'gt.bin','--iterations',3,'--beamwidth',2,'--l-search',9)
        # Isolated plaintext mode does not need an executable or ORAM memory.
        plaintext=run('benchmark.py',*common,'--reference-only','--build-dir',work/'missing','--csv',work/'plaintext.csv')
        assert '"mode": "reference"' in plaintext.stdout
        before=(work/'result.csv').read_bytes()
        for values in ([0],[-1],[1<<64]): run('benchmark.py',*common,'--iterations',*values,good=False)
        run('benchmark.py',*common,'--beamwidth',1,2,3,good=False)
        run('benchmark.py',*common,'--l-search',4,good=False)
        run('benchmark.py',*common,'--no-oram-snapshot','--rebuild-oram',good=False)
        run('prepare.py',*prepare,'--cache-depth',1,'--output',work/'prepared.bin',good=False)
        bad=work/'bad.bin';bad.write_bytes((work/'prepared.bin').read_bytes()[:-1])
        run('benchmark.py',*common,'--prepared',bad,good=False)
        assert (work/'result.csv').read_bytes()==before
        # Importing fractional DEEP as uint8 must fail without overwriting data.
        if scalar=='float32':
            old=(work/'prepared.bin').read_bytes()
            run('prepare.py',*prepare,'--scalar','uint8','--output',work/'prepared.bin',good=False)
            assert (work/'prepared.bin').read_bytes()==old
    print('BiSON Appendix F Native/SGX pipeline, TW budgets, legacy snapshots and oracle passed')
if __name__=='__main__': test(Path(sys.argv[1]).resolve(),sys.argv[2] if len(sys.argv)>2 else 'native')
