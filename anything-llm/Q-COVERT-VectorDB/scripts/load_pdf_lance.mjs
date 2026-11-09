#!/usr/bin/env node
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import readline from "node:readline";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const summaryMode = process.argv.includes("--summary");
const pdfRoot = path.join(root, "pdf-corpus-v1");
const corpus = summaryMode ? path.join(pdfRoot, "summary-v1") : pdfRoot;
const lanceRoot = "/home/hpf/anything-llm-lance";
const requireLance = createRequire(path.join(lanceRoot, "server/package.json"));
const lancedb = requireLance("@lancedb/lancedb");
const tableName = summaryMode ? "papers_summary" : "papers";
const batchSize = 500;
const progressFile = path.join(corpus, "lance-import-progress.json");
const progressMd = path.join(corpus, "LanceDB导入进度.md");
const manifest = JSON.parse(fs.readFileSync(path.join(corpus, "manifest.json"), "utf8"));

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function saveProgress(status, imported) {
  const progress = {
    status,
    collection: tableName,
    target: path.join(lanceRoot, "server/storage/lancedb"),
    fingerprint: manifest.fingerprint,
    imported,
    total: manifest.chunk_count,
    updated_at: new Date().toISOString(),
  };
  const temporary = `${progressFile}.tmp`;
  fs.writeFileSync(temporary, JSON.stringify(progress, null, 2) + "\n");
  fs.renameSync(temporary, progressFile);
  fs.writeFileSync(
    progressMd,
    `# LanceDB 论文导入进度\n\n- 状态：${status}\n- 工作区及表：\`${tableName}\`\n- 已写入：${imported}/${manifest.chunk_count}\n- 更新时间：${progress.updated_at}\n\n续跑：\`node scripts/load_pdf_lance.mjs${summaryMode ? " --summary" : ""}\`\n`
  );
}

async function hashFile(file) {
  const hash = crypto.createHash("sha256");
  for await (const data of fs.createReadStream(path.join(corpus, file))) hash.update(data);
  return hash.digest("hex");
}

function asRow(chunk, vector) {
  const m = chunk.metadata;
  assert(m?.text === chunk.text, `metadata.text mismatch for ${chunk.id}`);
  const sourcePdfPath = path.resolve(pdfRoot, m.source_pdf);
  assert(sourcePdfPath.startsWith(path.join(pdfRoot, "source") + path.sep),
    `Invalid PDF source path for ${chunk.id}`);
  return {
    id: chunk.id,
    vector,
    text: m.text,
    title: m.title || "",
    url: m.url || "",
    docAuthor: m.docAuthor || "",
    published: m.published || "",
    docSource: m.docSource || "",
    paper_id: m.paper_id || chunk.paper_id,
    page_start: Number(m.page_start ?? chunk.page_start),
    page_end: Number(m.page_end ?? chunk.page_end),
    section: m.section || "",
    source_pdf: m.source_pdf || "",
    source_pdf_path: sourcePdfPath,
    is_summary: Boolean(m.is_summary),
  };
}

async function main() {
  assert(manifest.embedding?.dimension === 384, "Expected 384-dimensional embeddings");
  assert(manifest.chunk_count === (summaryMode ? 235 : 42157), "Unexpected corpus chunk count");
  assert(manifest.status === "complete" || manifest.status === "needs_review", "Corpus is not ready");
  for (const file of ["chunks.jsonl", "vectors.ids", "vectors.f32"]) {
    assert((await hashFile(file)) === manifest.files[file], `${file} hash mismatch`);
  }
  const dimensions = manifest.embedding.dimension;
  const bytesPerVector = dimensions * 4;
  assert(fs.statSync(path.join(corpus, "vectors.f32")).size === manifest.chunk_count * bytesPerVector,
    "Vector file size mismatch");

  const db = await lancedb.connect(path.join(lanceRoot, "server/storage/lancedb"));
  const exists = (await db.tableNames()).includes(tableName);
  const previous = fs.existsSync(progressFile)
    ? JSON.parse(fs.readFileSync(progressFile, "utf8"))
    : null;
  assert(!previous || previous.fingerprint === manifest.fingerprint, "Progress belongs to another corpus");
  assert(!exists || !!previous, "Existing papers table has no matching progress file; refusing to append");
  let table = exists ? await db.openTable(tableName) : null;
  const alreadyImported = table ? await table.countRows() : 0;
  assert(alreadyImported <= manifest.chunk_count, "LanceDB table has too many rows");
  saveProgress("running", alreadyImported);

  const chunksStream = fs.createReadStream(path.join(corpus, "chunks.jsonl"));
  const idsStream = fs.createReadStream(path.join(corpus, "vectors.ids"));
  const chunks = readline.createInterface({ input: chunksStream, crlfDelay: Infinity });
  const ids = readline.createInterface({ input: idsStream, crlfDelay: Infinity });
  const idIterator = ids[Symbol.asyncIterator]();
  const vectorFd = fs.openSync(path.join(corpus, "vectors.f32"), "r");
  const buffer = Buffer.alloc(bytesPerVector);
  let index = 0;
  let batch = [];

  async function flush() {
    if (!batch.length) return;
    if (table) await table.add(batch);
    else table = await db.createTable(tableName, batch);
    batch = [];
    const actual = await table.countRows();
    assert(actual === index, `LanceDB row count ${actual} differs from input position ${index}`);
    saveProgress("running", actual);
    console.log(`LanceDB ${actual}/${manifest.chunk_count}`);
  }

  try {
    for await (const line of chunks) {
      const chunk = JSON.parse(line);
      const idLine = await idIterator.next();
      assert(!idLine.done && chunk.id === idLine.value, `ID mismatch at row ${index}`);
      if (index >= alreadyImported) {
        const bytes = fs.readSync(vectorFd, buffer, 0, bytesPerVector, index * bytesPerVector);
        assert(bytes === bytesPerVector, `Short vector at row ${index}`);
        const vector = Array.from({ length: dimensions }, (_, j) => buffer.readFloatLE(j * 4));
        assert(vector.every(Number.isFinite), `Non-finite vector at row ${index}`);
        batch.push(asRow(chunk, vector));
      }
      index++;
      if (batch.length === batchSize) await flush();
    }
    await flush();
    assert(index === manifest.chunk_count, `Expected ${manifest.chunk_count} rows, saw ${index}`);
    assert((await idIterator.next()).done, "Extra vector IDs found");
    assert((await table.countRows()) === manifest.chunk_count, "Final row count mismatch");
    saveProgress("complete", index);
    console.log(`Complete: ${index} vectors in ${tableName}`);
  } finally {
    fs.closeSync(vectorFd);
    chunks.close();
    ids.close();
  }
}

main().catch((error) => {
  console.error(error.message);
  process.exitCode = 1;
});
