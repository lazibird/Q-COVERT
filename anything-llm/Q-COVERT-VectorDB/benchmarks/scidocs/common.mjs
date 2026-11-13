import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { createRequire } from "node:module";
import { mkdir, readFile, rename, stat, writeFile } from "node:fs/promises";
import path from "node:path";

export const DIMENSION = 384;
export const TOP_K = 10;
export const QCOVERT_CANDIDATE_FACTOR = 1.0;
export const DEFAULT_DATASET_DIR = "/home/anything-llm/scidocs";
export const DEFAULT_ANYTHINGLLM_SERVER = "/home/anything-llm/server";
export const DEFAULT_MODEL = "MintplexLabs/multilingual-e5-small";
export const DEFAULT_PASSAGE_PREFIX = "passage: ";
export const DEFAULT_QUERY_PREFIX = "query: ";
export const DEFAULT_QCOVERT_ENDPOINT = "http://127.0.0.1:8080";
export const DEFAULT_QCOVERT_COLLECTION = "scidocs_benchmark";
export const DEFAULT_QCOVERT_API_KEY = process.env.QCOVERT_API_KEY || "";

export function parseArgs(argv, booleanNames = []) {
  const booleans = new Set(booleanNames);
  const result = {};
  for (let index = 0; index < argv.length; index += 1) {
    const token = argv[index];
    if (!token.startsWith("--")) throw new Error(`Unexpected argument: ${token}`);
    const name = token.slice(2);
    if (booleans.has(name)) {
      result[name] = true;
      continue;
    }
    if (index + 1 >= argv.length || argv[index + 1].startsWith("--")) {
      throw new Error(`Missing value for --${name}`);
    }
    result[name] = argv[index + 1];
    index += 1;
  }
  return result;
}

export function numberArg(value, fallback, name, { integer = true, min = 0 } = {}) {
  if (value === undefined) return fallback;
  const parsed = Number(value);
  if (!Number.isFinite(parsed) || parsed < min || (integer && !Number.isInteger(parsed))) {
    throw new Error(`--${name} must be ${integer ? "an integer" : "a number"} >= ${min}.`);
  }
  return parsed;
}

export async function ensureDirectory(directory) {
  await mkdir(directory, { recursive: true });
}

export async function pathExists(target) {
  try {
    await stat(target);
    return true;
  } catch (error) {
    if (error?.code === "ENOENT") return false;
    throw error;
  }
}

export async function readJson(file) {
  return JSON.parse(await readFile(file, "utf8"));
}

export async function writeJsonAtomic(file, value) {
  await ensureDirectory(path.dirname(file));
  const temporary = `${file}.${process.pid}.tmp`;
  await writeFile(temporary, `${JSON.stringify(value, null, 2)}\n`);
  await rename(temporary, file);
}

export async function sha256File(file) {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(file)) hash.update(chunk);
  return hash.digest("hex");
}

export function sha256Object(value) {
  return createHash("sha256").update(JSON.stringify(value)).digest("hex");
}

export async function loadCache(cacheDirectory) {
  const manifestPath = path.join(cacheDirectory, "manifest.json");
  if (!(await pathExists(manifestPath))) {
    throw new Error(`Embedding cache is incomplete: ${manifestPath} does not exist.`);
  }
  const manifest = await readJson(manifestPath);
  if (manifest.status !== "complete" || manifest.dimension !== DIMENSION) {
    throw new Error("Embedding cache manifest is incomplete or has the wrong dimension.");
  }
  return { manifest, manifestPath };
}

export async function readIds(file) {
  const raw = await readFile(file, "utf8");
  return raw.split(/\r?\n/u).filter(Boolean);
}

export async function loadVectorMatrix(file, expectedRows) {
  const buffer = await readFile(file);
  const expectedBytes = expectedRows * DIMENSION * Float32Array.BYTES_PER_ELEMENT;
  if (buffer.byteLength !== expectedBytes) {
    throw new Error(`${file} is ${buffer.byteLength} bytes; expected ${expectedBytes}.`);
  }
  if (buffer.byteOffset % Float32Array.BYTES_PER_ELEMENT !== 0) {
    throw new Error(`${file} is not float32-aligned.`);
  }
  return new Float32Array(buffer.buffer, buffer.byteOffset, buffer.byteLength / 4);
}

export function vectorAt(matrix, row) {
  const start = row * DIMENSION;
  return Array.from(matrix.subarray(start, start + DIMENSION));
}

export function requireFromAnythingLlm(serverDirectory, packageName) {
  const require = createRequire(path.join(serverDirectory, "package.json"));
  return require(packageName);
}

export class QCovertClient {
  constructor({ endpoint, apiKey, timeoutMs = 30_000 }) {
    if (!apiKey) throw new Error("QCOVERT_API_KEY is required.");
    this.endpoint = endpoint.replace(/\/+$/u, "");
    this.apiKey = apiKey;
    this.timeoutMs = timeoutMs;
  }

  async request(route, { method = "GET", body, headers = {}, allowNotFound = false } = {}) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), this.timeoutMs);
    const encodedBody =
      body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body);
    try {
      const response = await fetch(`${this.endpoint}${route}`, {
        method,
        headers: {
          Authorization: `Bearer ${this.apiKey}`,
          ...(encodedBody === undefined ? {} : { "Content-Type": "application/json" }),
          ...headers,
        },
        ...(encodedBody === undefined ? {} : { body: encodedBody }),
        signal: controller.signal,
      });
      const raw = await response.text();
      let payload = null;
      if (raw) {
        try {
          payload = JSON.parse(raw);
        } catch {
          payload = { raw };
        }
      }
      if (allowNotFound && response.status === 404) return null;
      if (!response.ok) {
        const error = new Error(
          `Q-COVERT ${method} ${route} failed (${response.status}): ` +
            `${payload?.error?.message ?? payload?.message ?? raw}`
        );
        error.status = response.status;
        error.payload = payload;
        throw error;
      }
      return payload;
    } catch (error) {
      if (error?.name === "AbortError") {
        throw new Error(`Q-COVERT ${method} ${route} timed out after ${this.timeoutMs} ms.`);
      }
      throw error;
    } finally {
      clearTimeout(timeout);
    }
  }

  health() {
    return this.request("/v1/health");
  }

  collection(name) {
    return this.request(`/v1/collections/${encodeURIComponent(name)}`, { allowNotFound: true });
  }

  stats(name) {
    return this.request(`/v1/collections/${encodeURIComponent(name)}/stats`, {
      allowNotFound: true,
    });
  }

  createCollection(name, embeddingModel) {
    return this.request(`/v1/collections/${encodeURIComponent(name)}`, {
      method: "PUT",
      body: { dimension: DIMENSION, metric: "l2", embedding_model: embeddingModel },
    });
  }

  deleteCollection(name) {
    return this.request(`/v1/collections/${encodeURIComponent(name)}`, { method: "DELETE" });
  }

  upsert(name, records) {
    return this.request(`/v1/collections/${encodeURIComponent(name)}/vectors`, {
      method: "POST",
      body: { records },
    });
  }

  createInitialization(name, { expectedCount, fingerprint, idempotencyKey }) {
    return this.request(`/v1/collections/${encodeURIComponent(name)}/initializations`, {
      method: "POST",
      body: {
        expected_count: expectedCount,
        fingerprint,
        idempotency_key: idempotencyKey,
      },
    });
  }

  initialization(name, initializationId) {
    return this.request(
      `/v1/collections/${encodeURIComponent(name)}/initializations/${encodeURIComponent(initializationId)}`
    );
  }

  initializationBatch(name, initializationId, batchIndex, records) {
    const body = JSON.stringify({ records });
    const checksum = createHash("sha256").update(body).digest("hex");
    return this.request(
      `/v1/collections/${encodeURIComponent(name)}/initializations/` +
        `${encodeURIComponent(initializationId)}/batches/${batchIndex}`,
      {
        method: "PUT",
        body,
        headers: { "X-QCovert-Content-SHA256": checksum },
      }
    );
  }

  finalizeInitialization(name, initializationId) {
    return this.request(
      `/v1/collections/${encodeURIComponent(name)}/initializations/` +
        `${encodeURIComponent(initializationId)}/finalize`,
      { method: "POST", body: {} }
    );
  }

  startGeneration(name) {
    return this.request(`/v1/collections/${encodeURIComponent(name)}/generations`, {
      method: "POST",
    });
  }

  generationJob(name, jobId) {
    return this.request(
      `/v1/collections/${encodeURIComponent(name)}/jobs/${encodeURIComponent(jobId)}`
    );
  }

  search(name, vector) {
    return this.request(`/v1/collections/${encodeURIComponent(name)}/search`, {
      method: "POST",
      body: { vector, top_k: TOP_K, candidate_factor: QCOVERT_CANDIDATE_FACTOR },
    });
  }
}

export function percentile(values, quantile) {
  if (values.length === 0) return null;
  const sorted = [...values].sort((left, right) => left - right);
  const index = Math.max(0, Math.ceil(quantile * sorted.length) - 1);
  return sorted[index];
}

export function latencySummary(values, errorCount) {
  const mean = values.length
    ? values.reduce((total, value) => total + value, 0) / values.length
    : null;
  return {
    successful_queries: values.length,
    failed_queries: errorCount,
    mean_ms: mean,
    p50_ms: percentile(values, 0.5),
    p95_ms: percentile(values, 0.95),
    p99_ms: percentile(values, 0.99),
    min_ms: values.length ? Math.min(...values) : null,
    max_ms: values.length ? Math.max(...values) : null,
  };
}

export function seededShuffle(length, seed) {
  const indices = Array.from({ length }, (_, index) => index);
  let state = seed >>> 0;
  const random = () => {
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    return (state >>> 0) / 0x1_0000_0000;
  };
  for (let index = length - 1; index > 0; index -= 1) {
    const target = Math.floor(random() * (index + 1));
    [indices[index], indices[target]] = [indices[target], indices[index]];
  }
  return indices;
}
