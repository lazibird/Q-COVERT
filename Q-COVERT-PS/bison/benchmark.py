#!/usr/bin/env python3
"""Run BiSON-DO Native/SGX jobs with one initialization and append one CSV row per job."""
import argparse
from contextlib import ExitStack
import csv
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import time
import numpy as np
from dataio import ROOT, add_dataset_arguments, resolve_paths, read_vectors, read_groundtruth
from prepare import INVALID, prepared_full
from reference import search
FIELDS=['dataset','mode','metric','n','dimension','degree','pq_bytes','scalar','iterations','beamwidth','l_search','k',
        'cached_rounds','cache_nodes','logical_reads','recall_at_k','total_ms','search_ms','oram_ms','eviction_ms',
        'sets_ms','pq_ms','exact_ms','position_ms','cache_ms','setup_ms','node_block_bytes','prepared_bytes',
        'oram_storage_bytes','position_map_bytes','trusted_cache_bytes','peak_host_rss_bytes',
        'reference_recall_at_k','reference_ms','reference_result_match','z','s','a','evict_batch','stash','threads','search_variant','visited_capacity','rerank_vectors']

class RunnerUnavailableError(RuntimeError): pass

def jobs_for(args):
    lists=[args.iterations,args.beamwidth,args.l_search]
    count=max(map(len,lists))
    if not 0<count<=1024 or any(len(v) not in (1,count) for v in lists): raise ValueError('T/W/L lists must have matching lengths or one broadcast value')
    jobs=[tuple(v[0] if len(v)==1 else v[i] for v in lists) for i in range(count)]
    for t,w,l in jobs:
        if any(not 0<x<(1<<64) for x in (t,w,l)) or l<args.k: raise ValueError('positive uint64 T/W/L required, with L>=k')
    return jobs

def runner(args, layout):
    target='bison_benchmark' if args.mode=='native' else 'bison_sgx_host'
    sgx_flags=[] if args.mode=='native' else ['-DRINGDORAM_ENABLE_SGX=ON',f'-DRINGDORAM_SGX_MODE={args.mode.split("-")[1].upper()}']
    configure=['cmake','-S',str(ROOT),'-B',str(args.build_dir),'-DCMAKE_BUILD_TYPE=Release',
        '-DRINGDORAM_BUILD_BISON=ON','-DRINGDORAM_BUILD_EXAMPLES=ON',
        f'-DBISON_DIM={layout["dimension"]}',f'-DBISON_DEGREE={layout["degree"]}',
        f'-DBISON_PQ_BYTES={layout["pq_subspaces"]}',f'-DBISON_SCALAR={layout["scalar"]}']+sgx_flags
    multi=['cmake','-S',str(ROOT),'-B',str(args.build_dir),'-DCMAKE_BUILD_TYPE=Release',
        '-DRINGDORAM_BUILD_BISON=ON','-DRINGDORAM_BUILD_EXAMPLES=ON',
        f'-DRINGDORAM_BISON_LAYOUTS={layout["dimension"]}:{layout["degree"]}:{layout["pq_subspaces"]}:{layout["scalar"]}']+sgx_flags
    instructions=('\n  single-layout:'+'\n  '+shlex.join(configure)
        +'\n  '+shlex.join(['cmake','--build',str(args.build_dir),'--target',target,'-j2'])
        +'\n  or add a variant alongside existing binaries:'
        +'\n  '+shlex.join(multi)
        +'\n  '+shlex.join(['cmake','--build',str(args.build_dir),'-j2']))
    def probe(command):
        actual=json.loads(subprocess.run([command[0],'--describe-layout'],text=True,capture_output=True,check=True).stdout)
        return not any(actual.get(k)!=v for k,v in layout.items()) and actual.get('mode','').lower()==args.mode \
            and actual.get('search_protocol')==2 and all(actual.get(k)==1 for k in ('prepared_protocol','snapshot_protocol'))
    if args.mode=='native':
        candidates=[(path,None) for path in sorted((args.build_dir/'bison').glob('bison_benchmark*'))
            if path.is_file() and os.access(path,os.X_OK)]
    else:
        candidates=[]
        for path in sorted((args.build_dir/'bison').glob('bison_sgx_host*')):
            if not path.is_file() or not os.access(path,os.X_OK): continue
            suffix=path.name[len('bison_sgx_host'):]
            enclave=(args.build_dir/'apps/sgx/ringdoram_enclave.signed.so') if not suffix \
                else (args.build_dir/'bison'/f'bison_enclave{suffix}.signed.so')
            if enclave.is_file(): candidates.append((path,enclave))
    if not candidates:
        raise RunnerUnavailableError('BiSON runner/enclave not found. Build with:'+instructions)
    for path,enclave in candidates:
        command=[str(path)]+([str(enclave)] if enclave else [])
        if probe(command): return command
    raise RunnerUnavailableError(f'No BiSON runner in {args.build_dir/"bison"} matches layout {layout}. Rebuild with:'+instructions)

def recall(ids, truth, k):
    return sum(len((set(map(int,a))-{INVALID}) & set(map(int,b[:k])))/k for a,b in zip(ids,truth))/len(ids)

def identity(digest,layout,args):
    # Cross-mode: native-built images may be loaded by the SGX runner (the
    # enclave accepts the raw-wrapped experiment key), so mode is not part
    # of the identity.
    values={'format':'bison-initial-v1','oram_format':4,'prepared_sha256':digest,'layout':layout,
        'oram':[args.z,args.s,args.a,args.evict_batch,args.stash]}
    return hashlib.sha256(json.dumps(values,sort_keys=True).encode()).digest()

def reference_results(queries,model,medoids,nodes,cache,h,job,k):
    start=time.perf_counter()
    results=[search(q,model,medoids,nodes,cache,h,*job,k) for q in queries]
    return results,(time.perf_counter()-start)*1000/len(queries)

def parse_frame(output,args,job,count,raw,layout,model,medoids,nodes,cache,h,queries,truth,rss):
    if len(output)<112: raise ValueError('truncated BiSON report')
    magic,rows,k,setup,t,w,block,storage,positions,q,reported_h,c,cachebytes,l=struct.unpack_from('<14Q',output)
    expected=job[0]*job[1]
    if magic!=int.from_bytes(b'BISRT002','little') or rows!=count*args.repeats or k!=args.k or (t,w,l)!=job or q!=expected or reported_h!=0 or c!=0 or cachebytes!=0: raise ValueError('BiSON report parameters/budget mismatch')
    stride=72+16*k
    if len(output)!=112+rows*stride: raise ValueError('BiSON report length mismatch')
    times=[]; results=[]
    for i in range(rows):
        off=112+i*stride; timing=struct.unpack_from('<9Q',output,off)
        if timing[7]!=expected: raise ValueError('variable BiSON ORAM operation count')
        times.append(timing)
        results.append([struct.unpack_from('<Qd',output,off+72+j*16) for j in range(k)])
    means=np.mean(np.asarray(times,dtype='f8'),axis=0)/1e6
    ids=[[i for i,_ in row] for row in results]
    row=dict(dataset=args.dataset,mode=args.mode,metric='l2',n=len(nodes),dimension=layout['dimension'],degree=layout['degree'],pq_bytes=layout['pq_subspaces'],scalar=layout['scalar'],
        iterations=t,beamwidth=w,l_search=l,k=k,cached_rounds=reported_h,cache_nodes=c,logical_reads=q,
        recall_at_k=recall(ids,[truth[i%count] for i in range(rows)],k),total_ms=means[0],search_ms=means[0]-means[2],
        oram_ms=means[1],eviction_ms=means[2],sets_ms=means[3],pq_ms=means[4],exact_ms=means[5],position_ms=means[6],cache_ms=means[8],
        setup_ms=setup/1e6,node_block_bytes=block,prepared_bytes=len(raw),oram_storage_bytes=storage,position_map_bytes=positions,trusted_cache_bytes=cachebytes,peak_host_rss_bytes=rss,
        reference_recall_at_k='',reference_ms='',reference_result_match='',z=args.z,s=args.s,a=args.a,evict_batch=args.evict_batch,stash=args.stash,threads=args.threads,search_variant='appendix-f',visited_capacity=t*w,rerank_vectors=t*w)
    if args.reference:
        ref,ms=reference_results(queries[:count],model,medoids,nodes,cache,h,job,k)
        match=all(value==ref[i%count] for i,value in enumerate(results))
        if not match: raise ValueError('BiSON output differs from plaintext reference (ID/distance)')
        row.update(reference_ms=ms,reference_result_match=match,reference_recall_at_k=recall([[i for i,_ in a] for a in ref],truth[:count],k))
    return row

def run_jobs(args,jobs,raw,layout,model,medoids,nodes,cache,h,queries,truth,count):
    command=runner(args,layout)
    image=None if args.no_oram_snapshot else (args.oram_snapshot or args.prepared.with_name(args.prepared.stem+'.initial_oram.'+args.mode+'.state')).resolve()
    if image in (args.prepared.resolve(),args.query.resolve(),args.groundtruth.resolve(),args.csv.resolve()): raise ValueError('snapshot path conflicts with data/CSV')
    header=[int.from_bytes(b'BISRQ002','little'),*map(max,zip(*jobs)),args.k,args.z,args.s,args.a,args.evict_batch,args.stash,
            args.threads,args.warmup,len(raw),0,count,args.repeats,0 if image is None else (3 if args.rebuild_oram else 1)]
    rows=[]
    with ExitStack() as stack:
        if image:
            image.parent.mkdir(parents=True,exist_ok=True)
            lock=stack.enter_context(Path(str(image)+'.lock').open('a+b')); fcntl.flock(lock,fcntl.LOCK_EX)
        folder=Path(stack.enter_context(tempfile.TemporaryDirectory(prefix='bison-benchmark-')))
        request,report=folder/'request.bin',folder/'report.bin'
        digest=hashlib.sha256()
        with request.open('wb') as out:
            out.write(struct.pack('<17Q',*header)); out.write(bytes(32)); out.write(struct.pack('<Q',len(jobs)))
            for job in jobs: out.write(struct.pack('<3Q',*job))
            for offset in range(0,len(raw),1<<20):
                chunk=raw[offset:offset+(1<<20)]; out.write(chunk); digest.update(chunk)
            out.write(queries.tobytes()); out.seek(136); out.write(identity(digest.hexdigest(),layout,args))
        process=subprocess.Popen(command+[str(request),str(report)]+([str(image)] if image else []),text=True,stdout=subprocess.PIPE)
        # The runner streams one report block per job; buffer each completed
        # job's row here and write them all once after the runner reports the
        # final peak RSS / snapshot status.
        frame_bytes=112+count*args.repeats*(72+args.k*16)
        done=0
        while done<len(jobs):
            size=report.stat().st_size if report.exists() else 0
            while done<len(jobs) and size>=(done+1)*frame_bytes:
                with report.open('rb') as f:
                    f.seek(done*frame_bytes); frame=f.read(frame_bytes)
                t,w,l=jobs[done]
                row=parse_frame(frame,args,jobs[done],count,raw,layout,model,medoids,nodes,cache,h,queries,truth,0)
                rows.append(row); done+=1
                print(f'job {done}/{len(jobs)}: T={t} W={w} L={l} recall={row["recall_at_k"]:.4f} total_ms={row["total_ms"]:.3f}',flush=True)
            if process.poll() is not None: break
            time.sleep(5)
        stdout=process.stdout.read() if process.stdout else ''
        print(stdout,end=''); rc=process.wait()
    values=dict(line.split('=',1) for line in stdout.splitlines() if '=' in line)
    status=values.get('oram_snapshot_status','none'); rss=int(values.get('peak_host_rss_bytes',0))
    if rc!=0:
        print(f'warning: runner exited {rc} after {done}/{len(jobs)} jobs; no CSV rows were written for this run',flush=True)
        raise subprocess.CalledProcessError(rc,command[0])
    if image and status not in ('built','loaded'): raise ValueError('missing snapshot outcome')
    if done!=len(jobs): raise ValueError('BiSON report job count mismatch')
    for row in rows: row.update(peak_host_rss_bytes=rss,oram_snapshot_status=status,oram_snapshot_path=str(image) if image else '')
    return rows

def append_rows(path,rows):
    path.parent.mkdir(parents=True,exist_ok=True)
    with path.open('a+',newline='') as out:
        fcntl.flock(out,fcntl.LOCK_EX); out.seek(0); existing=next(csv.reader(out),None)
        if existing is not None and existing!=FIELDS: raise ValueError('CSV header mismatch; choose another --csv')
        out.seek(0,os.SEEK_END); writer=csv.DictWriter(out,FIELDS,extrasaction='ignore')
        if existing is None: writer.writeheader()
        writer.writerows(rows)

def existing_jobs(path,mode):
    if not path.is_file(): return set()
    with path.open() as f:
        return {(int(r['iterations']),int(r['beamwidth']),int(r['l_search']))
                for r in csv.DictReader(f) if r.get('mode')==mode and r.get('recall_at_k')}

def run(args):
    resolve_paths(args)
    if args.build_dir is None: args.build_dir=ROOT/('build/bison-release' if args.mode=='native' else 'build/bison-'+args.mode)
    args.build_dir=args.build_dir.resolve()
    if not 0<args.k<=65536 or min(args.threads,args.queries,args.repeats,args.z,args.s,args.a,args.evict_batch)<=0 or args.warmup<0 or args.stash<0: raise ValueError('invalid public parameters')
    if max(args.z,args.s,args.a,args.evict_batch)>=(1<<32) or args.stash>=(1<<64): raise ValueError('ORAM parameter overflow')
    if args.no_oram_snapshot and (args.oram_snapshot or args.rebuild_oram): raise ValueError('snapshot flags conflict')
    jobs=jobs_for(args)
    if not args.prepared.is_file():
        raise ValueError(f'BiSON prepared index not found: {args.prepared}; run bison/prepare.py --dataset {args.dataset} first')
    raw,layout,model,medoids,nodes,cache,h=prepared_full(args.prepared)
    if any(t*w*layout['degree']>=(1<<63) or l+w*layout['degree']>=(1<<63) for t,w,l in jobs): raise ValueError('BiSON capacity overflow')
    queries=read_vectors(args.query,args.query_format,args.query_dtype); truth=read_groundtruth(args.groundtruth,args.gt_format)
    count=min(args.queries,len(queries),len(truth))
    if not count or queries.shape[1]!=layout['dimension'] or truth.shape[1]<args.k: raise ValueError('query/GT dimension or k mismatch')
    if args.threads>256 or args.repeats>10000 or count*args.repeats*len(jobs)>1000000 or args.warmup*len(jobs)>1000000: raise ValueError('BiSON job/query limits exceeded')
    queries=np.ascontiguousarray(queries[:count],dtype='<f4')
    if not np.all(np.isfinite(queries)) or np.any(np.abs(queries)>1e4): raise ValueError('invalid query values')
    if args.resume:
        skip=existing_jobs(args.csv,args.mode)
        jobs=[j for j in jobs if j not in skip]
        if not jobs:
            print(f'all jobs already in {args.csv}; nothing to do')
            return []
    if args.reference_only:
        summaries=[]
        for job in jobs:
            result,ms=reference_results(queries,model,medoids,nodes,cache,h,job,args.k)
            row={key:'' for key in FIELDS}
            row.update(dataset=args.dataset,mode='reference',metric='l2',n=len(nodes),dimension=layout['dimension'],degree=layout['degree'],pq_bytes=layout['pq_subspaces'],scalar=layout['scalar'],
                iterations=job[0],beamwidth=job[1],l_search=job[2],k=args.k,cached_rounds=0,cache_nodes=0,logical_reads=job[0]*job[1],trusted_cache_bytes=0,cache_ms=0,
                search_variant='appendix-f',visited_capacity=job[0]*job[1],rerank_vectors=job[0]*job[1],
                recall_at_k=recall([[i for i,_ in v] for v in result],truth[:count],args.k),reference_ms=ms,reference_result_match=True,prepared_bytes=len(raw))
            summaries.append(row)
        append_rows(args.csv,summaries)
        print(json.dumps(summaries[0] if len(summaries)==1 else summaries,indent=2))
        return summaries
    if args.job_isolation:
        skip=existing_jobs(args.csv,args.mode)
        summaries=[]
        for job in jobs:
            if job in skip:
                print(f'skip T={job[0]}/W={job[1]}/L={job[2]}: already in {args.csv}',flush=True); continue
            rows=run_jobs(args,[job],raw,layout,model,medoids,nodes,cache,h,queries,truth,count)
            append_rows(args.csv,rows)
            summaries+=rows
            print(json.dumps(rows[0],indent=2),flush=True)
        return summaries
    summaries=run_jobs(args,jobs,raw,layout,model,medoids,nodes,cache,h,queries,truth,count)
    append_rows(args.csv,summaries)
    print(json.dumps(summaries[0] if len(summaries)==1 else summaries,indent=2))
    return summaries

def parser():
    p=argparse.ArgumentParser(description=__doc__); add_dataset_arguments(p)
    p.add_argument('--prepared',type=Path); p.add_argument('--query',type=Path); p.add_argument('--groundtruth',type=Path)
    p.add_argument('--query-format',choices=['auto','fvecs','bin'],default='auto'); p.add_argument('--query-dtype',choices=['uint8','float32'])
    p.add_argument('--gt-format',choices=['auto','ivecs','bin'],default='auto'); p.add_argument('--csv',type=Path)
    p.add_argument('--mode',choices=['native','sgx-sim','sgx-hw'],default='native'); p.add_argument('--build-dir',type=Path)
    p.add_argument('--oram-snapshot',type=Path); p.add_argument('--no-oram-snapshot',action='store_true'); p.add_argument('--rebuild-oram',action='store_true')
    p.add_argument('--reference',action='store_true'); p.add_argument('--reference-only',action='store_true',help='plaintext recall without constructing ORAM')
    p.add_argument('--job-isolation',action='store_true',help='run each (T,W,L) job in its own runner invocation, appending one CSV row per job; jobs already in the CSV are skipped')
    p.add_argument('--resume',action='store_true',help='skip (T,W,L) jobs already present in the CSV for this mode before running')
    for name,default in [('iterations',7),('beamwidth',13),('l-search',125)]:
        p.add_argument('--'+name,'--'+name+'-list',dest=name.replace('-','_'),type=int,nargs='+',default=[default])
    for name in ('k','threads','queries','warmup','repeats','z','s','a','evict-batch','stash'): p.add_argument('--'+name,type=int)
    return p
if __name__=='__main__':
    cli=parser()
    try: run(cli.parse_args())
    except (ValueError,OSError,RunnerUnavailableError,subprocess.CalledProcessError) as e: cli.exit(2,f'error: {e}\n')
