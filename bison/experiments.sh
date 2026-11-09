
./build/client d=sift1m_R128 c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=5 w_list=47 nc=12827 mal=1

./build/client d=sift1m_R128 c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=6 w_list=19 nc=12827 mal=1

./build/client d=sift1m_R128 c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=7 w_list=13 nc=12827 mal=1

./build/client d=sift1m_R128 c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=8 w_list=10 nc=12827 mal=1

# ./build/search_disk_index --data_type=float --dist_fn=l2 --index_path_prefix=/data/trip/disk_index_trip_base_R128_L125_A1.2_PQ64 --query_file=/data/trip/trip/queries.fbin --gt_file=/data/trip/trip/rohan_gt10 --result_path=results/trip --num_nodes_to_cache=0 -L 125 -W 12 --search_io_limit 7 -K 10
# ./build/search_disk_index --data_type=float --dist_fn=l2 --index_path_prefix=/data/trip/disk_index_trip_base_R128_L125_A1.2_PQ64 --query_file=/data/trip/trip/queries.fbin --gt_file=/data/trip/trip/rohan_gt10 --result_path=results/trip --num_nodes_to_cache=0 -L 125 -W 36 --search_io_limit 5 -K 10
# ./build/search_disk_index --data_type=float --dist_fn=l2 --index_path_prefix=/data/trip/disk_index_trip_base_R128_L125_A1.2_PQ64 --query_file=/data/trip/trip/queries.fbin --gt_file=/data/trip/trip/rohan_gt10 --result_path=results/trip --num_nodes_to_cache=0 -L 125 -W 5 --search_io_limit 1 -K 10

./build/search_disk_index --data_type=float --dist_fn=cosine --index_path_prefix=/data/msmarco/disk_index_msmarco_base_cosine_R128_L125_A1.2_PQ64 --query_file=/data/msmarco/queries.fbin --gt_file=/data/msmarco/msmarco_cosine_gt10 --result_path=results/msmarco --num_nodes_to_cache=0 -L 125 -W 37 --search_io_limit 6 -K 10
./build/search_disk_index --data_type=float --dist_fn=cosine --index_path_prefix=/data/msmarco/disk_index_msmarco_base_cosine_R128_L125_A1.2_PQ64 --query_file=/data/msmarco/queries.fbin --gt_file=/data/msmarco/msmarco_cosine_gt10 --result_path=results/trip --num_nodes_to_cache=0 -L 125 -W 20 --search_io_limit 7 -K 10

# python3 -m venv venv
# source venv/bin/activate

# python3 single_query.py config/config_yk1.json trip 7 12
# python3 single_query.py config/config_yk1.json trip 5 36

# python3 single_query.py config/config_yk1.json msmarco 7 20
# python3 single_query.py config/config_yk1.json msmarco 6 37

# ./build/client d=laion100k_R128 c=config/rr_mal.json ocl=4 sip=10.209.224.140 i_list=5 w_list=12 nc=129 mal=1
# ./build/client d=sift1m_R128 c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=5 w_list=47 nc=12827 mal=1
# ./build/client d=trip_R128 c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=5 w_list=36 nc=15825 mal=1
# ./build/client d=msmarco_R128 c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=6 w_list=37 nc=16156 mal=1
# ./build/client d=laion100k_R128 c=config/rr_mal.json ocl=4 sip=10.209.224.140 i_list=5 w_list=12 nc=129 mal=0

# ./build/client d=sift1m_R128 c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=7 w_list=13 nc=12827 mal=0

# ./build/client d=trip_R128 c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=7 w_list=12 nc=15825 mal=0

# ./build/client d=msmarco_R128 c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=7 w_list=20 nc=16156 mal=0


# ./build/client d=sift10m c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=6 w_list=18 nc=16075 mal=1
# ./build/client d=sift50m c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=6 w_list=48 nc=16266 mal=1
# ./build/client d=sift100m c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=7 w_list=19 nc=16369 mal=1

# ./build/client d=sift10m c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=7 w_list=9 nc=16075 mal=0

# ./build/client d=sift50m c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=9 w_list=7 nc=16266 mal=0

# ./build/client d=sift100m c=config/rr_mal.json ocl=6 sip=10.209.224.140 i_list=9 w_list=8 nc=16369 mal=0






# ./build/client d=sift_1M_R128 c=config/rr_config.json i_list=5,6,7,8,9,10 w_list=20,20,20,20,20,20 nc=1

# ./build/client d=sift_1M_R128 c=config/rr_config.json i_list=5,6,7,8,9,10 w_list=20,20,20,20,20,20 nc=1





# ./build/client d=trip_R128 c=config/rr_config.json i_list=5,6,7 w_list=36,18,12 nc=15825 >> anoth_EXP_wan__trip128.txt

# ./build/client d=msmarco_R128 c=config/rr_config.json i_list=6,7,8 w_list=37,20,16 nc=16156 >> anoth_EXP_wan__msmarco128.txt


# echo "DONE"




# ./build/client d=sift50m c=config/rr_oram_build.json i_list=6,7,8 w_list=48,15,9 nc=16266 >> lev9_EXP_fift.txt


# ./build/client d=sift100m c=config/rr_oram_build.json i_list=6,7,8 w_list=94,19,11 nc=16369 >> lev9_EXP_hundm.txt


# sudo tcset eno1np0 --rate 10Gbps --delay 0.10ms --direction outgoing --overwrite
# sudo tcset eno1np0 --rate 10Gbps --delay 0.10ms --direction incoming --add

# ./build/client d=sift10m c=config/rr_oram_build.json i_list=6,7,8,9,10 w_list=18,9,7,5,4 nc=16075 >> lev9_EXP_tenm.txt


# ./build/client d=sift10m c=config/rr_oram_build.json i_list=6,7 w_list=18,9 nc=16075 


# ./build/client d=sift50m c=config/rr_oram_build.json i_list=6,7,8,9,10 w_list=48,15,9,7,6 nc=16266 >> lev9_EXP_fift.txt


# ./build/client d=sift100m c=config/rr_oram_build.json i_list=6,7,8,9,10 w_list=94,19,11,8,7 nc=16369 >> lev9_EXP_hundm.txt











# echo "DONE"






# sudo tcset eno1np0 --rate 400Mbps --delay 40ms --direction outgoing --overwrite
# sudo tcset eno1np0 --rate 400Mbps --delay 40ms --direction incoming --add



# ./build/client d=sift10m c=config/rr_oram_build.json i_list=6,7,8,9,10 w_list=18,9,7,5,4 nc=16075 >> new_EXP_tenm.txt


# ./build/client d=sift50m c=config/rr_oram_build.json i_list=6,7,8,9,10 w_list=48,15,9,7,6 nc=16266 >> new_EXP_fift.txt


# ./build/client d=sift100m c=config/rr_oram_build.json i_list=6,7,8,9,10 w_list=94,19,11,8,7 nc=16369 >> new_EXP_hundm.txt






















# # ./build/client d=sift10m c=config/rr_oram_build.json i_list=6,7,8 w_list=18,9,7 nc=16075 >> EXP_wan__tenm_three1.txt


# # ./build/client d=sift50m c=config/rr_oram_build.json i_list=6,7,8 w_list=48,15,9 nc=16266 >> EXP_wan_fift_three.txt


# # ./build/client d=sift100m c=config/rr_oram_build.json i_list=6,7,8 w_list=94,19,11 nc=16369 >> EXP_wan_hundm_three.txt



# # # ./build/client d=laion100k_R128 c=config/rr_config.json i_list=5,6,7,8,9,10 w_list=12,8,6,5,4,4 nc=0 >> EXP_wan__laion.txt
# # ./build/client d=laion100k_R128 c=config/rr_config.json i_list=5,6,7 w_list=12,8,6 nc=10643 >> EXP_wan_laion.txt



# # # ./build/client d=sift_1M_R128 c=config/rr_config.json i_list=5,6,7,8,9,10 w_list=31,10,7,5,4,4 nc=0 >> EXP_wan__sift1m.txt
# # ./build/client d=sift_1M_R128 c=config/rr_config.json i_list=5,6,7 w_list=31,10,7 nc=12827 >> EXP_wan_sift1m.txt



# # # ./build/client d=trip_R128 c=config/rr_config.json i_list=5,6,7,8,9,10 w_list=36,18,12,10,8,7 nc=0 >> EXP_wan__trip128.txt


# # # ./build/client d=trip_R256 c=config/rr_config.json i_list=5,6,7,8,9,10 w_list=23,14,11,8,7,6 nc=0 >> EXP_wan__trip256.txt
# # ./build/client d=trip_R256 c=config/rr_config.json i_list=5,6,7 w_list=23,14,11 nc=59539 >> EXP_wan__trip256.txt


# # # ./build/client d=msmarco_R128 c=config/rr_config.json i_list=6,7,8,9,10 w_list=37,20,16,12,10 nc=0 >> EXP_wan__msmarco128.txt


# # # ./build/client d=msmarco_R256 c=config/rr_config.json i_list=5,6,7,8,9,10 w_list=51,25,16,12,10,10 nc=0 >> EXP_wan__msmarco256.txt
# # ./build/client d=msmarco_R256 c=config/rr_config.json i_list=5,6,7 w_list=51,25,16 nc=62317 >> EXP_wan__msmarco256.txt


#MIPS
#./build/build_disk_index --data_type float --dist_fn mips --data_path /data/trip/trip/base.fbin --index_path_prefix /data/trip/disk_index_trip_base_mips_R128_L125_A1.2_PQ64 -R 128 -L 125 -B 0.092 -M 256 -A 1.2
# ./build/compute_groundtruth --data_type float   --dist_fn mips   --base_file /data/trip/base.fbin   --query_file /data/trip/queries.fbin   --gt_file /data/trip/trip_mips_gt10   --K 10
#./build/search_disk_index --data_type=float --dist_fn=mips --index_path_prefix=/data/trip/disk_index_trip_base_mips_R128_L125_A1.2_PQ64 --query_file=/data/trip/queries.fbin --gt_file=/data/trip/trip_mips_gt10 --result_path=results/trip --num_nodes_to_cache=0 -L 125 -W 125 --search_io_limit 10 -K 10

#./build/compute_groundtruth --data_type float   --dist_fn mips   --base_file /data/laion100k/laion_base.fbin  --query_file /data/laion100k/laion100k_query.fbin   --gt_file /data/laion100k/laion100k_mips_gt10   --K 10

#./build/build_disk_index --data_type float --dist_fn mips --data_path /data/laion100k/laion_base.fbin --index_path_prefix /data/laion100k/disk_index_laion100k_base_mips_R128_L125_A1.2_PQ64 -R 128 -L 125 -B 0.092 -M 256 -A 1.2
#./build/search_disk_index --data_type=float --dist_fn=mips --index_path_prefix=/data/laion100k/disk_index_laion100k_base_mips_R128_L125_A1.2_PQ64 --query_file=/data/laion100k/laion_query.fbin --gt_file=/data/laion100k/laion100k_mips_gt10 --result_path=results/laion100k --num_nodes_to_cache=0 -L 125 -W 125 --search_io_limit 10 -K 10


