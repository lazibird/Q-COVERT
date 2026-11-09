# Copyright (c) Facebook, Inc. and its affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# add_executable(demo_imi_flat EXCLUDE_FROM_ALL demo_imi_flat.cpp)
# target_link_libraries(demo_imi_flat PRIVATE faiss)

# add_executable(demo_imi_pq EXCLUDE_FROM_ALL demo_imi_pq.cpp)
# target_link_libraries(demo_imi_pq PRIVATE faiss)

# add_executable(demo_ivfpq_indexing EXCLUDE_FROM_ALL demo_ivfpq_indexing.cpp)
# target_link_libraries(demo_ivfpq_indexing PRIVATE faiss)

# add_executable(demo_nndescent EXCLUDE_FROM_ALL demo_nndescent.cpp)
# target_link_libraries(demo_nndescent PRIVATE faiss)

add_executable(demo_sift1M EXCLUDE_FROM_ALL demo_sift1M.cpp)
target_link_libraries(demo_sift1M PRIVATE faiss)

add_executable(demo_generate_gt EXCLUDE_FROM_ALL demo_generate_gt.cpp)
target_link_libraries(demo_generate_gt PRIVATE faiss)

add_executable(hnsw_trip EXCLUDE_FROM_ALL demo_hnsw_trip.cpp)
target_link_libraries(hnsw_trip PRIVATE faiss)

add_executable(hnsw_laion EXCLUDE_FROM_ALL demo_hnsw_laion.cpp)
target_link_libraries(hnsw_laion PRIVATE faiss)

add_executable(hnsw_msmarco EXCLUDE_FROM_ALL demo_hnsw_msmarco.cpp)
target_link_libraries(hnsw_msmarco PRIVATE faiss)

add_executable(hnsw_sift  EXCLUDE_FROM_ALL demo_hnsw_sift.cpp)
target_link_libraries(hnsw_sift PRIVATE faiss)

add_executable(kmeans_sift  EXCLUDE_FROM_ALL demo_kmeans_sift.cpp)
target_link_libraries(kmeans_sift PRIVATE faiss)

add_executable(kmeans_laion  EXCLUDE_FROM_ALL demo_kmeans_laion.cpp)
target_link_libraries(kmeans_laion PRIVATE faiss)

add_executable(kmeans_trip  EXCLUDE_FROM_ALL demo_kmeans_trip.cpp)
target_link_libraries(kmeans_trip PRIVATE faiss)

add_executable(kmeans_msmarco  EXCLUDE_FROM_ALL demo_kmeans_msmarco.cpp)
target_link_libraries(kmeans_msmarco PRIVATE faiss)

add_executable(lsh_trip  EXCLUDE_FROM_ALL demo_lsh_trip.cpp)
target_link_libraries(lsh_trip PRIVATE faiss)

add_executable(demo_weighted_kmeans EXCLUDE_FROM_ALL demo_weighted_kmeans.cpp)
target_link_libraries(demo_weighted_kmeans PRIVATE faiss)

# add_executable(demo_residual_quantizer EXCLUDE_FROM_ALL demo_residual_quantizer.cpp)
# target_link_libraries(demo_residual_quantizer PRIVATE faiss)
