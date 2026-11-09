# ./build/oram_initializer d=laion100k_R128 c=config/rr_mal.json type=2
# ./build/oram_initializer d=sift_1M_R128 c=config/rr_mal.json type=2
# ./build/oram_initializer d=trip_R128 c=config/rr_mal.json type=2
# ./build/oram_initializer d=msmarco_R128 c=config/rr_mal.json type=2

# ./build/oram_initializer d=sift10m c=config/rr_mal.json type=2
# ./build/oram_initializer d=sift50m c=config/rr_mal.json type=2
# ./build/oram_initializer d=sift100m c=config/rr_mal.json type=2




# ./build/build_disk_index --data_type float --dist_fn l2 --data_path /data/small/new_data/datasets/laion100k/laion_base.fbin --index_path_prefix /data/small/new_data/index/laion100k/disk_index_laion100k_base_R128_L125_A1.2_PQ64 -R 128 -L 125 -B 0.006 -M 256 -A 1.2

# ./build/oram_initializer d=laion100k_R128 c=config/rr_config.json r=128 e=100 gp=/data/small/new_data/graphs/graph100000_128.txt

########################################

# ./build/build_disk_index --data_type float --dist_fn l2 --data_path /data/data/datasets/sift200m/base.fbin --index_path_prefix /data/data/index/sift200m/disk_index_sift200m_base_R128_L100_A1.2 -R 128 -L 100 -B 2 -M 512 -A 1.2






# ./build/build_disk_index --data_type float --dist_fn l2 --data_path data/datasets/trip/base.fbin --index_path_prefix data/index/trip/__disk_index_trip_base_R256_L125_A1.2_PQ64 -R 256 -L 125 -B 0.092 -M 256 -A 1.2

# echo "INDEX BUILDING DONE"


# ./build/oram_initializer d=trip c=config/rr_config.json


# echo "ORAM INTIIALIZATION DONE"



# ./build/build_disk_index --data_type float --dist_fn l2 --data_path data/datasets/laion100k/laion_base.fbin --index_path_prefix data/index/laion100k/disk_index_laion100k_base_R64_L125_A1.2_PQ64 -R 64 -L 125 -B 0.035 -M 128 -A 1.2
# ./build/oram_initializer d=laion100k c=config/rr_config.json r=64


# tmux ls
# tmux kill-session -t 0
# tmux new -s build_marco


# ./build/build_disk_index --data_type float --dist_fn l2 --data_path data/datasets/msmarco/base.fbin --index_path_prefix data/index/msmarco/disk_index_msmarco_base_R128_L125_A1.2_PQ64 -R 128 -L 125 -B 0.535 -M 128 -A 1.2

# echo "INDEX BUILDING DONE"


# ./build/oram_initializer d=msmarco c=config/rr_config.json


# echo "ORAM INTIIALIZATION DONE"

# ./build/oram_initializer d=sift20m c=config/rr_oram_build.json r=128 e=100 gp=/data/data/graphs/sift20m/graph_R128_L100_PQ32.txt >> build_sift20m.txt

# ./build/oram_initializer d=sift100m c=config/rr_oram_build.json r=128 e=100 gp=/data/data/graphs/sift100m/graph_R128_L100_PQ32.txt >> build_sift100m.txt

