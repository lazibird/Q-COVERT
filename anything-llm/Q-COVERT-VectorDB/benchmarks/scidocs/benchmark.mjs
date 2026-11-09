import os from "node:os";
import path from "node:path";
import {
  DEFAULT_ANYTHINGLLM_SERVER,
  DEFAULT_QCOVERT_API_KEY,
  DEFAULT_QCOVERT_COLLECTION,
  DEFAULT_QCOVERT_ENDPOINT,
  QCovertClient,
  QCOVERT_CANDIDATE_FACTOR,
  TOP_K,
  ensureDirectory,
  latencySummary,
  loadCache,
  loadVectorMatrix,
  numberArg,
  parseArgs,
  readIds,
  readJson,
  requireFromAnythingLlm,
  seededShuffle,
  vectorAt,
  writeJsonAtomic,
} from "./common.mjs";

const scriptDirectory = path.dirname(new URL(import.meta.url).pathname);
const args = parseArgs(process.argv.slice(2), ["help"]);
if (args.help) {
  console.log(`Usage: node benchmark.mjs [options]

Options:
  --database NAME          lance, qcovert, or both (default: both)
  --cache DIR              Embedding cache (default: ./cache)
  --ground-truth FILE      Faiss exact results (default: CACHE/faiss-ground-truth.json)
  --lance-database DIR     LanceDB directory (default: ./data/lancedb)
  --lance-table NAME       LanceDB table (default: scidocs_benchmark)
  --endpoint URL           Q-COVERT query endpoint
  --collection NAME        Q-COVERT collection
  --anythingllm-server DIR Directory containing @lancedb/lancedb
  --warmup N               Untimed warm-up queries per database (default: 100)
  --rounds N               Measured rounds over every query (default: 5)
  --seed N                 Query-order seed (default: 20260824)
  --request-timeout-ms N   Q-COVERT search timeout (default: 30000)
  --acceptable-p95-ms N    Optional predeclared Q-COVERT p95 acceptance limit
  --output DIR             Results directory (default: ./results)
`);
  process.exit(0);
}

const databaseChoice = args.database ?? "both";
if (!["lance", "qcovert", "both"].includes(databaseChoice)) {
  throw new Error("--database must be lance, qcovert, or both.");
}
const useLance = databaseChoice === "lance" || databaseChoice === "both";
const useQCovert = databaseChoice === "qcovert" || databaseChoice === "both";
const cacheDirectory = path.resolve(args.cache ?? path.join(scriptDirectory, "cache"));
const groundTruthPath = path.resolve(
  args["ground-truth"] ?? path.join(cacheDirectory, "faiss-ground-truth.json")
);
const lanceDirectory = path.resolve(
  args["lance-database"] ?? path.join(scriptDirectory, "data", "lancedb")
);
const lanceTableName = args["lance-table"] ?? "scidocs_benchmark";
const anythingLlmServer = path.resolve(args["anythingllm-server"] ?? DEFAULT_ANYTHINGLLM_SERVER);
const endpoint = args.endpoint ?? process.env.QCOVERT_ENDPOINT ?? DEFAULT_QCOVERT_ENDPOINT;
const collection = args.collection ?? DEFAULT_QCOVERT_COLLECTION;
const warmupCount = numberArg(args.warmup, 100, "warmup", { min: 0 });
const rounds = numberArg(args.rounds, 5, "rounds", { min: 1 });
const seed = numberArg(args.seed, 20_260_824, "seed", { min: 0 });
const requestTimeoutMs = numberArg(args["request-timeout-ms"], 30_000, "request-timeout-ms", {
  min: 1,
});
const acceptableP95 =
  args["acceptable-p95-ms"] === undefined
    ? null
    : numberArg(args["acceptable-p95-ms"], null, "acceptable-p95-ms", {
        integer: false,
        min: 0,
      });
const outputDirectory = path.resolve(args.output ?? path.join(scriptDirectory, "results"));

const { manifest } = await loadCache(cacheDirectory);
const queryIds = await readIds(path.join(cacheDirectory, manifest.files.query_ids));
const queryMatrix = await loadVectorMatrix(
  path.join(cacheDirectory, manifest.files.query_vectors),
  manifest.query_count
);
if (queryIds.length !== manifest.query_count) throw new Error("Query ID count differs from manifest.");
const queryVectors = queryIds.map((_, index) => vectorAt(queryMatrix, index));
const groundTruth = await readJson(groundTruthPath);
if (
  groundTruth.cache_fingerprint !== manifest.fingerprint ||
  groundTruth.faiss_index !== "IndexFlatL2" ||
  groundTruth.metric !== "squared_l2" ||
  groundTruth.top_k !== TOP_K ||
  groundTruth.corpus_count !== manifest.corpus_count ||
  groundTruth.query_count !== manifest.query_count
) {
  throw new Error(
    `Faiss ground truth does not match the embedding cache: ${groundTruthPath}. ` +
      "Run prepare_ground_truth.py again."
  );
}
const groundTruthRankings = queryIds.map((queryId) => {
  const hits = groundTruth.results?.[queryId];
  if (!Array.isArray(hits) || hits.length === 0) {
    throw new Error(`Faiss ground truth has no results for query ${queryId}.`);
  }
  return hits.map((hit) => hit.id).slice(0, TOP_K);
});

const runners = {};
if (useLance) {
  const lancedb = requireFromAnythingLlm(anythingLlmServer, "@lancedb/lancedb");
  const database = await lancedb.connect(lanceDirectory);
  if (!(await database.tableNames()).includes(lanceTableName)) {
    throw new Error(`LanceDB table does not exist: ${lanceTableName}`);
  }
  const table = await database.openTable(lanceTableName);
  if ((await table.countRows()) !== manifest.corpus_count) {
    throw new Error("LanceDB row count differs from the embedding cache.");
  }
  runners.lance = async (vector) => {
    const rows = await table
      .vectorSearch(vector)
      .distanceType("cosine")
      .select(["id"])
      .limit(TOP_K)
      .toArray();
    return rows.map((row) => row.id);
  };
}

if (useQCovert) {
  const client = new QCovertClient({
    endpoint,
    apiKey: process.env.QCOVERT_API_KEY ?? DEFAULT_QCOVERT_API_KEY,
    timeoutMs: requestTimeoutMs,
  });
  const stats = await client.stats(collection);
  const activeCount = stats?.active_generation_stats?.generation_vector_count;
  if (
    !stats?.generation_ready ||
    stats?.collection?.vector_count !== manifest.corpus_count ||
    activeCount !== manifest.corpus_count ||
    stats?.collection?.pending_generation
  ) {
    throw new Error(
      `Q-COVERT collection is not ready for this cache: vectors=${stats?.collection?.vector_count}, ` +
        `active=${activeCount}, pending=${stats?.collection?.pending_generation}.`
    );
  }
  runners.qcovert = async (vector) => {
    const response = await client.search(collection, vector);
    return (response.results ?? []).map((row) => row.id);
  };
}

const systemNames = Object.keys(runners);
const results = Object.fromEntries(
  systemNames.map((name) => [
    name,
    { latencies: [], errors: [], firstRoundRankings: Array(queryIds.length).fill(null) },
  ])
);

for (const name of systemNames) {
  const count = Math.min(warmupCount, queryIds.length);
  console.log(`Warming ${name} with ${count} sequential queries ...`);
  for (let index = 0; index < count; index += 1) await runners[name](queryVectors[index]);
}

for (let round = 0; round < rounds; round += 1) {
  const queryOrder = seededShuffle(queryIds.length, seed + round);
  const databaseOrder = round % 2 === 0 ? systemNames : [...systemNames].reverse();
  for (const name of databaseOrder) {
    console.log(`Measured round ${round + 1}/${rounds}: ${name}, ${queryIds.length} sequential queries`);
    for (const queryIndex of queryOrder) {
      const started = process.hrtime.bigint();
      try {
        const ranking = await runners[name](queryVectors[queryIndex]);
        const elapsedMs = Number(process.hrtime.bigint() - started) / 1_000_000;
        results[name].latencies.push(elapsedMs);
        if (round === 0) results[name].firstRoundRankings[queryIndex] = ranking;
      } catch (error) {
        const elapsedMs = Number(process.hrtime.bigint() - started) / 1_000_000;
        results[name].errors.push({
          round: round + 1,
          query_id: queryIds[queryIndex],
          elapsed_ms: elapsedMs,
          message: error.message,
        });
        if (round === 0) results[name].firstRoundRankings[queryIndex] = [];
      }
    }
  }
}

function effectiveness(rankings) {
  let recallTotal = 0;
  let ndcgTotal = 0;
  let overlapTotal = 0;
  let exactSetMatches = 0;
  let topOneMatches = 0;
  let evaluated = 0;
  for (let index = 0; index < queryIds.length; index += 1) {
    const exactRanking = groundTruthRankings[index];
    const relevant = new Set(exactRanking);
    if (relevant.size === 0) continue;
    const ranking = rankings[index] ?? [];
    let hits = 0;
    let dcg = 0;
    ranking.slice(0, TOP_K).forEach((corpusId, rank) => {
      if (!relevant.has(corpusId)) return;
      hits += 1;
      dcg += 1 / Math.log2(rank + 2);
    });
    const idealHits = Math.min(TOP_K, relevant.size);
    let idealDcg = 0;
    for (let rank = 0; rank < idealHits; rank += 1) idealDcg += 1 / Math.log2(rank + 2);
    recallTotal += hits / relevant.size;
    ndcgTotal += idealDcg > 0 ? dcg / idealDcg : 0;
    overlapTotal += hits;
    if (hits === relevant.size && new Set(ranking.slice(0, TOP_K)).size === relevant.size) {
      exactSetMatches += 1;
    }
    if (ranking[0] === exactRanking[0]) topOneMatches += 1;
    evaluated += 1;
  }
  return {
    evaluated_queries: evaluated,
    recall_at_10: evaluated ? recallTotal / evaluated : null,
    ndcg_at_10: evaluated ? ndcgTotal / evaluated : null,
    mean_exact_top10_overlap: evaluated ? overlapTotal / evaluated : null,
    exact_top10_set_match_rate: evaluated ? exactSetMatches / evaluated : null,
    exact_top1_match_rate: evaluated ? topOneMatches / evaluated : null,
  };
}

const report = {
  benchmark: "SCIDOCS LanceDB vs Q-COVERT database-only sequential retrieval",
  created_at: new Date().toISOString(),
  cache_fingerprint: manifest.fingerprint,
  model: manifest.model,
  dimension: manifest.dimension,
  corpus_count: manifest.corpus_count,
  query_count: manifest.query_count,
  top_k: TOP_K,
  ground_truth: {
    file: groundTruthPath,
    faiss_index: groundTruth.faiss_index,
    metric: groundTruth.metric,
  },
  similarity_filter: false,
  concurrency: 1,
  warmup_queries_per_database: Math.min(warmupCount, queryIds.length),
  measured_rounds: rounds,
  qcovert_candidate_factor: useQCovert ? QCOVERT_CANDIDATE_FACTOR : null,
  latency_scope: {
    excludes: ["embedding", "AnythingLLM", "LLM generation", "reranking"],
    lance: useLance ? "in-process vectorSearch call through result materialization" : null,
    qcovert: useQCovert ? "HTTP request construction through response JSON parsing" : null,
  },
  environment: {
    node: process.version,
    platform: `${os.platform()} ${os.release()} ${os.arch()}`,
    cpu: os.cpus()[0]?.model,
    logical_cpus: os.cpus().length,
    memory_bytes: os.totalmem(),
  },
  systems: {},
};

for (const name of systemNames) {
  report.systems[name] = {
    latency: latencySummary(results[name].latencies, results[name].errors.length),
    effectiveness: effectiveness(results[name].firstRoundRankings),
    errors: results[name].errors,
    first_round_rankings: Object.fromEntries(
      queryIds.map((queryId, index) => [queryId, results[name].firstRoundRankings[index] ?? []])
    ),
  };
}
if (useLance && useQCovert) {
  const lanceLatency = report.systems.lance.latency;
  const qcovertLatency = report.systems.qcovert.latency;
  const lanceQuality = report.systems.lance.effectiveness;
  const qcovertQuality = report.systems.qcovert.effectiveness;
  report.comparison = {
    qcovert_mean_to_lance_ratio:
      qcovertLatency.mean_ms !== null && lanceLatency.mean_ms > 0
        ? qcovertLatency.mean_ms / lanceLatency.mean_ms
        : null,
    qcovert_p95_to_lance_ratio:
      qcovertLatency.p95_ms !== null && lanceLatency.p95_ms > 0
        ? qcovertLatency.p95_ms / lanceLatency.p95_ms
        : null,
    qcovert_p95_is_slower:
      qcovertLatency.p95_ms !== null && lanceLatency.p95_ms !== null
        ? qcovertLatency.p95_ms > lanceLatency.p95_ms
        : null,
    qcovert_recall_to_lance_ratio:
      qcovertQuality.recall_at_10 !== null && lanceQuality.recall_at_10 > 0
        ? qcovertQuality.recall_at_10 / lanceQuality.recall_at_10
        : null,
  };
}

if (useQCovert) {
  const qcovertP95 = report.systems.qcovert.latency.p95_ms;
  const qcovertErrors = report.systems.qcovert.latency.failed_queries;
  report.acceptance = {
    predeclared_p95_limit_ms: acceptableP95,
    requires_zero_errors: true,
    passed:
      acceptableP95 === null || qcovertP95 === null
        ? null
        : qcovertP95 <= acceptableP95 && qcovertErrors === 0,
    note:
      acceptableP95 === null
        ? "No acceptance limit was declared; performance acceptability is not inferred from the measurements."
        : `Q-COVERT is acceptable by this criterion only when p95 <= ${acceptableP95} ms.`,
  };
  if (report.acceptance.passed === true) {
    report.acceptance.conclusion_zh = report.comparison?.qcovert_p95_is_slower
      ? "Q-COVERT 的检索延迟高于 LanceDB，但满足预先声明的 p95 延迟上限。"
      : "Q-COVERT 满足预先声明的 p95 延迟上限。";
  } else if (report.acceptance.passed === false) {
    report.acceptance.conclusion_zh =
      "Q-COVERT 未满足预先声明的 p95 延迟上限或查询发生错误，不能称为延迟可接受。";
  } else {
    report.acceptance.conclusion_zh = "尚未预先声明 p95 上限，脚本不判断延迟是否可接受。";
  }
}

await ensureDirectory(outputDirectory);
const timestamp = new Date().toISOString().replace(/[:.]/gu, "-");
const reportPath = path.join(outputDirectory, `benchmark-${timestamp}.json`);
await writeJsonAtomic(reportPath, report);
await writeJsonAtomic(path.join(outputDirectory, "latest.json"), report);

for (const name of systemNames) {
  const latency = report.systems[name].latency;
  const quality = report.systems[name].effectiveness;
  console.log(
    `${name}: FaissExactRecall@10=${quality.recall_at_10?.toFixed(6)}, ` +
      `nDCG@10=${quality.ndcg_at_10?.toFixed(6)}, mean=${latency.mean_ms?.toFixed(3)} ms, ` +
      `p95=${latency.p95_ms?.toFixed(3)} ms, errors=${latency.failed_queries}`
  );
}
if (report.acceptance) console.log(`acceptance: ${report.acceptance.conclusion_zh}`);
console.log(`Full report: ${reportPath}`);
