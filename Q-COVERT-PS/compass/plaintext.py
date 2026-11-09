"""Independent offline exact oracle; ordinary sets/dicts, never a TEE path."""
import numpy as np
INVALID = (1 << 64) - 1

def reference_scan(query, nodes, upper, scan, scan_level, book, metric, ef, spec, ef_n, k, upper_steps=1, trace=None):
    if min(ef, spec, ef_n, k) <= 0 or upper_steps != 1:
        raise ValueError('requires positive ef/spec/ef_n/k and upper_steps=1')
    q = query.astype('f8')
    def exact(record):
        v = record['embedding'].astype('f8')
        return float(np.sum((q - v)**2) if metric == 0 else -np.sum(q * v))
    p, _, subdim = book.shape
    subq = q.reshape(p, 1, subdim)
    table = (np.sum((subq - book.astype('f8'))**2, axis=2) if metric == 0
             else -np.sum(subq * book.astype('f8'), axis=2))
    def rank(edges, visited, count):
        codes = {}
        for edge in edges:
            node = int(edge['id'])
            if node != INVALID and node not in visited:
                codes.setdefault(node, edge['pq'])
        ranked = sorted((float(sum(table[j, int(c)] for j, c in enumerate(code))), node)
                        for node, code in codes.items())
        return [node for _, node in ranked[:count]]
    nearest = min(scan, key=lambda record: (exact(record), int(record['id'])))
    if trace is not None:
        trace.update(scan_entry=int(nearest['id']), upper_expanded=[], upper_fetched=[], expanded=[], fetched=[])
    if len(upper):
        upper_by_id = {int(record['id']): record for record in upper}
        current = int(nearest['id'])
        ids = rank(nearest['neighbors'], {current}, ef_n)
        if trace is not None:
            trace['upper_expanded'].append(current)
            trace['upper_fetched'].extend(ids + [INVALID] * (ef_n - len(ids)))
        nearest = min([nearest] + [upper_by_id[node] for node in ids],
                      key=lambda record: (exact(record), int(record['id'])))
    entry = int(nearest['id'])
    if trace is not None:
        trace['bottom_entry'] = entry
        trace['fetched'].append(entry)
    visited = {entry}
    candidates = {entry: exact(nodes[entry])}
    best = [(candidates[entry], entry)]
    rounds, batch = (ef + spec - 1) // spec, spec * ef_n
    for _ in range(rounds):
        beam = sorted((score, node) for node, score in candidates.items())[:spec]
        for _, node in beam:
            del candidates[node]
        if trace is not None:
            trace['expanded'].extend([node for _, node in beam] + [INVALID] * (spec - len(beam)))
        edges = [edge for _, node in beam for edge in nodes[node]['neighbors']]
        ids = rank(edges, visited, batch)
        if trace is not None:
            trace['fetched'].extend(ids + [INVALID] * (batch - len(ids)))
        fetched = [(exact(nodes[node]), node) for node in ids]
        visited.update(ids)
        candidates.update((node, score) for score, node in fetched)
        best = sorted(best + fetched)[:max(ef, k)]
    return [node for _, node in best[:k]] + [INVALID] * max(0, k - len(best))
