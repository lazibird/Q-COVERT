import path from "node:path";
import {
  DEFAULT_QCOVERT_COLLECTION,
  DEFAULT_QCOVERT_ENDPOINT,
  DEFAULT_QCOVERT_API_KEY,
  QCovertClient,
  loadCache,
  loadVectorMatrix,
  numberArg,
  parseArgs,
  pathExists,
  readIds,
  readJson,
  vectorAt,
  writeJsonAtomic,
} from "./common.mjs";

const scriptDirectory = path.dirname(new URL(import.meta.url).pathname);
const args = parseArgs(process.argv.slice(2), ["help", "recreate"]);
if (args.help) {
  console.log(`Usage: node load_qcovert.mjs [options]

Options:
  --cache DIR            Embedding cache (default: ./cache)
  --endpoint URL         Q-COVERT query endpoint (default: ${DEFAULT_QCOVERT_ENDPOINT})
  --collection NAME      Collection name (default: ${DEFAULT_QCOVERT_COLLECTION})
  --batch-size N         Initialization batch size (default: 100)
  --request-timeout-ms N Per-request timeout (default: 30000)
  --generation-minutes N Maximum generation wait (default: 180)
  --recreate             Delete and rebuild an existing collection
`);
  process.exit(0);
}

const cacheDirectory = path.resolve(args.cache ?? path.join(scriptDirectory, "cache"));
const endpoint = args.endpoint ?? process.env.QCOVERT_ENDPOINT ?? DEFAULT_QCOVERT_ENDPOINT;
const collection = args.collection ?? DEFAULT_QCOVERT_COLLECTION;
const batchSize = numberArg(args["batch-size"], 100, "batch-size", { min: 1 });
const timeoutMs = numberArg(args["request-timeout-ms"], 30_000, "request-timeout-ms", { min: 1 });
const generationMinutes = numberArg(args["generation-minutes"], 180, "generation-minutes", {
  integer: false,
  min: 1,
});
const statePath = path.join(scriptDirectory, "data", `${collection}.qcovert-state.json`);

const { manifest } = await loadCache(cacheDirectory);
const ids = await readIds(path.join(cacheDirectory, manifest.files.corpus_ids));
if (ids.length !== manifest.corpus_count) throw new Error("Corpus ID count differs from manifest.");
const vectors = await loadVectorMatrix(
  path.join(cacheDirectory, manifest.files.corpus_vectors),
  manifest.corpus_count
);
const embeddingModel = `${manifest.model}@${manifest.fingerprint.slice(0, 16)}`;
const client = new QCovertClient({
  endpoint,
  apiKey: process.env.QCOVERT_API_KEY ?? DEFAULT_QCOVERT_API_KEY,
  timeoutMs,
});
const health = await client.health();
if (health?.status !== "ok") throw new Error("Q-COVERT health response is not ok.");

let existing = await client.collection(collection);
if (existing && args.recreate) {
  console.log(`Deleting existing Q-COVERT collection ${collection} ...`);
  await client.deleteCollection(collection);
  existing = null;
}

let state = (await pathExists(statePath)) ? await readJson(statePath) : null;
if (!existing) {
  await client.createCollection(collection, embeddingModel);
  existing = await client.collection(collection);
  state = {
    endpoint,
    collection,
    fingerprint: manifest.fingerprint,
    expected_count: ids.length,
    started_at: new Date().toISOString(),
  };
  await writeJsonAtomic(statePath, state);
} else {
  if (existing.dimension !== 384 || existing.metric !== "l2" || existing.embedding_model !== embeddingModel) {
    throw new Error(
      `Existing collection ${collection} has incompatible dimension/metric/embedding model. ` +
        "Use --recreate or another --collection."
    );
  }
  if (state && (state.fingerprint !== manifest.fingerprint || state.collection !== collection)) {
    throw new Error(
      `Existing collection ${collection} cannot be safely resumed from ${statePath}. ` +
        "Use --recreate or another --collection."
    );
  }
}
state ??= {
  endpoint,
  collection,
  fingerprint: manifest.fingerprint,
  expected_count: ids.length,
  started_at: new Date().toISOString(),
};

const started = performance.now();
async function waitForInitialization(initializationId) {
  const deadline = Date.now() + generationMinutes * 60_000;
  let lastLog = 0;
  while (Date.now() < deadline) {
    const session = await client.initialization(collection, initializationId);
    if (session.state === "succeeded") return session;
    if (session.state === "failed") {
      throw new Error(
        `Initialization ${initializationId} failed in generation ` +
          `${session.generation_job_id}: ${session.error}`
      );
    }
    if (Date.now() - lastLog >= 30_000) {
      console.log(
        `Q-COVERT initialization ${initializationId}: ${session.state}, ` +
          `uploaded=${session.received_count}/${session.expected_count}, ` +
          `generation_job=${session.generation_job_id || "not-started"}`
      );
      lastLog = Date.now();
    }
    await new Promise((resolve) => setTimeout(resolve, 5_000));
  }
  throw new Error(
    `Initialization ${initializationId} did not finish within ${generationMinutes} minutes.`
  );
}

let stats = await client.stats(collection);
let activeCount = stats?.active_generation_stats?.generation_vector_count ?? 0;
let ready =
  stats?.generation_ready &&
  stats?.collection?.vector_count === ids.length &&
  activeCount === ids.length &&
  !stats?.collection?.pending_generation;

let initialization = null;
if (!ready) {
  try {
    initialization = await client.createInitialization(collection, {
      expectedCount: ids.length,
      fingerprint: manifest.fingerprint,
      idempotencyKey: `scidocs-${manifest.fingerprint}`,
    });
  } catch (error) {
    throw new Error(
      `${error.message} Existing non-empty collections created by the old online-upsert loader ` +
        "cannot be converted in place; use --recreate once to start the bulk initialization path."
    );
  }

  if (initialization.state === "uploading") {
    let offset = initialization.received_count;
    let batchIndex = initialization.next_batch_index;
    while (offset < ids.length) {
      const end = Math.min(ids.length, offset + batchSize);
      const records = [];
      for (let index = offset; index < end; index += 1) {
        records.push({ id: ids[index], vector: vectorAt(vectors, index), metadata: {} });
      }
      const response = await client.initializationBatch(
        collection,
        initialization.initialization_id,
        batchIndex,
        records
      );
      initialization = response.initialization;
      offset = initialization.received_count;
      batchIndex = initialization.next_batch_index;
      state.initialization_id = initialization.initialization_id;
      state.next_index = offset;
      state.next_batch_index = batchIndex;
      state.vector_count = offset;
      await writeJsonAtomic(statePath, state);
      console.log(
        `Q-COVERT initialization: submitted ${offset}/${ids.length}; ` +
          `next_batch=${batchIndex}`
      );
    }
  }

  console.log(
    `Finalizing Q-COVERT initialization ${initialization.initialization_id}; ` +
      `one generation will be built from all ${ids.length} vectors ...`
  );
  initialization = await client.finalizeInitialization(
    collection,
    initialization.initialization_id
  );
  if (initialization.state !== "succeeded") {
    initialization = await waitForInitialization(initialization.initialization_id);
  }
  stats = await client.stats(collection);
  activeCount = stats?.active_generation_stats?.generation_vector_count ?? 0;
  ready =
    initialization.state === "succeeded" &&
    stats?.generation_ready &&
    stats?.collection?.vector_count === ids.length &&
    activeCount === ids.length &&
    !stats?.collection?.pending_generation;
}

const finalManifest = stats?.collection;
const finalActiveCount = stats?.active_generation_stats?.generation_vector_count;
if (
  !ready ||
  finalManifest?.vector_count !== ids.length ||
  finalActiveCount !== ids.length ||
  finalManifest?.pending_generation
) {
  throw new Error(
    `Q-COVERT is not ready: vector_count=${finalManifest?.vector_count}, ` +
      `generation_vector_count=${finalActiveCount}, pending=${finalManifest?.pending_generation}.`
  );
}

state.vector_count = ids.length;
state.next_index = ids.length;
state.next_batch_index = initialization?.next_batch_index ?? state.next_batch_index;
state.initialization_id = initialization?.initialization_id ?? state.initialization_id;
state.active_generation = finalManifest.active_generation;
state.final_generation = finalManifest.active_generation;
state.final_generation_complete = true;
state.completed_at = new Date().toISOString();
state.load_and_wait_seconds = (performance.now() - started) / 1000;
await writeJsonAtomic(statePath, state);
console.log(
  `Q-COVERT load complete: ${ids.length} vectors, active generation ` +
    `${finalManifest.active_generation}, ${(state.load_and_wait_seconds / 60).toFixed(2)} min.`
);
