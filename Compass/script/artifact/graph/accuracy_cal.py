import os

import numpy as np 
import pandas as pd

from typing import List, Dict, Tuple
from statistics import mean

def evaluate_mrr(results: List[List[int]], offset_to_qid: List[int], offset_to_pid: List[int], qrels: Dict[int, List[int]]):    
    ranks = []
    for qoffset, poffsets in enumerate(results):
        qid = offset_to_qid[qoffset]
        if qid in qrels.keys():
            rank = next((1.0 / (i + 1) for i, poffset in enumerate(poffsets) if offset_to_pid[poffset] in qrels[qid]), 0)
            ranks.append(rank)
            # print(qrels[qid])
            # print([offset_to_pid[poffset] for poffset in poffsets])
            # exit()
    return mean(ranks)

def ivecs_read(fname):
    a = np.fromfile(fname, dtype='int32')
    d = a[0]
    return a.reshape(-1, d + 1)[:, 1:].copy()

def fvecs_read(fname):
    return ivecs_read(fname).view('float32')

# For sift and laion
def compute_mrr_non_text(gt_path, result_path):
    gt = ivecs_read(gt_path)
    gt = gt[:, 0]

    search_result = ivecs_read(result_path)

    rr = []
    for i in range(gt.shape[0]):
        # print(search_result[0])
        # print(gt[i])
        rank = np.where(search_result[i] == gt[i])[0]

        # print(rank)
        # print(rank[0])

        if rank.shape[0] > 0:
            rr.append(1.0 / (rank[0] + 1))
        else:
            rr.append(0)

    return mean(rr)

def prepare_mrr_text(passage_file, query_file, qrel_file):
    # create poffset to pid  mapping
    # print("Create posffset to pid mapping...")
    passage_df = pd.read_csv(passage_file,sep='\t', header=None)
    passage_df.rename(columns={0: "pid", 1: "passage_text"}, inplace=True)

    offset_to_pid = passage_df.iloc[:, 0].tolist()
    # print(offset_to_pid[0:10])
    # print(len(offset_to_pid))
    # for index, row in passage_df.iterrows():
    #     offset_to_pid.append(row["pid"])

    # create qrel mapping
    # print("Create qrel mapping...")
    qrel_df = pd.read_csv(qrel_file,sep='\t', header=None)
    qrel_df.rename(columns={0: "qid", 1: "none", 2: "pid", 3: "relevance_score"}, inplace=True)

    qrels = {}
    prev_qid = -1

    for index, row in qrel_df.iterrows():
        qid = row["qid"]
        pid = row["pid"]
        score = row["relevance_score"]

        if qid != prev_qid:
            # real_qid += 1
            prev_qid = qid
            qrels[qid] = []

        if score > 0:
            qrels[qid].append(pid)

    # print(len(qrels))
    # print(qrels[38])

    # create qoffset to qid mapping
    # print("Create qoffset to qid mapping...")
    query_df = pd.read_csv(query_file,sep='\t', header=None)
    query_df.rename(columns={0: "qid", 1: "query"}, inplace=True)

    offset_to_qid = []
    for index, row in query_df.iterrows():
        offset_to_qid.append(row["qid"])

    # print(offset_to_qid[0:10])
    # print(len(offset_to_qid))

    return (offset_to_qid, offset_to_pid, qrels) 


def compute_mrr_text(passage_file, query_file, qrel_file, result_path):
    # create poffset to pid  mapping
    # print("Create posffset to pid mapping...")
    passage_df = pd.read_csv(passage_file,sep='\t', header=None)
    passage_df.rename(columns={0: "pid", 1: "passage_text"}, inplace=True)

    offset_to_pid = passage_df.iloc[:, 0].tolist()
    # print(offset_to_pid[0:10])
    # print(len(offset_to_pid))
    # for index, row in passage_df.iterrows():
    #     offset_to_pid.append(row["pid"])

    # create qrel mapping
    # print("Create qrel mapping...")
    qrel_df = pd.read_csv(qrel_file,sep='\t', header=None)
    qrel_df.rename(columns={0: "qid", 1: "none", 2: "pid", 3: "relevance_score"}, inplace=True)

    qrels = {}
    prev_qid = -1

    for index, row in qrel_df.iterrows():
        qid = row["qid"]
        pid = row["pid"]
        score = row["relevance_score"]

        if qid != prev_qid:
            # real_qid += 1
            prev_qid = qid
            qrels[qid] = []

        if score > 0:
            qrels[qid].append(pid)

    # print(len(qrels))
    # print(qrels[38])

    # create qoffset to qid mapping
    # print("Create qoffset to qid mapping...")
    query_df = pd.read_csv(query_file,sep='\t', header=None)
    query_df.rename(columns={0: "qid", 1: "query"}, inplace=True)

    offset_to_qid = []
    for index, row in query_df.iterrows():
        offset_to_qid.append(row["qid"])

    # print(offset_to_qid[0:10])
    # print(len(offset_to_qid))

    # for qid in offset_to_qid:
    #     if qid not in qrels.keys():
    #         print("Query ", qid, " has no relevant files")
    #     elif len(qrels[qid]) == 0:
    #         print("Query ", qid, " has no gt relevant files")

    # loading search result

    # query_result = ivecs_read("/home/clive/see/data/dataset/trip_distilbert/tiptoe_trip.ivecs")
    query_result = ivecs_read(result_path)

    return evaluate_mrr(
            query_result.tolist(),
            offset_to_qid,
            offset_to_pid,
            qrels
        )

def get_mean_perceived_lat(latency_result):
    l = fvecs_read(latency_result)[0]
    nq = int(l.shape[0] / 2)
    per_l = l[0:nq]
    return per_l.mean()

def get_mean_full_lat(latency_result):
    l = fvecs_read(latency_result)[0]
    nq = int(l.shape[0] / 2)
    full_l = l[nq:]
    return full_l.mean()

def get_mean_tfidf_lat(latency_result):
    l = fvecs_read(latency_result)[0]
    return l.mean()

class AccCal():

    def __init__(self):
        # trip
        trip_passage_file = "./data/dataset/trip_distilbert/benchmark_tsv/documents/docs.tsv"
        trip_query_file = "./data/dataset/trip_distilbert/benchmark_tsv/topics/topics.head.val.tsv"
        trip_qrel_file = "./data/dataset/trip_distilbert/benchmark_tsv/qrels/qrels.dctr.head.val.tsv"
        self.trip_meta = prepare_mrr_text(trip_passage_file, trip_query_file, trip_qrel_file)
        # msmarco
        msmarco_passage_file = "./data/dataset/msmarco_bert/passages/collection.tsv"
        msmarco_query_file = "./data/dataset/msmarco_bert/passages/queries.dev.small.tsv"
        msmarco_qrel_file = "./data/dataset/msmarco_bert/passages/qrels.dev.small.tsv"
        self.msmarco_meta = prepare_mrr_text(msmarco_passage_file, msmarco_query_file, msmarco_qrel_file)

    def mrr_msmarco(self, search_result):
        return evaluate_mrr(ivecs_read(search_result).tolist(), self.msmarco_meta[0], self.msmarco_meta[1], self.msmarco_meta[2])

    def mrr_trip(self, search_result):
        return evaluate_mrr(ivecs_read(search_result).tolist(), self.trip_meta[0], self.trip_meta[1], self.trip_meta[2])

    def mrr_sift(self, search_result):
        gt = ivecs_read("./data/dataset/sift/gt.ivecs")
        gt = gt[:, 0]
        search_result= ivecs_read(search_result)
        rr = []
        for i in range(gt.shape[0]):
            rank = np.where(search_result[i] == gt[i])[0]
            if rank.shape[0] > 0:
                rr.append(1.0 / (rank[0] + 1))
            else:
                rr.append(0)
        return mean(rr)

    def mrr_laion(self, search_result):
        gt = ivecs_read("./data/dataset/laion1m/100k/gt.ivecs")
        gt = gt[:, 0]
        search_result= ivecs_read(search_result)
        rr = []
        for i in range(gt.shape[0]):
            rank = np.where(search_result[i] == gt[i])[0]
            if rank.shape[0] > 0:
                rr.append(1.0 / (rank[0] + 1))
            else:
                rr.append(0)
        return mean(rr)
        
    def compute_mrr(self, d, search_result):
        if d == "sift":
            return self.mrr_sift(search_result)
        if d == "laion":
            return self.mrr_laion(search_result)
        if d == "trip":
            return self.mrr_trip(search_result)
        if d == "msmarco":
            return self.mrr_msmarco(search_result)
        assert(0)

if __name__ == "__main__":
    os.chdir(os.path.expanduser('~/compass/'))
    passage_file = "./data/dataset/msmarco_bert/passages/collection.tsv"
    query_file = "./data/dataset/msmarco_bert/passages/queries.dev.small.tsv"
    qrel_file = "./data/dataset/msmarco_bert/passages/qrels.dev.small.tsv"
    mrr = compute_mrr_text(passage_file, query_file, qrel_file, "./script/artifact/results/ablation_accuracy_msmarco_8_24.ivecs")
    print("mrr: ", mrr)