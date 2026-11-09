"""Small deterministic DiskANN-format fixtures for public implementation tests."""
from pathlib import Path
import struct
import numpy as np
from prepare import SECTOR

def write_vecs(path, values, integer=False):
    with Path(path).open('wb') as out:
        for row in values:
            out.write(struct.pack('<I',len(row)))
            out.write(np.asarray(row,dtype='<i4' if integer else '<f4').tobytes())

def write_bin(path, values, dtype):
    with Path(path).open('wb') as out:
        out.write(struct.pack('<II',*values.shape)); out.write(np.asarray(values,dtype=dtype).tobytes())

def fixture(prefix, dim, degree, p, fractional=False, multi_sector=False, old_pivots=False, rotation=True, scalar='float32'):
    prefix=Path(prefix); prefix.parent.mkdir(parents=True,exist_ok=True)
    rng=np.random.default_rng(121)
    n=48; base=rng.integers(0,32,(n,dim)).astype('f4')
    if fractional: base=base/8-2.25+rng.uniform(-0.025,0.025,base.shape).astype('f4')
    dtype=np.dtype('u1' if scalar=='uint8' else '<f4')
    payload=dtype.itemsize*dim
    length=payload+4+4*degree
    if multi_sector and length<=SECTOR: raise ValueError('multi-sector fixture needs a sufficiently large R')
    per=SECTOR//length
    sectors=(n+per-1)//per if per else n*((length+SECTOR-1)//SECTOR)
    raw=bytearray((1+sectors)*SECTOR)
    meta=[n,dim,0,length,per,0,0,0,len(raw)]
    struct.pack_into('<II9Q',raw,0,9,1,*meta)
    for i in range(n):
        start=(i//per+1)*SECTOR+(i%per)*length if per else SECTOR+i*((length+SECTOR-1)//SECTOR)*SECTOR
        raw[start:start+payload]=base[i].astype(dtype).tobytes()
        neighbors=[(i+j+1)%n for j in range(min(degree,5))]
        if i%11==10: neighbors=[]
        if i%9==0 and len(neighbors)>1: neighbors[-1]=neighbors[0] # repeated edges
        struct.pack_into('<I',raw,start+payload,len(neighbors))
        for j,v in enumerate(neighbors): struct.pack_into('<I',raw,start+payload+4+4*j,v)
    Path(str(prefix)+'_disk.index').write_bytes(raw)
    medids=np.asarray([[0],[13]],'<u4')
    write_bin(str(prefix)+'_disk.index_medoids.bin',medids,'<u4')
    write_bin(str(prefix)+'_disk.index_centroids.bin',base[[0,13]],'<f4')
    codes=rng.integers(0,256,(n,p),dtype='u1')
    write_bin(str(prefix)+'_pq_compressed.bin',codes,'u1')
    chunks=np.linspace(0,dim,p+1,dtype='<u4')
    if p>1 and chunks[1]>1: chunks[1]=1 # deliberately unequal chunks
    mean=np.linspace(-.125,.125,dim,dtype='f4')[:,None]
    book=rng.uniform(-2,32,(256,dim)).astype('f4')
    sections=[]
    def section(arr,dtype): return struct.pack('<II',*arr.shape)+np.asarray(arr,dtype=dtype).tobytes()
    # Both DiskANN offset variants, with the extra old-format section retained.
    sections=[section(book,'<f4'),section(mean,'<f4')]
    if old_pivots: sections.append(section(np.arange(dim,dtype='<u4')[:,None],'<u4'))
    sections.append(section(chunks[:,None],'<u4'))
    count=5 if old_pivots else 4
    offsets=[]; running=8+8*count
    for part in sections: offsets.append(running); running+=len(part)
    offsets.append(running)
    pivots=Path(str(prefix)+'_pq_pivots.bin')
    pivots.write_bytes(struct.pack('<II',count,1)+struct.pack(f'<{count}Q',*offsets)+b''.join(sections))
    if rotation:
        mat=np.eye(dim,dtype='f4')
        if dim>1: mat[0,0]=0;mat[1,1]=0;mat[0,1]=1;mat[1,0]=-1
        write_bin(str(pivots)+'_rotation_matrix.bin',mat,'<f4')
    return base
