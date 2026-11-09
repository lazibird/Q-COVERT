./build/oram_initializer d=laion100k_R128 c=config/rr_new_mal.json type=1
./build/oram_initializer d=sift1m_R128 c=config/rr_new_mal.json type=1
./build/oram_initializer d=trip_R128 c=config/rr_new_mal.json type=1
./build/oram_initializer d=msmarco_R128 c=config/rr_new_mal.json type=1

./build/oram_initializer d=sift10m c=config/rr_new_mal.json type=1
./build/oram_initializer d=sift50m c=config/rr_new_mal.json type=1
./build/oram_initializer d=sift100m c=config/rr_new_mal.json type=1












# ./build/build_disk_index --data_type float --dist_fn l2 --data_path data/datasets/msmarco/base.fbin --index_path_prefix data/index/msmarco/__disk_index_msmarco_base_R256_L125_A1.2_PQ64 -R 256 -L 125 -B 0.535 -M 256 -A 1.2

# echo "INDEX BUILDING DONE"


# ./build/oram_initializer d=msmarco c=config/rr_config.json


# echo "ORAM INTIIALIZATION DONE"



