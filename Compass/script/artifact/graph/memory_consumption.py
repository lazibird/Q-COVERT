import os

def get_size_mb(fname):
    file_size_bytes = os.path.getsize(fname)
    file_size_mb = file_size_bytes / (1024 * 1024)
    return file_size_mb

def get_size_gb(fname):
    file_size_bytes = os.path.getsize(fname)
    file_size_gb = file_size_bytes / (1024 * 1024 * 1024)
    return file_size_gb

def compute_he_cluster_db_size(d):
    if d == "laion":
        nc = 407
        dim = 512
        node_per_cluster = 703
    elif d == "sift":
        nc = 1452
        dim = 128
        node_per_cluster = 1361
    elif d == "trip":
        nc = 1530
        dim = 768
        node_per_cluster = 3272
    elif d== "msmarco":
        nc = 3810
        dim = 768
        node_per_cluster = 7224
    
    per_ctx_size = 524401
    slot_count = 8192
    num_embed_per_ctx = slot_count // dim
    num_ctx_per_cluster = (node_per_cluster // num_embed_per_ctx) + 1

    num_ctx = num_ctx_per_cluster*nc
    return num_ctx*per_ctx_size / (1024*1024*1024)

def pretty_print_mem():
    os.chdir(os.path.expanduser('~/compass/'))

    print()
    print("# LAION: ")
    print("--> Embed Size: {:.2f} GB".format(get_size_gb("data/dataset/laion1m/100k/laion_base.fvecs")))
    print("--> Graph Size: {:.2f} GB".format(get_size_gb("data/dataset/laion1m/100k/hnsw_64_80_2_ip.index")))
    print("--> Compass Server SH: {:.2f} GB".format(get_size_gb("data/snap/server/laion/buckets.bin")))
    print("--> Compass Server MAL: {:.2f} GB".format(get_size_gb("data/snap/server/laion/buckets.bin") + get_size_gb("data/snap/client/laion/hash.bin")))
    print("--> Compass Client Hints: {:.2f} MB".format(get_size_mb("data/dataset/laion1m/100k/pq_full_32_8_ip.index")))
    print("--> Compass Client ORAM: {:.2f} MB".format(get_size_mb("data/snap/client/laion/position_map.bin") + get_size_mb("data/snap/client/laion/metadata.bin")))
    print("--> Compass Client Graph: {:.2f} MB".format(get_size_mb("data/snap/client/laion/graph_cache.bin")))
    total = get_size_mb("data/dataset/laion1m/100k/pq_full_32_8_ip.index") + get_size_mb("data/snap/client/laion/position_map.bin") + get_size_mb("data/snap/client/laion/metadata.bin") + get_size_mb("data/snap/client/laion/graph_cache.bin")
    print("--> Compass Client Total: {:.2f} MB".format(total))
    print("--> HE-Cluster Server: {:.2f} GB".format(compute_he_cluster_db_size("laion")))
    print("--> HE-Cluster Client: {:.2f} MB".format(get_size_mb("data/dataset/laion1m/100k/cluster_centroids.index")))
    print("--> Inv-ORAM Server: N/A")
    print("--> Inv-ORAM Client: N/A")

    print("# SIFT1M: ")
    print("--> Embed Size: {:.2f} GB".format(get_size_gb("data/dataset/sift/base.fvecs")))
    print("--> Graph Size: {:.2f} GB".format(get_size_gb("data/dataset/sift/hnsw_64_80_2_ip.index")))
    print("--> Compass Server SH: {:.2f} GB".format(get_size_gb("data/snap/server/sift/buckets.bin")))
    print("--> Compass Server MAL: {:.2f} GB".format(get_size_gb("data/snap/server/sift/buckets.bin") + get_size_gb("data/snap/client/sift/hash.bin")))
    print("--> Compass Client Hints: {:.2f} MB".format(get_size_mb("data/dataset/sift/pq_full_8.index")))
    print("--> Compass Client ORAM: {:.2f} MB".format(get_size_mb("data/snap/client/sift/position_map.bin") + get_size_mb("data/snap/client/sift/metadata.bin")))
    print("--> Compass Client Graph: {:.2f} MB".format(get_size_mb("data/snap/client/sift/graph_cache.bin")))
    total = get_size_mb("data/dataset/sift/pq_full_8.index") + get_size_mb("data/snap/client/sift/position_map.bin") + get_size_mb("data/snap/client/sift/metadata.bin") + get_size_mb("data/snap/client/sift/graph_cache.bin")
    print("--> Client Total: {:.2f} MB".format(total))
    print("--> HE-Cluster Server: {:.2f} GB".format(compute_he_cluster_db_size("sift")))
    print("--> HE-Cluster Client: {:.2f} MB".format(get_size_mb("data/dataset/sift/cluster_centroids.index")))
    print("--> Inv-ORAM Server: N/A")
    print("--> Inv-ORAM Client: N/A")

    print("# TripClick: ")
    print("--> Embed Size: {:.2f} GB".format(get_size_gb("data/dataset/trip_distilbert/passages.fvecs")))
    print("--> Graph Size: {:.2f} GB".format(get_size_gb("data/dataset/trip_distilbert/hnsw_128_160_2_ip.index")))
    print("--> Compass Server SH: {:.2f} GB".format(get_size_gb("data/snap/server/trip/buckets.bin")))
    print("--> Compass Server MAL: {:.2f} GB".format(get_size_gb("data/snap/server/trip/buckets.bin") + get_size_gb("data/snap/client/trip/hash.bin")))
    print("--> Compass Client Hints: {:.2f} MB".format(get_size_mb("data/dataset/trip_distilbert/pq_full_32_ip.index")))
    print("--> Compass Client ORAM: {:.2f} MB".format(get_size_mb("data/snap/client/trip/position_map.bin") + get_size_mb("data/snap/client/trip/metadata.bin")))
    print("--> Compass Client Graph: {:.2f} MB".format(get_size_mb("data/snap/client/trip/graph_cache.bin")))
    total = get_size_mb("data/dataset/trip_distilbert/pq_full_32_ip.index") + get_size_mb("data/snap/client/trip/position_map.bin") + get_size_mb("data/snap/client/trip/metadata.bin") + get_size_mb("data/snap/client/trip/graph_cache.bin")
    print("--> Compass Client Total: {:.2f} MB".format(total))
    print("--> HE-Cluster Server: {:.2f} GB".format(compute_he_cluster_db_size("trip")))
    print("--> HE-Cluster Client: {:.2f} MB".format(get_size_mb("data/dataset/trip_distilbert/cluster_centroids.index")))
    print("--> Inv-ORAM Server: {:.2f} GB".format(get_size_gb("data/snap/tfidf_trip/buckets.bin")))
    print("--> Inv-ORAM Client: {:.2f} MB".format(get_size_mb("data/snap/tfidf_trip/position_map.bin")))

    print("# MSMARCO: ")
    print("--> Embed Size: {:.2f} GB".format(get_size_gb("data/dataset/msmarco_bert/passages.fvecs")))
    print("--> Graph Size: {:.2f} GB".format(get_size_gb("data/dataset/msmarco_bert/hnsw_128_200_2_ip.index")))
    print("--> Compass Server SH: {:.2f} GB".format(get_size_gb("data/snap/server/msmarco/buckets.bin")))
    print("--> Compass Server MAL: {:.2f} GB".format(get_size_gb("data/snap/server/msmarco/buckets.bin") + get_size_gb("data/snap/client/msmarco/hash.bin")))
    print("--> Compass Client Hints: {:.2f} MB".format(get_size_mb("data/dataset/msmarco_bert/pq_full_32_ip.index")))
    print("--> Compass Client ORAM: {:.2f} MB".format(get_size_mb("data/snap/client/msmarco/position_map.bin") + get_size_mb("data/snap/client/msmarco/metadata.bin")))
    print("--> Compass Client Graph: {:.2f} MB".format(get_size_mb("data/snap/client/msmarco/graph_cache.bin")))
    total = get_size_mb("data/dataset/msmarco_bert/pq_full_32_ip.index") + get_size_mb("data/snap/client/msmarco/position_map.bin") + get_size_mb("data/snap/client/msmarco/metadata.bin") + get_size_mb("data/snap/client/msmarco/graph_cache.bin")
    print("--> Compass Client Total: {:.2f} MB".format(total))
    print("--> HE-Cluster Server: {:.2f} GB".format(compute_he_cluster_db_size("msmarco")))
    print("--> HE-Cluster Client: {:.2f} MB".format(get_size_mb("data/dataset/msmarco_bert/cluste_centroids.index")))
    print("--> Inv-ORAM Server: {:.2f} GB".format(get_size_gb("data/snap/tfidf_msmarco/buckets.bin")))
    print("--> Inv-ORAM Client: {:.2f} MB".format(get_size_mb("data/snap/tfidf_msmarco/position_map.bin")))


if __name__ == "__main__":
    pretty_print_mem()