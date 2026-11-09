#!/usr/bin/env node
// Resumable, row-aligned embedding for scripts/prepare_pdf_corpus.py output.
import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { appendFile, mkdir, readFile, rename, stat, truncate, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import path from "node:path";
import readline from "node:readline";
import { pathToFileURL } from "node:url";

const DEFAULT_OUTPUT = path.resolve(import.meta.dirname, "../pdf-corpus-v1");
const DEFAULT_SERVER = "/home/hpf/anything-llm/server";
const MODELS = {
  "MintplexLabs/multilingual-e5-small": { documentPrefix: "passage: ", queryPrefix: "query: " },
  "Xenova/all-MiniLM-L6-v2": { documentPrefix: "", queryPrefix: "" },
};
const DIMENSION = 384;

function argsOf(values) {
  const result = {};
  for (let i = 0; i < values.length; i += 2) {
    if (!values[i].startsWith("--") || !values[i + 1]) throw new Error(`Bad option: ${values[i]}`);
    result[values[i].slice(2)] = values[i + 1];
  }
  return result;
}

const args = argsOf(process.argv.slice(2));
const output = path.resolve(args.output || DEFAULT_OUTPUT);
const server = path.resolve(args["anythingllm-server"] || DEFAULT_SERVER);
const model = args.model || "MintplexLabs/multilingual-e5-small";
const prefixes = MODELS[model];
if (!prefixes) throw new Error(`Unsupported model: ${model}`);
const batchSize = Number(args["batch-size"] || 5);
if (!Number.isInteger(batchSize) || batchSize < 1 || batchSize > 32) throw new Error("batch-size must be 1..32");
const maxRows = args["max-rows"] ? Number(args["max-rows"]) : Infinity;
if (!(maxRows === Infinity || (Number.isInteger(maxRows) && maxRows > 0))) throw new Error("max-rows must be a positive integer");

const chunksPath = path.join(output, "chunks.jsonl");
const idsPath = path.join(output, "vectors.ids");
const vectorsPath = path.join(output, "vectors.f32");
const progressPath = path.join(output, "embedding-progress.json");
const manifestPath = path.join(output, "manifest.json");

async function shaFile(filename) {
  const hash = createHash("sha256");
  for await (const block of createReadStream(filename)) hash.update(block);
  return hash.digest("hex");
}

async function exists(filename) {
  try { await stat(filename); return true; }
  catch (error) { if (error?.code === "ENOENT") return false; throw error; }
}

async function atomicJson(filename, value) {
  const temporary = `${filename}.${process.pid}.tmp`;
  await writeFile(temporary, `${JSON.stringify(value, null, 2)}\n`);
  await rename(temporary, filename);
}

async function writeStatus(progress) {
  await atomicJson(progressPath, progress);
  const content = `# SP2026 PDF 向量生成进度\n\n` +
    `更新时间：${progress.updated_at}\n\n` +
    `- 模型：\`${progress.model}\`\n` +
    `- 已完成：${progress.completed_rows}/${progress.expected_rows} 片段\n` +
    `- 状态：${progress.status}\n\n` +
    `续跑命令：\`node scripts/embed_pdf_corpus.mjs\`\n`;
  const statusFile = path.join(output, "向量进度.md");
  const temporary = `${statusFile}.${process.pid}.tmp`;
  await writeFile(temporary, content);
  await rename(temporary, statusFile);
}

const textManifest = JSON.parse(await readFile(path.join(output, "text-manifest.json"), "utf8"));
if (!textManifest.chunk_count) throw new Error("No chunks in text-manifest.json");
const actualChunksSha = await shaFile(chunksPath);
if (textManifest.files["chunks.jsonl"] !== actualChunksSha) throw new Error("chunks.jsonl differs from text-manifest.json");
const config = {
  chunks_sha256: actualChunksSha,
  model,
  dimension: DIMENSION,
  document_prefix: prefixes.documentPrefix,
  query_prefix: prefixes.queryPrefix,
  pooling: "mean",
  normalized: true,
};
const configSha = createHash("sha256").update(JSON.stringify(config)).digest("hex");
let progress;
if (await exists(progressPath)) {
  progress = JSON.parse(await readFile(progressPath, "utf8"));
  if (progress.config_sha256 !== configSha || progress.expected_rows !== textManifest.chunk_count) {
    if (args["restart-cache"] !== "yes") {
      throw new Error("Existing embedding cache has different input/model; rerun with --restart-cache yes to archive it and restart");
    }
    const backup = path.join(output, "work", "embedding-backups", new Date().toISOString().replace(/[:.]/g, "-"));
    await mkdir(backup, { recursive: true });
    for (const filename of ["vectors.ids", "vectors.f32", "embedding-progress.json", "manifest.json", "向量进度.md"]) {
      const source = path.join(output, filename);
      if (await exists(source)) await rename(source, path.join(backup, filename));
    }
    console.log(`Archived incompatible embedding cache to ${backup}`);
    progress = null;
  }
}
if (!progress) {
  if (await exists(idsPath) || await exists(vectorsPath)) {
    throw new Error("Vector files exist without progress file; inspect them before continuing");
  }
  progress = { status: "running", config_sha256: configSha, model,
    chunks_sha256: actualChunksSha,
    expected_rows: textManifest.chunk_count, completed_rows: 0,
    started_at: new Date().toISOString(), updated_at: new Date().toISOString() };
  await writeStatus(progress);
}

let ids = [];
if (await exists(idsPath)) {
  const content = await readFile(idsPath, "utf8");
  if (content && !content.endsWith("\n")) {
    const complete = content.slice(0, content.lastIndexOf("\n") + 1);
    await truncate(idsPath, Buffer.byteLength(complete));
    ids = complete.split("\n").filter(Boolean);
  } else ids = content.split("\n").filter(Boolean);
}
const rowBytes = DIMENSION * 4;
const vectorBytes = (await exists(vectorsPath)) ? (await stat(vectorsPath)).size : 0;
if (vectorBytes % rowBytes) throw new Error("vectors.f32 has an incomplete float32 row");
const vectorRows = vectorBytes / rowBytes;
if (ids.length > vectorRows) throw new Error("vectors.ids has more rows than vectors.f32");
if (vectorRows > ids.length) {
  console.log(`Truncating ${vectorRows - ids.length} uncommitted vector rows`);
  await truncate(vectorsPath, ids.length * rowBytes);
}
if (ids.length > textManifest.chunk_count) throw new Error("More committed rows than chunks");
if (progress.completed_rows !== ids.length) {
  // IDs are the commit marker; the progress JSON may lag after an interruption.
  progress.completed_rows = ids.length;
  progress.updated_at = new Date().toISOString();
  await writeStatus(progress);
}

const require = createRequire(path.join(server, "package.json"));
const { env, pipeline } = await import(pathToFileURL(require.resolve("@xenova/transformers")).href);
env.allowRemoteModels = false;
env.allowLocalModels = true;
const cacheDir = path.join(server, "storage", "models");
console.log(`Loading ${model} from ${cacheDir}`);
const extractor = await pipeline("feature-extraction", model, { cache_dir: cacheDir, local_files_only: true });

let seen = 0;
let batch = [];
let batchIds = [];
let lastLog = Date.now();
async function flush() {
  if (!batch.length) return;
  const tensor = await extractor(batch, { pooling: "mean", normalize: true, padding: true, truncation: true });
  const values = tensor.tolist();
  if (values.length !== batch.length) throw new Error("Model returned unexpected batch length");
  const floats = new Float32Array(batch.length * DIMENSION);
  for (let i = 0; i < values.length; i++) {
    if (!Array.isArray(values[i]) || values[i].length !== DIMENSION || values[i].some(x => !Number.isFinite(x))) {
      throw new Error(`Invalid embedding at row ${seen - batch.length + i}`);
    }
    floats.set(values[i], i * DIMENSION);
  }
  // Commit vector bytes before IDs. On restart, any extra vector rows are truncated.
  const buffer = Buffer.allocUnsafe(floats.length * 4);
  floats.forEach((value, index) => buffer.writeFloatLE(value, index * 4));
  await appendFile(vectorsPath, buffer);
  await appendFile(idsPath, `${batchIds.join("\n")}\n`, "utf8");
  progress.completed_rows += batch.length;
  progress.updated_at = new Date().toISOString();
  await writeStatus(progress);
  if (Date.now() - lastLog > 10000 || progress.completed_rows === textManifest.chunk_count) {
    console.log(`Embedded ${progress.completed_rows}/${textManifest.chunk_count} chunks`);
    lastLog = Date.now();
  }
  batch = [];
  batchIds = [];
}

const lines = readline.createInterface({ input: createReadStream(chunksPath, { encoding: "utf8" }), crlfDelay: Infinity });
for await (const line of lines) {
  if (!line) continue;
  const record = JSON.parse(line);
  if (seen < ids.length) {
    if (ids[seen] !== record.id) throw new Error(`ID mismatch on resume at row ${seen}`);
    seen++;
    continue;
  }
  if (!record.embedding_text || !record.metadata?.text) throw new Error(`Missing embedding text at row ${seen}`);
  const input = `${prefixes.documentPrefix}${record.embedding_text}`;
  if (input.length > 1000) throw new Error(`Embedding input exceeds 1000 characters at row ${seen}`);
  batch.push(input);
  batchIds.push(record.id);
  seen++;
  if (batch.length >= batchSize) await flush();
  if (seen >= maxRows) break;
}
await flush();
if (seen < textManifest.chunk_count && seen >= maxRows) {
  progress.status = "paused";
  progress.updated_at = new Date().toISOString();
  await writeStatus(progress);
  console.log(`Paused after ${seen} rows; rerun without --max-rows to continue`);
  process.exit(0);
}
if (seen !== textManifest.chunk_count || progress.completed_rows !== seen) throw new Error("Final row count differs from text manifest");
if ((await stat(vectorsPath)).size !== seen * rowBytes) throw new Error("Final vectors.f32 size mismatch");

const fileNames = ["documents.jsonl", "pages.jsonl", "chunks.jsonl", "vectors.ids", "vectors.f32"];
if (await exists(path.join(output, "rejected.jsonl"))) fileNames.push("rejected.jsonl");
const files = Object.fromEntries(await Promise.all(fileNames.map(async name => [name, await shaFile(path.join(output, name))])));
const manifest = {
  format_version: 1,
  status: textManifest.status === "text_complete" ? "complete" : "needs_review",
  dataset_name: textManifest.dataset_name,
  processing_version: textManifest.processing_version,
  workspace_slug: args.workspace || "papers",
  collection: `anythingllm_${args.workspace || "papers"}`,
  pdf_count: textManifest.pdf_count,
  page_count: textManifest.page_count,
  chunk_count: textManifest.chunk_count,
  rejected_count: textManifest.rejected_count,
  embedding: { engine: "native", model, dimension: DIMENSION, dtype: "float32-le",
    pooling: "mean", normalized: true, document_prefix: prefixes.documentPrefix,
    query_prefix: prefixes.queryPrefix },
  chunking: textManifest.chunking,
  files,
};
manifest.fingerprint = createHash("sha256").update(JSON.stringify(manifest)).digest("hex");
await atomicJson(manifestPath, manifest);
progress.status = "complete";
progress.updated_at = new Date().toISOString();
await writeStatus(progress);
console.log(`Embedding complete: ${seen} vectors; manifest=${manifestPath}`);
