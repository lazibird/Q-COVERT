import os

def parse_compass_comm(fname):
    with open(fname, "r") as f:
        val1, val2 = map(int, f.read().split())
        return val1 / ((1024 * 1024 * 100)), int(val2 / 200)

def parse_tfidf_comm(fname):
    with open(fname, "r") as f:
        val1, val2 = map(int, f.read().split())
        return val1 / ((1024 * 1024 * 10)), int(val2 / 20)

def parse_cluster_comm(fname):
    with open(fname, "r") as f:
        val1, val2 = map(int, f.read().split())
        return val1 / ((1024 * 1024 )), int(val2 / 2)

def pretty_print_comm():
    os.chdir(os.path.expanduser('~/compass/'))
    compass_datasets = ["laion", "laion_mal", "sift", "sift_mal", "trip", "trip_mal", "msmarco", "msmarco_mal"]
    print("Communication cost of Compass: ")
    for d in compass_datasets:
        comm, rnd = parse_compass_comm(f"./script/artifact/results/comm_{d}.txt")
        print(f"{d:<12}:  {comm:>8.1f} MB {rnd:>2} RT")

    print("Communication cost of HE-Cluster: ")
    for d in ["laion", "sift", "trip"]:
        comm, rnd = parse_cluster_comm(f"./script/artifact/results/comm_cluster_{d}.txt")
        print(f"{d:<12}:  {comm:>8.1f} MB {rnd:>2} RT")
    
    d = "msmarco"
    print(f"{d:<12}:  - -")

    print("Communication cost of Inv-ORAM with trunc=10000: ")
    d = "laion"
    print(f"{d:<12}:  - -")
    d = "sift"
    print(f"{d:<12}:  - -")
    for d in ["trip", "msmarco"]:
        comm, rnd = parse_tfidf_comm(f"./script/artifact/results/comm_obi_{d}_10000.txt")
        print(f"{d:<12}:  {comm:>8.1f} MB {rnd:>2} RT")

if __name__ == "__main__":
    pretty_print_comm()
    