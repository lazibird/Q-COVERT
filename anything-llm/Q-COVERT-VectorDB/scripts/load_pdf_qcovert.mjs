#!/usr/bin/env node
/** Resumable bulk initialization of a prepared PDF corpus. Never deletes a collection. */
import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { readFile, stat, writeFile, rename } from "node:fs/promises";
import path from "node:path";
import { QCovertClient } from "../benchmarks/scidocs/common.mjs";

function options(argv) {
  const result = {};
  for (let i = 0; i < argv.length; i++) {
    const key = argv[i];
    if (key === "--validate-only" || key === "--allow-warnings") {
      result[key.slice(2)] = true;
    } else if (key.startsWith("--") && argv[i + 1] && !argv[i + 1].startsWith("--")) {
      result[key.slice(2)] = argv[++i];
    } else throw new Error(`Unexpected argument: ${key}`);
  }
  return result;
}

const args = options(process.argv.slice(2));
const root = path.resolve(args.data || path.join(import.meta.dirname, "../pdf-corpus-v1"));
const endpoint = args.endpoint || process.env.QCOVERT_ENDPOINT || "http://127.0.0.1:8080";
const batchSize = Number(args["batch-size"] || 100);
const waitMinutes = Number(args["generation-minutes"] || 180);
if (!Number.isInteger(batchSize) || batchSize < 1 || batchSize > 500) throw new Error("batch-size must be 1..500");
if (!Number.isFinite(waitMinutes) || waitMinutes < 1) throw new Error("generation-minutes must be positive");

const manifest = JSON.parse(await readFile(path.join(root, "manifest.json"), "utf8"));
if (manifest.format_version !== 1 || !Number.isInteger(manifest.chunk_count) || manifest.chunk_count < 1) {
  throw new Error("Invalid PDF corpus manifest");
}
if (manifest.status !== "complete") {
  if (manifest.status !== "needs_review" || !args["allow-warnings"]) {
    throw new Error("Corpus has review flags; inspect rejected.jsonl and pass --allow-warnings to import");
  }
  const rejected = (await readFile(path.join(root, "rejected.jsonl"), "utf8")).trim().split("\n").map(JSON.parse);
  if (manifest.pdf_count !== 235 || rejected.some(item =>
    item.stage !== "extraction" || !item.reason.includes("replacement characters"))) {
    throw new Error("Review flags include missing PDFs/pages or unexpected failures; refusing import");
  }
  console.log(`Accepting ${rejected.length} replacement-character page warnings; no PDFs were omitted`);
}
if (manifest.embedding?.dimension !== 384 || manifest.embedding?.dtype !== "float32-le" ||
    manifest.embedding?.model !== "MintplexLabs/multilingual-e5-small" ||
    manifest.embedding?.document_prefix !== "passage: " || manifest.embedding?.query_prefix !== "query: ") {
  throw new Error("Embedding configuration is not the expected AnythingLLM 384D e5 setup");
}
if (!['papers', 'papers_summary'].includes(manifest.workspace_slug) ||
    manifest.collection !== `anythingllm_${manifest.workspace_slug}`) {
  throw new Error("Manifest collection does not match the planned papers workspace");
}

async function shaFile(filename) {
  const hash = createHash("sha256");
  for await (const block of createReadStream(filename)) hash.update(block);
  return hash.digest("hex");
}
async function atomicJson(filename, value) {
  const tmp = `${filename}.${process.pid}.tmp`;
  await writeFile(tmp, `${JSON.stringify(value, null, 2)}\n`);
  await rename(tmp, filename);
}
async function saveProgress(state) {
  state.updated_at = new Date().toISOString();
  await atomicJson(path.join(root, "import-progress.json"), state);
  const note = `# Q-COVERT 导入进度\n\n` +
    `更新时间：${state.updated_at}\n\n` +
    `- Collection：\`${state.collection}\`\n` +
    `- 状态：${state.status}\n` +
    `- 已上传：${state.received_count}/${state.expected_count}\n` +
    `- 初始化会话：${state.initialization_id || "尚未创建"}\n` +
    `- Generation：${state.active_generation || state.generation_job_id || "尚未完成"}\n\n` +
    `续跑：\`node scripts/load_pdf_qcovert.mjs --allow-warnings\`\n`;
  const filename = path.join(root, "导入进度.md");
  const tmp = `${filename}.${process.pid}.tmp`;
  await writeFile(tmp, note);
  await rename(tmp, filename);
}

for (const [filename, expected] of Object.entries(manifest.files)) {
  const actual = await shaFile(path.join(root, filename));
  if (actual !== expected) throw new Error(`File hash differs from manifest: ${filename}`);
}
const ids = (await readFile(path.join(root, "vectors.ids"), "utf8")).trimEnd().split("\n");
const chunkLines = (await readFile(path.join(root, "chunks.jsonl"), "utf8")).trimEnd().split("\n");
const matrix = await readFile(path.join(root, "vectors.f32"));
if (ids.length !== manifest.chunk_count || chunkLines.length !== ids.length || matrix.length !== ids.length * 384 * 4) {
  throw new Error("ID/chunk/vector row counts do not match the manifest");
}
const parsedChunks = chunkLines.map((line, index) => {
  const chunk = JSON.parse(line);
  if (chunk.id !== ids[index] || !chunk.metadata?.text || chunk.metadata.text !== chunk.text) {
    throw new Error(`Invalid ID or citation text at row ${index}`);
  }
  return chunk;
});
console.log(`Validated ${ids.length} rows and ${Object.keys(manifest.files).length} file hashes`);
if (args["validate-only"]) process.exit(0);

if (!process.env.QCOVERT_API_KEY) throw new Error("QCOVERT_API_KEY is required");
const client = new QCovertClient({ endpoint, apiKey: process.env.QCOVERT_API_KEY,
  timeoutMs: Number(args["request-timeout-ms"] || 30000) });
if ((await client.health())?.status !== "ok") throw new Error("Q-COVERT query service is not healthy");
const collectionName = manifest.collection;
const progressPath = path.join(root, "import-progress.json");
let state;
try { state = JSON.parse(await readFile(progressPath, "utf8")); }
catch (error) { if (error.code !== "ENOENT") throw error; }
if (state && (state.fingerprint !== manifest.fingerprint || state.collection !== collectionName ||
    state.expected_count !== ids.length)) throw new Error("Import state belongs to another corpus");

let collection = await client.collection(collectionName);
if (!collection) {
  if (state) throw new Error("Collection disappeared but local import state exists; inspect before retrying");
  await client.createCollection(collectionName, manifest.embedding.model);
  collection = await client.collection(collectionName);
}
if (collection.dimension !== 384 || collection.metric !== "l2" ||
    collection.embedding_model !== manifest.embedding.model) {
  throw new Error("Existing collection has incompatible dimension, metric, or embedding_model");
}
if (collection.vector_count > 0 && !state) {
  throw new Error("Existing nonempty collection has no matching local import state; refusing to overwrite it");
}
state ??= { collection: collectionName, endpoint, fingerprint: manifest.fingerprint,
  expected_count: ids.length, received_count: 0, status: "starting",
  started_at: new Date().toISOString() };
await saveProgress(state);

let stats = await client.stats(collectionName);
let ready = stats?.generation_ready && stats?.collection?.vector_count === ids.length &&
  stats?.active_generation_stats?.generation_vector_count === ids.length &&
  !stats?.collection?.pending_generation;
if (!ready) {
  let session = await client.createInitialization(collectionName, {
    expectedCount: ids.length, fingerprint: manifest.fingerprint,
    idempotencyKey: `pdf-corpus-${manifest.fingerprint}`,
  });
  state.initialization_id = session.initialization_id;
  state.status = session.state;
  state.received_count = session.received_count;
  state.next_batch_index = session.next_batch_index;
  await saveProgress(state);
  if (session.state === "uploading") {
    let offset = session.received_count;
    while (offset < ids.length) {
      const end = Math.min(offset + batchSize, ids.length);
      const records = [];
      for (let index = offset; index < end; index++) {
        const vector = Array.from({ length: 384 }, (_, dimension) =>
          matrix.readFloatLE((index * 384 + dimension) * 4));
        const metadata = parsedChunks[index].metadata;
        const pdfRoot = manifest.workspace_slug === 'papers_summary' ? path.dirname(root) : root;
        const sourcePdfPath = path.resolve(pdfRoot, metadata.source_pdf);
        if (!sourcePdfPath.startsWith(path.join(pdfRoot, "source") + path.sep)) {
          throw new Error(`Invalid PDF source path at row ${index}`);
        }
        records.push({ id: ids[index], vector,
          metadata: { ...metadata, source_pdf_path: sourcePdfPath } });
      }
      const reply = await client.initializationBatch(collectionName, session.initialization_id,
        session.next_batch_index, records);
      session = reply.initialization;
      if (session.received_count !== end) throw new Error(`Server count differs after batch ${session.next_batch_index}`);
      offset = session.received_count;
      state.received_count = offset;
      state.next_batch_index = session.next_batch_index;
      state.status = session.state;
      await saveProgress(state);
      if (offset % (batchSize * 10) === 0 || offset === ids.length) {
        console.log(`Uploaded ${offset}/${ids.length} vectors`);
      }
    }
  }
  if (session.state !== "succeeded") {
    console.log(`Finalizing initialization ${session.initialization_id}`);
    session = await client.finalizeInitialization(collectionName, session.initialization_id);
  }
  state.status = session.state;
  state.generation_job_id = session.generation_job_id;
  await saveProgress(state);
  const deadline = Date.now() + waitMinutes * 60000;
  let nextLog = 0;
  while (session.state !== "succeeded" && Date.now() < deadline) {
    if (session.state === "failed") throw new Error(`Generation failed: ${session.error}`);
    await new Promise(resolve => setTimeout(resolve, 5000));
    session = await client.initialization(collectionName, session.initialization_id);
    state.status = session.state;
    state.generation_job_id = session.generation_job_id;
    await saveProgress(state);
    if (Date.now() >= nextLog) {
      console.log(`Generation state=${session.state}, uploaded=${session.received_count}/${ids.length}`);
      nextLog = Date.now() + 30000;
    }
  }
  if (session.state !== "succeeded") throw new Error(`Generation did not finish within ${waitMinutes} minutes`);
  stats = await client.stats(collectionName);
  ready = stats?.generation_ready && stats?.collection?.vector_count === ids.length &&
    stats?.active_generation_stats?.generation_vector_count === ids.length &&
    !stats?.collection?.pending_generation;
}
if (!ready) throw new Error("Collection does not have a complete active generation");
state.status = "complete";
state.received_count = ids.length;
state.active_generation = stats.collection.active_generation;
state.completed_at = new Date().toISOString();
await saveProgress(state);
console.log(`Ready: ${collectionName}, ${ids.length} vectors, generation ${state.active_generation}`);
