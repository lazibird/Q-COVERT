import { createReadStream } from "node:fs";
import { appendFile, readFile, rm, stat, truncate } from "node:fs/promises";
import { createRequire } from "node:module";
import path from "node:path";
import { pathToFileURL } from "node:url";
import readline from "node:readline";
import {
  DEFAULT_ANYTHINGLLM_SERVER,
  DEFAULT_DATASET_DIR,
  DEFAULT_MODEL,
  DEFAULT_PASSAGE_PREFIX,
  DEFAULT_QUERY_PREFIX,
  DIMENSION,
  ensureDirectory,
  numberArg,
  parseArgs,
  pathExists,
  readJson,
  sha256File,
  sha256Object,
  writeJsonAtomic,
} from "./common.mjs";

const scriptDirectory = path.dirname(new URL(import.meta.url).pathname);
const args = parseArgs(process.argv.slice(2), ["help", "recreate", "allow-download"]);

if (args.help) {
  console.log(`Usage: node prepare_embeddings.mjs [options]

Options:
  --dataset DIR             SCIDOCS directory (default: ${DEFAULT_DATASET_DIR})
  --output DIR              Cache directory (default: ./cache)
  --anythingllm-server DIR  Directory containing node_modules
  --model NAME              Transformers.js model name
  --batch-size N            Embedding batch size (default: 5)
  --limit N                 Only prepare the first N corpus/query rows (smoke tests)
  --allow-download           Allow Transformers.js to download missing model files
  --recreate                 Remove and rebuild this generated cache
`);
  process.exit(0);
}

const datasetDirectory = path.resolve(args.dataset ?? DEFAULT_DATASET_DIR);
const outputDirectory = path.resolve(args.output ?? path.join(scriptDirectory, "cache"));
const anythingLlmServer = path.resolve(args["anythingllm-server"] ?? DEFAULT_ANYTHINGLLM_SERVER);
const model = args.model ?? DEFAULT_MODEL;
const passagePrefix = args["passage-prefix"] ?? DEFAULT_PASSAGE_PREFIX;
const queryPrefix = args["query-prefix"] ?? DEFAULT_QUERY_PREFIX;
const batchSize = numberArg(args["batch-size"], 5, "batch-size", { min: 1 });
const limit = args.limit === undefined ? null : numberArg(args.limit, null, "limit", { min: 1 });

const corpusPath = path.join(datasetDirectory, "corpus.jsonl");
const queriesPath = path.join(datasetDirectory, "queries.jsonl");
const configPath = path.join(outputDirectory, "cache-config.json");
const manifestPath = path.join(outputDirectory, "manifest.json");

for (const required of [corpusPath, queriesPath]) {
  if (!(await pathExists(required))) throw new Error(`Required SCIDOCS file is missing: ${required}`);
}

if (args.recreate && (await pathExists(outputDirectory))) {
  await rm(outputDirectory, { recursive: true, force: true });
}
await ensureDirectory(outputDirectory);

const config = {
  format_version: 1,
  dataset_directory: datasetDirectory,
  corpus_size_bytes: (await stat(corpusPath)).size,
  queries_size_bytes: (await stat(queriesPath)).size,
  model,
  dimension: DIMENSION,
  passage_prefix: passagePrefix,
  query_prefix: queryPrefix,
  limit,
};

if (await pathExists(configPath)) {
  const existing = await readJson(configPath);
  if (sha256Object(existing) !== sha256Object(config)) {
    throw new Error(
      `Cache configuration differs from ${configPath}. Use a different --output or --recreate.`
    );
  }
} else {
  await writeJsonAtomic(configPath, config);
}

if (await pathExists(manifestPath)) {
  const existing = await readJson(manifestPath);
  if (existing.status === "complete") {
    console.log(`Embedding cache is already complete: ${outputDirectory}`);
    process.exit(0);
  }
}

const require = createRequire(path.join(anythingLlmServer, "package.json"));
const transformersEntry = require.resolve("@xenova/transformers");
const { env, pipeline } = await import(pathToFileURL(transformersEntry).href);
env.allowRemoteModels = Boolean(args["allow-download"]);
env.allowLocalModels = true;
const modelCache = path.join(anythingLlmServer, "storage", "models");

console.log(`Loading embedding model ${model} from ${modelCache} ...`);
const extractor = await pipeline("feature-extraction", model, {
  cache_dir: modelCache,
  local_files_only: !args["allow-download"],
});

async function countIds(idsPath) {
  if (!(await pathExists(idsPath))) return 0;
  const raw = await readFile(idsPath, "utf8");
  return raw.split(/\r?\n/u).filter(Boolean).length;
}

async function repairPartialVectorAppend(idsPath, vectorsPath) {
  const rows = await countIds(idsPath);
  if (!(await pathExists(vectorsPath))) return rows;
  const bytes = (await stat(vectorsPath)).size;
  const rowBytes = DIMENSION * 4;
  if (bytes % rowBytes !== 0) throw new Error(`${vectorsPath} is not aligned to ${rowBytes} bytes.`);
  const vectorRows = bytes / rowBytes;
  if (vectorRows < rows) throw new Error(`${vectorsPath} has fewer rows than ${idsPath}.`);
  if (vectorRows > rows) {
    console.warn(`Recovering interrupted append: truncating ${vectorRows - rows} uncommitted rows.`);
    await truncate(vectorsPath, rows * rowBytes);
  }
  return rows;
}

async function embedFile({ kind, inputPath, idsPath, vectorsPath, prefix, formatText }) {
  const completedRows = await repairPartialVectorAppend(idsPath, vectorsPath);
  console.log(`${kind}: resuming after ${completedRows} completed rows.`);
  const input = createReadStream(inputPath, { encoding: "utf8" });
  const lines = readline.createInterface({ input, crlfDelay: Infinity });
  let sourceIndex = 0;
  let writtenRows = completedRows;
  let batch = [];
  const started = performance.now();

  const flush = async () => {
    if (batch.length === 0) return;
    const texts = batch.map((record) => `${prefix}${formatText(record)}`);
    const tensor = await extractor(texts, {
      pooling: "mean",
      normalize: true,
      padding: true,
      truncation: true,
    });
    const vectors = tensor.tolist();
    if (!Array.isArray(vectors) || vectors.length !== batch.length) {
      throw new Error(`${kind}: model returned an unexpected batch shape.`);
    }
    const flattened = new Float32Array(batch.length * DIMENSION);
    vectors.forEach((vector, index) => {
      if (!Array.isArray(vector) || vector.length !== DIMENSION) {
        throw new Error(`${kind}: expected ${DIMENSION} dimensions, got ${vector?.length}.`);
      }
      flattened.set(vector, index * DIMENSION);
    });
    await appendFile(vectorsPath, Buffer.from(flattened.buffer));
    await appendFile(idsPath, `${batch.map((record) => record._id).join("\n")}\n`);
    writtenRows += batch.length;
    const elapsedSeconds = (performance.now() - started) / 1000;
    const newRows = writtenRows - completedRows;
    console.log(
      `${kind}: ${writtenRows} rows ` +
        `(${(newRows / Math.max(elapsedSeconds, 0.001)).toFixed(2)} new rows/s)`
    );
    batch = [];
  };

  for await (const line of lines) {
    if (!line.trim()) continue;
    if (limit !== null && sourceIndex >= limit) break;
    if (sourceIndex < completedRows) {
      sourceIndex += 1;
      continue;
    }
    const record = JSON.parse(line);
    if (!record._id) throw new Error(`${kind}: row ${sourceIndex + 1} has no _id.`);
    batch.push(record);
    sourceIndex += 1;
    if (batch.length >= batchSize) await flush();
  }
  await flush();
  return countIds(idsPath);
}

const corpusIdsPath = path.join(outputDirectory, "corpus.ids");
const corpusVectorsPath = path.join(outputDirectory, "corpus.f32");
const queryIdsPath = path.join(outputDirectory, "queries.ids");
const queryVectorsPath = path.join(outputDirectory, "queries.f32");

const corpusCount = await embedFile({
  kind: "corpus",
  inputPath: corpusPath,
  idsPath: corpusIdsPath,
  vectorsPath: corpusVectorsPath,
  prefix: passagePrefix,
  formatText: (record) => [record.title, record.text].filter(Boolean).join("\n"),
});
const queryCount = await embedFile({
  kind: "queries",
  inputPath: queriesPath,
  idsPath: queryIdsPath,
  vectorsPath: queryVectorsPath,
  prefix: queryPrefix,
  formatText: (record) => record.text ?? "",
});

const identity = {
  format_version: 1,
  model,
  dimension: DIMENSION,
  passage_prefix: passagePrefix,
  query_prefix: queryPrefix,
  corpus_sha256: await sha256File(corpusPath),
  queries_sha256: await sha256File(queriesPath),
  corpus_count: corpusCount,
  query_count: queryCount,
  limit,
};
const manifest = {
  status: "complete",
  ...identity,
  fingerprint: sha256Object(identity),
  dataset_directory: datasetDirectory,
  created_at: new Date().toISOString(),
  files: {
    corpus_ids: "corpus.ids",
    corpus_vectors: "corpus.f32",
    query_ids: "queries.ids",
    query_vectors: "queries.f32",
  },
};
await writeJsonAtomic(manifestPath, manifest);
console.log(`Embedding cache complete: ${manifestPath}`);
console.log(`fingerprint=${manifest.fingerprint}`);
