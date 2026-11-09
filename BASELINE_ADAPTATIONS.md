# Baseline Adaptations

We adapt [Compass](https://github.com/Clive2312/compass) and
[BiSON](https://github.com/r-dott/bison) in two ways: lightweight-client
variants for the client-owned (CO) scenario, named Compass-ls and BiSON-ls,
and TEE variants for the provider-served (PS) scenario, named Compass-TEE
and BiSON-TEE. All variants preserve the core graph-search procedures of
the original schemes.

## Original schemes

**Compass** stores an HNSW graph in a server-side Ring ORAM. Its client
caches all but the last two graph layers, together with PQ codes used as
quantized hints for directional neighbor filtering and the ORAM position
information. Compass restricts the second-to-last layer to one search step.

**BiSON** stores a DiskANN graph in a server-side ORAM, where each block
holds a node's full vector and neighbor IDs, and performs a fixed-round beam
search. Its client keeps the complete PQ codes, an explicit node-to-block
mapping in its implementation, the ORAM position information, and a
persistent graph-node cache.

Both schemes therefore retain database-scale auxiliary state at the client.

## Lightweight-client variants

Compass-ls and BiSON-ls outsource the PQ codes and, for SIFT10M, the graph
position map to oblivious server-side storage while preserving the original
graph-search and candidate-selection procedures. In both variants, bucket
metadata is stored on the server and retrieved following the Ring ORAM
layout.

### Compass-ls

Compass-ls moves the PQ codes into a separate Ring ORAM, packing multiple
codes into each block. During graph traversal, the client obliviously
retrieves the required PQ blocks before evaluating and filtering neighboring
candidates. The graph structure, directional filtering, and speculative
prefetching mechanisms remain unchanged.

For the million-vector configurations, the graph position map is small and
remains at the client. For SIFT10M, it is outsourced through a recursive Ring
ORAM construction, so graph accesses additionally require oblivious
position-map lookups and updates. The client retains the remaining small
position map, together with the PQ codebooks and Compass's upper-layer graph
cache.

### BiSON-ls

BiSON-ls retains beam search and server-side graph ORAM, but replaces the
client's PQ table with a separate server-side PQ ORAM. The client retrieves
the required PQ codes obliviously to evaluate candidates during graph
expansion. A temporary cache reuses PQ blocks within each query and is
cleared when the query completes.

We eliminate the explicit node-to-block mapping table by assigning each
graph node a logical ORAM block with the same identifier, while physical
storage locations remain protected by ORAM. BiSON-ls also disables the
persistent graph-node cache to reduce client storage. For the million-vector
configurations, the graph position map remains local. For SIFT10M, it is
stored in a recursive ORAM on the server, and its lookups are performed
obliviously.

## TEE variants

To evaluate the generality of the Q-COVERT TEE backend, we adapt both schemes
to the PS setting by moving their client-side search logic into the enclave
and reorganizing private state for doubly-oblivious execution. All
data-dependent accesses use fixed scans or Q-COVERT's oblivious interfaces.
Both adaptations use the same `ReadPath`, `EarlyReshuffle`, and `BatchEvict`
as Q-COVERT-PS, so performance differences do not stem from the backend.

### Compass-TEE

We reorganize the Compass index into three levels. Layer 2 and above are
flattened into a table, where each record stores the node ID, full vector,
and fixed-size Layer-1 neighbor records consisting of neighbor IDs and their
PQ codes. Layers 1 and 0 are stored in two doubly-oblivious Ring ORAMs with
separate position maps, each accessed by a fixed linear scan. An ORAM block
stores the node ID and full vector together with the fixed-size neighbor
records of the corresponding layer.

For a query, the enclave first scans the Layer-2 table and selects the closest
node $e_1$ as the Layer-1 entry. It then invokes oblivious top-k over the PQ
distances of $e_1$'s Layer-1 neighbors to select `ef_n` candidates and
batch-reads their Layer-1 blocks. The enclave computes their exact distances
and performs a fixed scan together with $e_1$ to select the closest node as
the Layer-0 entry $e_0$. It then reads the Layer-0 block of $e_0$ to initialize
the Layer-0 search.

At Layer 0, we retain Compass's fixed-round search. The enclave maintains
the visited set `Vis`, the candidate set `Cand`, and the set `Best` of the
best `ef` nodes, and performs

$$
n_{\mathrm{iter}} = \left\lceil
\frac{\mathrm{ef}}{\mathrm{ef}_{\mathrm{spec}}}
\right\rceil
$$

iterations, where `ef_spec` is the number of candidates expanded per
iteration. In each iteration, oblivious top-k extracts the closest `ef_spec`
nodes from `Cand`. Their neighbors and `Vis` are processed by oblivious
sorting (`OSort`) on node IDs, followed by a fixed scan that marks visited
and duplicate entries as invalid and assigns them infinite distance.
Oblivious top-k then selects `ef_spec * ef_n` candidates using their PQ
distances, followed by one Ring ORAM batch read. The returned nodes are
appended to `Vis` and `Cand`, while oblivious top-k over `Best` and the
returned nodes retains the closest `ef` nodes in `Best`. After the fixed
iterations, the closest `k` nodes in `Best` are returned.

### BiSON-TEE

We augment each BiSON graph block with fixed-size neighbor records
containing their IDs and PQ codes, so the enclave needs no separate PQ
table. The graph is stored in a doubly-oblivious Ring ORAM whose position
map is accessed by a linear scan.

Starting from the entry node, the enclave maintains a fixed-size beam list
`Beam` of $h$ entries and a visited set `Vis` for $n_{\mathrm{iter}}$
iterations. In each iteration, oblivious top-k selects the closest $\omega$
unvisited candidates in `Beam` using PQ distances, and their blocks are
retrieved through one ORAM batch read. The returned nodes are appended to
`Vis`, while their neighbor records are merged with `Beam`. The merged
candidate records and visited IDs are processed by `OSort` on node IDs,
followed by a fixed scan that marks visited and duplicate entries as invalid
and assigns them infinite distance. Oblivious top-k then retains the closest
$h$ candidates and writes them back to `Beam`, padding with dummy entries
when necessary. After $n_{\mathrm{iter}}$ iterations, the enclave computes
exact distances over the $n_{\mathrm{iter}}\omega$ retrieved vectors and
invokes oblivious top-k to return the final results.

In the experiment CSVs, `T`, `W`, and `L` correspond to
$n_{\mathrm{iter}}$, $\omega$, and $h$, respectively. Compass uses
`ef`, `ef_spec`, and `ef_n` directly. For installation and execution commands,
see [QUICKSTART.md](QUICKSTART.md) and the scheme run guides linked from
[README.md](README.md).
