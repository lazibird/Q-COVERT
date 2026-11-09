"""Independent plaintext oracle for Appendix F BiSON-TEE. Public data only."""
import numpy as np
from prepare import INVALID

def table_for(query, model):
    book, mean, chunks, rotation, rotated = model
    centered=np.asarray(query,dtype='f4')-mean
    transformed=centered.copy()
    if rotated:
        transformed=np.zeros(len(query),'f4')
        for d in range(len(query)):
            for j in range(len(query)):
                transformed[d]=np.float32(transformed[d]+np.float32(centered[j]*rotation[j,d]))
    table=np.zeros((len(chunks)-1,256),'f4')
    for p in range(len(chunks)-1):
        for d in range(int(chunks[p]),int(chunks[p+1])):
            diff=(book[d]-transformed[d]).astype('f8')
            table[p] += (diff*diff).astype('f4')
    return table

def approximate(code, table):
    value=np.float32(0)
    for p,c in enumerate(code): value=np.float32(value+table[p,int(c)])
    return float(value)

def exact(query, embedding):
    value=np.float32(0)
    for q,v in zip(query,embedding):
        diff=np.float32(np.float32(q)-np.float32(v))
        value=np.float32(value+np.float32(diff*diff))
    return float(value)

def search(query, model, medoids, nodes, cache, h, iterations, beamwidth, l_search, k, trace=None, table=None):
    if not iterations or not beamwidth or not k or l_search<k: raise ValueError('invalid reference parameters')
    if table is None: table=table_for(query,model)
    entry=min(range(len(medoids)),key=lambda i:(exact(query,medoids[i]['centroid']),i))
    medoid=medoids[entry]; eid=int(medoid['id'])
    # Legacy cache/h arguments are accepted only for prepared-file compatibility.
    candidates={eid:approximate(medoid['pq'],table)}
    visited=set(); retrieved=[]
    if trace is not None: trace.update(entry=eid,expanded=[],fetched=[],candidates=[])
    for _ in range(iterations):
        beam=sorted(candidates,key=lambda i:(candidates[i],i))[:beamwidth]
        padded=beam+[INVALID]*(beamwidth-len(beam))
        if trace is not None:
            trace['expanded'].extend(padded); trace['fetched'].extend(padded)
        visited.update(beam)
        retrieved.extend(beam)
        merged=dict(candidates)
        for i in beam:
            node=nodes[i]
            for edge in node['neighbors'][:int(node['degree'])]:
                nid=int(edge['id'])
                if nid!=INVALID and nid not in visited:
                    score=approximate(edge['pq'],table)
                    merged[nid]=min(merged.get(nid,float('inf')),score)
        candidates=dict(sorted(((i,d) for i,d in merged.items() if i not in visited),
                               key=lambda v:(v[1],v[0]))[:l_search])
        if trace is not None:
            trace['candidates'].append([(i,d) for i,d in candidates.items()]+
                                      [(INVALID,float('inf'))]*(l_search-len(candidates)))
    # Exact distances are computed only after every PQ search iteration.
    full=[(i,exact(query,nodes[i]['embedding'])) for i in retrieved]
    return sorted(full,key=lambda v:(v[1],v[0]))[:k]+[(INVALID,float('inf'))]*max(0,k-len(full))
