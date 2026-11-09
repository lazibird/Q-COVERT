import path from "node:path";
import {
  DEFAULT_ANYTHINGLLM_SERVER,
  ensureDirectory,
  loadCache,
  loadVectorMatrix,
  numberArg,
  parseArgs,
  pathExists,
  readIds,
  readJson,
  requireFromAnythingLlm,
  vectorAt,
  writeJsonAtomic,
} from "./common.mjs";

const scriptDirectory = path.dirname(new URL(import.meta.url).pathname);
const args = parseArgs(process.argv.slice(2), ["help", "recreate"]);
if (args.help) {
  console.log(`Usage: node load_lancedb.mjs [options]

Options:
  --cache DIR              Embedding cache (default: ./cache)
  --database DIR           LanceDB directory (default: ./data/lancedb)
  --table NAME             Table name (default: scidocs_benchmark)
  --batch-size N           Insert batch size (default: 256)
  --anythingllm-server DIR Directory containing @lancedb/lancedb
  --recreate               Drop and rebuild an existing benchmark table
`);
  process.exit(0);
}

const cacheDirectory = path.resolve(args.cache ?? path.join(scriptDirectory, "cache"));
const databaseDirectory = path.resolve(args.database ?? path.join(scriptDirectory, "data", "lancedb"));
const tableName = args.table ?? "scidocs_benchmark";
const batchSize = numberArg(args["batch-size"], 256, "batch-size", { min: 1 });
const anythingLlmServer = path.resolve(args["anythingllm-server"] ?? DEFAULT_ANYTHINGLLM_SERVER);
const statePath = path.join(databaseDirectory, `${tableName}.benchmark-state.json`);

const { manifest } = await loadCache(cacheDirectory);
const ids = await readIds(path.join(cacheDirectory, manifest.files.corpus_ids));
if (ids.length !== manifest.corpus_count) throw new Error("Corpus ID count differs from manifest.");
const vectors = await loadVectorMatrix(
  path.join(cacheDirectory, manifest.files.corpus_vectors),
  manifest.corpus_count
);

await ensureDirectory(databaseDirectory);
const lancedb = requireFromAnythingLlm(anythingLlmServer, "@lancedb/lancedb");
const database = await lancedb.connect(databaseDirectory);
const existingTables = await database.tableNames();
const exists = existingTables.includes(tableName);

if (exists && args.recreate) await database.dropTable(tableName);
if (exists && !args.recreate) {
  const table = await database.openTable(tableName);
  const rows = await table.countRows();
  const state = (await pathExists(statePath)) ? await readJson(statePath) : null;
  if (rows === ids.length && state?.fingerprint === manifest.fingerprint) {
    console.log(`LanceDB table is already complete: ${tableName} (${rows} rows).`);
    process.exit(0);
  }
  throw new Error(
    `LanceDB table ${tableName} already exists but does not match this cache. ` +
      "Use --recreate or choose another --table/--database."
  );
}

let table = null;
const started = performance.now();
for (let offset = 0; offset < ids.length; offset += batchSize) {
  const end = Math.min(ids.length, offset + batchSize);
  const records = [];
  for (let index = offset; index < end; index += 1) {
    records.push({ id: ids[index], vector: vectorAt(vectors, index) });
  }
  if (table === null) table = await database.createTable(tableName, records);
  else await table.add(records);
  console.log(`LanceDB: inserted ${end}/${ids.length}`);
}

if (table === null) throw new Error("Cannot create an empty LanceDB table.");
const rowCount = await table.countRows();
if (rowCount !== ids.length) throw new Error(`LanceDB has ${rowCount} rows; expected ${ids.length}.`);
const state = {
  database: databaseDirectory,
  table: tableName,
  fingerprint: manifest.fingerprint,
  vector_count: rowCount,
  vector_index_created: false,
  completed_at: new Date().toISOString(),
  load_seconds: (performance.now() - started) / 1000,
};
await writeJsonAtomic(statePath, state);
console.log(`LanceDB load complete in ${state.load_seconds.toFixed(2)} s. No vector index was created.`);
