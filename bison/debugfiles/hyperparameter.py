import subprocess
import os
from rich import print

# always run this script from the project root directory
project_root = os.getcwd()

# double-check this once
dataset_sizes = {
    "trip": 1523871,
    "sift": 1000000,
    "marco": 8841823
}

dimensions = {
    "trip": 768,
    "sift": 128,
    "marco": 768,
}

pq_bytes = {
    "trip": 64,
    "sift": 32,
    "marco": 64
}

non_search_params = {
    "trip": {
        "debug": False,
        "integrity": False,
        "base_size": dataset_sizes["trip"],
        "dummy_size": 64,
        "real_bucket_size": 32,
        "evict_rate": 36,
        "num_levels": 16,
        "oram_cached_levels": 6,
        "block_size": 1026,
        "large_number": 1000000,
        "use_oram": False,

        "buckets_path": "oram_data/trip/buckets.bin",
        "graph_cache_path": "oram_data/trip/graph_cache.bin",
        "hash_path": "oram_data/trip/hash.bin",
        "metadata_path": "oram_data/trip/metadata.bin",
        "pos_map_path": "oram_data/trip/position_map.bin",
        "block_map_path": "oram_data/trip/block_mapping.bin",

        "--data_type": "float",
        "--dist_fn": "l2",
        "dim": dimensions["trip"],
        #   "M": 128,
        #   "-W": 16,
        "-K": 10,
        #   "-L": 45,
        #   "--search_io_limit": 7,
        "--num_nodes_to_cache": 0,
        "--query_nums": 1175,

        #   "--index_path_prefix": "data/index/trip/disk_index_trip_base_R128_L50_A1.2",
        #   "base_path": "../compass/data/dataset/trip_distilbert/passages.fvecs",
        #   "--query_file": "data/dataset/trip/queries.fbin",
        #   "--gt_file": "data/dataset/trip/trip_query_base_gt_100",
        #   "--result_path": "results/trip"
    },
    
    "sift": {
        "debug": False,
        "integrity": False,
        "base_size": dataset_sizes["sift"],
        "dummy_size": 64,
        "real_bucket_size": 32,
        "evict_rate": 36, 
        "num_levels": 16,
        "oram_cached_levels": 4, 
        "block_size": 258,
        "large_number": 1,
        "use_oram": False,

        "buckets_path": "oram_data/sift/buckets.bin",
        "graph_cache_path": "oram_data/sift/graph_cache.bin",
        "hash_path": "oram_data/sift/hash.bin",
        "metadata_path": "oram_data/sift/metadata.bin",
        "pos_map_path": "oram_data/sift/position_map.bin",
        "block_map_path": "oram_data/sift/block_mapping.bin",

        "--data_type": "float",
        "--dist_fn": "l2",
        "dim": dimensions["sift"],
        # "M": 128, 
        # "-W": 4,
        "-K": 10,
        # "-L": 32,
        # "--search_io_limit": 10,
        "--num_nodes_to_cache": 0,
        "--query_nums": 10000,

        # "--index_path_prefix": "data/index/sift/disk_index_sift_base_R128_L50_A1.2",
        # "base_path": "data/datasets/sift/base.fvecs",
        # "--query_file": "data/datasets/sift/sift_query.fbin",
        # "--gt_file": "data/datasets/sift/sift_query_base_gt100",
        # "--result_path": "results/sift"
    },

    "marco": {
        "debug": False,
        "integrity": False,
        "base_size": dataset_sizes["marco"],
        "dummy_size": 64,
        "real_bucket_size": 32,
        "evict_rate": 36,
        "num_levels": 19,
        "oram_cached_levels": 6,
        "block_size": 1026,
        "large_number": 1000000,
        "use_oram": False,

        "buckets_path": "oram_data/marco/buckets.bin",
        "graph_cache_path": "oram_data/marco/graph_cache.bin",
        "hash_path": "oram_data/marco/hash.bin",
        "metadata_path": "oram_data/marco/metadata.bin",
        "pos_map_path": "oram_data/marco/position_map.bin",
        "block_map_path": "oram_data/marco/block_mapping.bin",

        "--data_type": "float",
        "--dist_fn": "l2",
        "dim": dimensions["marco"],
        #   "M": 128,
        #   "-W": 16,
        "-K": 10,
        #   "-L": 45,
        #   "--search_io_limit": 7,
        "--num_nodes_to_cache": 0,
        "--query_nums": 6980,

        #   "--index_path_prefix": "data/index/trip/disk_index_trip_base_R128_L50_A1.2",
        #   "base_path": "../compass/data/dataset/trip_distilbert/passages.fvecs",
        #   "--query_file": "data/dataset/trip/queries.fbin",
        #   "--gt_file": "data/dataset/trip/trip_query_base_gt_100",
        #   "--result_path": "results/trip"
    }
}

def get_dataset_paths(r, efc, alpha, dataset):
    paths = {
        # index build-time paths
        "--data_path": f"{project_root}/data/datasets/{dataset}/base.fbin",
        
        # search-time paths
        "--index_path_prefix": f"{project_root}/data/index/{dataset}/disk_index_{dataset}_base_R{r}_L{efc}_A{alpha}",
        "base_path": f"{project_root}/data/datasets/{dataset}/base.fvecs",
        "--query_file": f"{project_root}/data/datasets/{dataset}/{dataset}_query.fbin",
        "--gt_file": f"{project_root}/data/datasets/{dataset}/{dataset}_query_base_gt100",
        "--result_path": f"{project_root}/results/{dataset}"
    }
    
    return paths

def get_build_index_command(r, efc, alpha, dataset):
    paths = get_dataset_paths(r, efc, alpha, dataset)
    
    # compute pq-bytes size
    B = (pq_bytes[dataset]*dataset_sizes[dataset]) / (2**30)
    B_str = "{:.3f}".format(B)
    
    command_params = [
        f"{project_root}/src/diskann/build/apps/build_disk_index",

        "--data_type", "float",

        "--dist_fn", "l2",

        "--data_path", paths["--data_path"],

        "--index_path_prefix", paths["--index_path_prefix"],

        "-R", str(r),

        "-L", str(efc),

        "-B", str(B_str),

        "-M", str(10000)
    ]
    
    command = " ".join(command_params)
    
    return command
    
def run_fvecs_to_fbin(path):
    path = os.path.abspath(path)
    if not path.endswith(".fvecs"):
        print(f"[bold magenta]Error: {path} is not a valid fvecs file.[/bold magenta]")
        exit(1)
        
    folder = os.path.dirname(path)
    bin_path = os.path.basename(path).replace(".fvecs", ".fbin")
    
    if os.path.exists(bin_path):
        print("fbin file already exists...skipping")
        return


    # convert fvecs to fbin
    fvecs_to_fbin_command = f"{project_root}/src/diskann/build/apps/utils/fvecs_to_bin float {path} {bin_path}"
    print(f"Converting fvecs to fbin for dataset {dataset}: ")
    print(fvecs_to_fbin_command)
    
    try:
        subprocess.run(fvecs_to_fbin_command, shell=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f"[bold magenta]Error during fvecs to fbin conversion: {e}[/bold magenta]")
        exit(1)
    
    print("\n\n fbin conversion completed.\n\n")


def build_diskann_index(r, efc, alpha, dataset):  
    paths = get_dataset_paths(r, efc, alpha, dataset)
    
    if os.path.exists(paths["--index_path_prefix"] + "_disk.index") \
        and os.path.exists(paths["--index_path_prefix"] + "_pq_compressed.bin") \
        and os.path.exists(paths["--index_path_prefix"] + "_pq_pivots.bin"):
        print(f"Index already exists for R = {r}, L = {efc}, A = {alpha}, dataset = {dataset}. Skipping index build.")
        return

    # run_fvecs_to_fbin(r, efc, alpha, dataset)
    run_fvecs_to_fbin(paths["base_path"])
      
    # build index
    command = get_build_index_command(r, efc, alpha, dataset)
    
    print(f"Running index build for R = {r}, L = {efc}, A = {alpha}, dataset = {dataset}: ")
    print(command)
    
    try:
        subprocess.run(command, shell=True)
    except subprocess.CalledProcessError as e:
        print(f"[bold magenta]Error during index build: {e}[/bold magenta]")
        exit(1)
        
    print("\n\n")
    
    # assert that the index was built successfully
    
    if not os.path.exists(paths["--index_path_prefix"] + "_disk.index"):
        print(
            f"Index not written to the expected path: {paths['--index_path_prefix']}_disk.index"
        )
        exit(1)
    
    if not os.path.exists(paths["--index_path_prefix"] + "_pq_compressed.bin"):
        print(
            f"Compressed PQ-vectors not written to the expected path: {paths['--index_path_prefix']}_pq_compressed.bin"
        )
        exit(1)

    if not os.path.exists(paths["--index_path_prefix"] + "_pq_pivots.bin"):
        print(
            f"PQ-pivots not written to the expected path: {paths['--index_path_prefix']}_pq_pivots.bin"
        )
        exit(1)
        
    
    current_graph_path = f"{project_root}/data/graphs/graph.txt"
    if not os.path.exists(current_graph_path):
        print(
            f"graph.txt not found at the expected path: {current_graph_path}."
        )
        exit(1)
    
    final_graph_path = f"{project_root}/data/graphs/{dataset}/graph_R{r}_L{efc}_A{alpha}.txt"
    try:
        subprocess.run("mv " + current_graph_path + " " + final_graph_path, shell=True)
    except subprocess.CalledProcessError as e:
        print(f"[bold magenta]Error moving graph file: {e}[/bold magenta]")
        exit(1)
    
    print("Building index completed.\n\n")
    
    
def get_search_command(r, efc, alpha, ef, w, iterations, dataset):
    paths = get_dataset_paths(r, efc, alpha, dataset)  
    
    dataset_config_contents = non_search_params[dataset]
    dataset_config_contents.update({
        "dim": dimensions[dataset],
        "M": r,
        "-W": w,
        "-L": ef,
        "--search_io_limit": iterations
    })
    dataset_config_contents.update(paths)
    
    config_file_data = {
        dataset: dataset_config_contents
    }
    
    import json
    config_file = f"{project_root}/config/config_{dataset}.json"
    with open(config_file, 'w') as f:
        json.dump(config_file_data, f, indent=4)
    
    print(f"Running search for R = {r}, L = {efc}, A = {alpha}, ef = {ef}, w = {w}, iterations = {iterations}, dataset = {dataset}: ")

    # server 
    server_command_params = [
        "./server ",

        "d=", dataset
    ]
    server_command = "".join(server_command_params)
    
    
    # client
    client_command_params = [
        "./client ",

        "d=", dataset
    ]
    client_command = "".join(client_command_params)    
    
    return (client_command, server_command)


def run_diskann_search(r, efc, alpha, ef, w, iterations, dataset):
    # paths = get_dataset_paths(r, efc, alpha, dataset)
    # run_fvecs_to_fbin(paths["query_file"])
    
    client_command, server_command = get_search_command(r, efc, alpha, ef, w, iterations, dataset)

    server = subprocess.Popen(server_command, shell=True)
    client = subprocess.Popen(client_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, shell=True)

    server.wait()
    client.wait()
    
    client_output = client.stdout.read()
    
    return client_output
    
def log_search_results(client_output, dataset, r, efc, ef, w, iterations):
    lines = client_output.strip().splitlines()[-25:]
    
    result_line = lines[5]
    print(result_line)
    
    result_parts = result_line.split('|')
    
    num_queries = result_parts[2].strip()
    recall_at_10 = result_parts[3].strip()
    mrr_at_10 = result_parts[4].strip()
    hops = result_parts[6].strip()
    
    # log into log file
    result_json = {
        "Dataset": dataset,
        "Num Queries": int(float(num_queries)),
        
        # index hyperparameters
        "Degree": r,
        "Ef Construction": efc,
        "PQ Bytes": pq_bytes[dataset],

        # search hyperparameters
        "Queue Size": ef,
        "Iterations": iterations,
        "Beam Width": w,
        
        # search results
        "Recall@10": float(recall_at_10),
        "MRR@10": float(mrr_at_10),
        "Hops": float(hops)
    }
    
    import json
    log_file = f"{project_root}/hyperparameter_search_results.json"
    with open(log_file, 'r') as f:
        current_results = list(json.load(f))
    
    current_results.append(result_json)
    with open(log_file, 'w') as f:
        json.dump(current_results, f, indent=4)
        
        
def run_experiment_with_hyperparameters(r, efc, alpha, ef_range, w_range, iterations_range, dataset):
    build_diskann_index(r, efc, alpha, dataset)
    
    choice = input(f"Proceed with search? (y/n): ")
    if choice.lower() != 'y':
        print("Exiting without running search.")
        exit(0)
    
    ef_range = [efc]
    print(ef_range)
    for ef in ef_range:
        for w in w_range:
            for iterations in iterations_range:
                client_output = run_diskann_search(r, efc, alpha, ef, w, iterations, dataset)
                
                log_search_results(client_output, dataset, r, efc, ef, w, iterations)
    

datasets = ["marco"]

# index hyperparameters
r_range = [128]
efc_ranges = {
    128: [100],         
    256: [200],         
    512: [400]
}
alpha_range = [1.2]

# search hyperparameters
ef_range = []
w_range = [4]
iterations_range = [4, 5]


def driver():
    for dataset in datasets:
        for r in r_range:
            efc_range = efc_ranges[r]
            for efc in efc_range:
                for alpha in alpha_range:
                    run_experiment_with_hyperparameters(r, efc, alpha, ef_range, w_range, iterations_range, dataset)

if __name__ == "__main__":
    driver()
