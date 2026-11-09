import importlib.util
import json
import struct
import tempfile
import unittest
import csv
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "benchmark.py"
spec = importlib.util.spec_from_file_location("benchmark", SCRIPT)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class DatasetAdapterTest(unittest.TestCase):
    def test_vector_conversion_and_query_subset(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            vectors = root / "input.bvecs"
            with vectors.open("wb") as stream:
                for value in (3, 4, 5):
                    stream.write(struct.pack("<I", 128))
                    stream.write(bytes([value]) * 128)
            base = root / "base.u8bin"
            self.assertEqual(benchmark.convert_vectors(vectors, base, 128, "uint8", 2), 2)
            self.assertEqual(benchmark.inspect_bin(base, 128, 1), 2)
            with base.open("rb") as stream:
                stream.seek(8)
                self.assertEqual(stream.read(128), bytes([3]) * 128)

            truth = root / "source.ivecs"
            with truth.open("wb") as stream:
                for start in (0, 20):
                    stream.write(struct.pack("<I", 20))
                    stream.write(struct.pack("<20I", *range(start, start + 20)))
            gt = root / "gt10.bin"
            benchmark.convert_truth(truth, gt, 2)
            config = {"--query_file": str(base), "--gt_file": str(gt),
                      "--data_type": "uint8", "dim": 128}
            query_slice, gt_slice = benchmark.query_subset(config, 1, root / "slice")
            self.assertEqual(benchmark.inspect_bin(query_slice, 128, 1), 1)
            self.assertEqual(benchmark.inspect_truth(gt_slice, 1), (20, False))

    def test_truth_with_distances_is_sliced(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            query = root / "query.fbin"
            query.write_bytes(struct.pack("<II", 2, 96) + bytes(2 * 96 * 4))
            truth = root / "gt.bin"
            truth.write_bytes(struct.pack("<II", 2, 10) + struct.pack("<20I", *range(20)) +
                              struct.pack("<20f", *range(20)))
            config = {"--query_file": str(query), "--gt_file": str(truth),
                      "--data_type": "float", "dim": 96}
            _, sliced = benchmark.query_subset(config, 1, root / "slice")
            self.assertEqual(benchmark.inspect_truth(sliced, 1), (10, True))
            self.assertEqual(sliced.stat().st_size, 88)

    def test_prepared_sift_config(self):
        config_file = SCRIPT.parents[1] / "config" / "bench_sift.json"
        if not config_file.is_file():
            self.skipTest("local SIFT dataset was not prepared")
        config = benchmark.resolve_entry_paths(json.loads(config_file.read_text())["sift"])
        self.assertEqual((config["dim"], config["--data_type"], config["-L"]), (128, "float", 125))
        if not Path(config["--data_path"]).is_file():
            self.skipTest("full SIFT data is not installed")
        self.assertEqual(benchmark.inspect_bin(Path(config["--data_path"]), 128, 4), 1_000_000)
        gt_width, _ = benchmark.inspect_truth(Path(config["--gt_file"]), 10_000)
        self.assertGreaterEqual(gt_width, 10)

    def test_all_other_dataset_configs(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name, dim, data_type, expected_l, expected_block in (
                ("sift10m", 128, "uint8", 125, 162),
                ("deep1m", 96, "float", 125, 226),
                ("msmarco1m", 768, "float", 125, 898),
            ):
                item_size = 1 if data_type == "uint8" else 4
                src = root / name
                src.mkdir()
                base = src / "base.bin"
                query = src / "query.bin"
                base.write_bytes(struct.pack("<II", 2, dim) + bytes(2 * dim * item_size))
                query.write_bytes(struct.pack("<II", 1, dim) + bytes(dim * item_size))
                truth = src / "truth.bin"
                truth.write_bytes(struct.pack("<II", 1, 10) + struct.pack("<10I", *range(10)))
                config_path = src / "config.json"
                benchmark.cmd_prepare(SimpleNamespace(dataset=name, source=src, data_root=root / "data",
                                                      base=base, query=query, groundtruth=truth,
                                                      base_count=2, config=config_path, k=10))
                config = json.loads(config_path.read_text())[name]
                self.assertEqual((config["dim"], config["--data_type"], config["-L"], config["block_size"]),
                                 (dim, data_type, expected_l, expected_block))

    def test_network_model_and_dataset_csv(self):
        row = benchmark.result_row(dataset="sift", mode="oram", t=5, w=20, l=100,
                                   queries=100, recall=87.5, measured_ms=10,
                                   measured_network_ms=2, bytes_per_query=1024 * 1024,
                                   direction_changes_per_query=2)
        self.assertEqual(row["local_latency_ms"], 8)
        self.assertAlmostEqual(row["fast_network_ms"], 1 + 8 * 1024 * 1024 * 1000 / 3e9)
        self.assertAlmostEqual(row["slow_network_ms"], 80 + 8 * 1024 * 1024 * 1000 / 400e6)
        self.assertNotIn("meets_90pct", row)
        plain = benchmark.result_row(dataset="sift", mode="plain", t=5, w=20, l=100,
                                     queries=100, recall=87.5, measured_ms=4,
                                     measured_network_ms=0, bytes_per_query=0,
                                     direction_changes_per_query=0)
        self.assertEqual((plain["fast_total_ms"], plain["slow_total_ms"]), (4, 4))
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            path = benchmark.write_dataset_rows(output, "sift", [row])
            updated = dict(row, recall_at_k_pct=88.0)
            benchmark.write_dataset_rows(output, "sift", [updated, plain])
            with path.open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0]["recall_at_k_pct"], "88.0")

    def test_client_log_requires_exact_network_counters(self):
        log = ("Recall@10: 87.5\nTotal Latency: 10 ms\n"
               "---> Mean Network Time: 2 ms\n"
               "BENCH_NET total_bytes=104857600 direction_changes=200 queries=100\n")
        recalls, latencies, network, counters = benchmark.parse_client_results(log, 1, 100)
        self.assertEqual((recalls, latencies, network, counters),
                         ([87.5], [10.0], [2.0], [(104857600, 200, 100)]))
        with self.assertRaises(ValueError):
            benchmark.parse_client_results(log.replace("direction_changes=200 ", ""), 1, 100)

    def test_plain_and_oram_share_one_dataset_csv(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            config_path = root / "config.json"
            source = root / "source"
            source.mkdir()
            base = source / "base.bin"
            query = source / "query.bin"
            truth = source / "gt.bin"
            base.write_bytes(struct.pack("<II", 2, 96) + bytes(2 * 96 * 4))
            query.write_bytes(struct.pack("<II", 2, 96) + bytes(2 * 96 * 4))
            truth.write_bytes(struct.pack("<II", 2, 10) + struct.pack("<20I", *range(20)))
            data_root = root / "data"
            benchmark.cmd_prepare(SimpleNamespace(dataset="deep1m", source=source, data_root=data_root,
                                                  base=base, query=query, groundtruth=truth,
                                                  base_count=2, config=config_path, k=10))
            config = json.loads(config_path.read_text())["deep1m"]
            index = Path(config["--index_path_prefix"] + "_disk.index")
            index.parent.mkdir(parents=True, exist_ok=True)
            index.touch()
            args = SimpleNamespace(dataset="deep1m", data_root=data_root, config=config_path,
                                   queries=2, t="5", w="20", pairing="zip", cache_nodes=0,
                                   threads=4, l=None, server_ip="127.0.0.1", port=8000,
                                   cached_levels=6, integrity=False, k=10)
            with patch.object(benchmark, "run", return_value="Recall@10: 75\nMean Latency: 4000\n"), \
                 patch.object(benchmark, "require_file"):
                benchmark.cmd_plain(args)
            secure_log = ("Recall@10: 74\nTotal Latency: 10 ms\n"
                          "---> Mean Network Time: 2 ms\n"
                          "BENCH_NET total_bytes=2097152 direction_changes=4 queries=2\n")
            with patch.object(benchmark, "run", return_value=secure_log):
                benchmark.cmd_client(args)
            csv_path = data_root / "deep1m" / "results" / "deep1m.csv"
            with csv_path.open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual([row["mode"] for row in rows], ["plain", "oram"])
            self.assertEqual(rows[0]["fast_total_ms"], "4.0")
            self.assertAlmostEqual(float(rows[1]["slow_total_ms"]),
                                   8 + benchmark.network_cost_ms(1048576, 2, 400e6, 80))
            server_bucket = Path(config["buckets_path"])
            server_bucket.parent.mkdir(parents=True, exist_ok=True)
            server_bucket.touch()
            server_args = SimpleNamespace(dataset="deep1m", data_root=data_root,
                                          config=config_path, port=8000,
                                          memory=False, io_events=16)
            with patch.object(benchmark, "run") as mocked_run:
                benchmark.cmd_server(server_args)
            self.assertIn(f"c={config_path}", mocked_run.call_args.args[0])
            self.assertIn(f"dp={server_bucket}", mocked_run.call_args.args[0])


    def test_float_query_to_uint8_is_lossless(self):
        with tempfile.TemporaryDirectory() as folder:
            source, dest = Path(folder) / "query.fvecs", Path(folder) / "query.u8bin"
            source.write_bytes(struct.pack("<I3f", 3, 0, 127, 255))
            benchmark.convert_vectors(source, dest, 3, "uint8")
            self.assertEqual(dest.read_bytes(), struct.pack("<II", 1, 3) + bytes([0, 127, 255]))
            source.write_bytes(struct.pack("<I3f", 3, 0, 0.5, 255))
            with self.assertRaisesRegex(ValueError, "losslessly"):
                benchmark.convert_vectors(source, dest, 3, "uint8")

    def test_shared_raw_profiles_and_low_storage_dispatch(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for dataset, (dim, data_type, _, _) in benchmark.SPECS.items():
                source = root / dataset / "raw"
                source.mkdir(parents=True)
                files = benchmark.raw_profile(dataset)
                size = 1 if data_type == "uint8" else 4
                for role in ("base", "query"):
                    path = source / Path(files[role]).name
                    if path.suffix == ".bin":
                        path.write_bytes(struct.pack("<II", 2, dim) + bytes(2 * dim * size))
                    else:
                        path.write_bytes((struct.pack("<I", dim) + bytes(dim * 4)) * 2)
                truth = source / Path(files["groundtruth"]).name
                if truth.suffix == ".bin":
                    truth.write_bytes(struct.pack("<II", 2, 10) + bytes(80))
                else:
                    truth.write_bytes((struct.pack("<I", 10) + bytes(40)) * 2)
                config_path = root / (dataset + ".json")
                template = json.loads((benchmark.ROOT / "config" / ("bench_low_" + dataset + ".json")).read_text())
                config_path.write_text(json.dumps(template))
                args = SimpleNamespace(dataset=dataset, source=source, data_root=root / "prepared",
                    base=None, query=None, groundtruth=None, base_count=2, config=config_path, k=10)
                benchmark.cmd_prepare(args)
                _, cfg = benchmark.load_config(args)
                self.assertTrue(cfg["low_client_storage"])
                self.assertEqual(cfg["oram_cached_levels"], 0)
                self.assertEqual(cfg["--num_nodes_to_cache"], 0)
                self.assertEqual(benchmark.inspect_bin(Path(cfg["--data_path"]), dim, size), 2)
                self.assertEqual(benchmark.inspect_truth(Path(cfg["--gt_file"]), 2)[0], 10)
                with patch.object(benchmark, "run") as run, patch.object(benchmark, "require_file"):
                    benchmark.cmd_oram_init(args)
                    self.assertEqual(run.call_count, 2)
                    self.assertEqual(Path(run.call_args.args[0][0]).name, "oram_initializer_low")
                args.role, args.port, args.memory, args.io_events = "pq", 8002, True, 16
                with patch.object(benchmark, "run") as run, patch.object(benchmark, "require_file"):
                    benchmark.cmd_server(args)
                    self.assertIn("role=pq", run.call_args.args[0])
                    self.assertIn("dp=" + cfg["pq_oram_buckets_path"], run.call_args.args[0])
                args.t, args.w, args.pairing, args.l = "5", "2", "zip", 125
                args.queries, args.threads, args.cached_levels = 2, 1, 6
                args.integrity, args.cache_nodes, args.server_ip = False, None, "127.0.0.1"
                args.csv = root / (dataset + ".csv")
                log = ("Recall@10: 75\nTotal Latency: 10 ms\nMean Network Time: 2 ms\n"
                       "BENCH_NET total_bytes=100 direction_changes=4 queries=2\n")
                with patch.object(benchmark, "run", return_value=log) as run:
                    benchmark.cmd_client(args)
                    self.assertIn("ocl=0", run.call_args.args[0])
                    self.assertIn("nc=0", run.call_args.args[0])
                with args.csv.open() as stream:
                    rows = list(csv.DictReader(stream))
                self.assertEqual(rows[0]["mode"], "oram_low")
                self.assertEqual(rows[0]["cached_levels"], "0")


if __name__ == "__main__":
    unittest.main()
