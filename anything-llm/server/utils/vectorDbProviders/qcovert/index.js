const { TextSplitter } = require("../../TextSplitter");
const { SystemSettings } = require("../../../models/systemSettings");
const { storeVectorResult, cachedVectorInformation } = require("../../files");
const { v4: uuidv4 } = require("uuid");
const { toChunks, getEmbeddingEngineSelection } = require("../../helpers");
const { sourceIdentifier } = require("../../chats");
const { paperContext } = require("../paperContext");
const { VectorDatabase } = require("../base");

const QCOVERT_DIMENSION = 384;
const DEFAULT_BATCH_SIZE = 100;
const DEFAULT_REQUEST_TIMEOUT_MS = 30_000;
const DEFAULT_GENERATION_TIMEOUT_MS = 30 * 60 * 1_000;
const DEFAULT_GENERATION_POLL_MS = 1_000;

class QCovertHttpError extends Error {
  constructor(message, status = 0, code = null) {
    super(message);
    this.name = "QCovertHttpError";
    this.status = status;
    this.code = code;
  }
}

class QCovertClient {
  constructor({
    endpoint,
    apiKey,
    requestTimeoutMs = DEFAULT_REQUEST_TIMEOUT_MS,
  }) {
    if (!endpoint) throw new Error("QCOVERT_ENDPOINT is required.");
    const parsed = new URL(endpoint);
    if (!["http:", "https:"].includes(parsed.protocol)) {
      throw new Error("QCOVERT_ENDPOINT must use http or https.");
    }
    if (!apiKey) throw new Error("QCOVERT_API_KEY is required.");
    this.endpoint = endpoint.replace(/\/+$/, "");
    this.apiKey = apiKey;
    this.requestTimeoutMs = requestTimeoutMs;
  }

  async request(
    path,
    { method = "GET", body = null, allowNotFound = false } = {}
  ) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), this.requestTimeoutMs);
    try {
      const response = await fetch(`${this.endpoint}${path}`, {
        method,
        headers: {
          Authorization: `Bearer ${this.apiKey}`,
          ...(body === null ? {} : { "Content-Type": "application/json" }),
        },
        ...(body === null ? {} : { body: JSON.stringify(body) }),
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
      if (response.status === 404 && allowNotFound) return null;
      if (!response.ok) {
        const code = payload?.error?.code || null;
        const detail = payload?.error?.message || payload?.message || raw;
        throw new QCovertHttpError(
          `Q-COVERT request failed (${response.status})${detail ? `: ${detail}` : ""}`,
          response.status,
          code
        );
      }
      return payload;
    } catch (error) {
      if (error?.name === "AbortError") {
        throw new QCovertHttpError(
          `Q-COVERT request timed out after ${this.requestTimeoutMs}ms.`
        );
      }
      throw error;
    } finally {
      clearTimeout(timeout);
    }
  }

  health() {
    return this.request("/v1/health");
  }

  globalStats() {
    return this.request("/v1/stats");
  }

  getCollection(collection) {
    return this.request(`/v1/collections/${encodeURIComponent(collection)}`, {
      allowNotFound: true,
    });
  }

  createCollection(collection, embeddingModel) {
    return this.request(`/v1/collections/${encodeURIComponent(collection)}`, {
      method: "PUT",
      body: {
        dimension: QCOVERT_DIMENSION,
        metric: "l2",
        embedding_model: embeddingModel,
      },
    });
  }

  collectionStats(collection) {
    return this.request(
      `/v1/collections/${encodeURIComponent(collection)}/stats`,
      { allowNotFound: true }
    );
  }

  upsert(collection, records) {
    return this.request(
      `/v1/collections/${encodeURIComponent(collection)}/vectors`,
      { method: "POST", body: { records } }
    );
  }

  deleteVectorsNoop(collection) {
    return this.request(
      `/v1/collections/${encodeURIComponent(collection)}/vectors`,
      { method: "DELETE" }
    );
  }

  deleteCollection(collection) {
    return this.request(`/v1/collections/${encodeURIComponent(collection)}`, {
      method: "DELETE",
    });
  }

  startGeneration(collection) {
    return this.request(
      `/v1/collections/${encodeURIComponent(collection)}/generations`,
      { method: "POST" }
    );
  }

  search(collection, vector, topK, candidateFactor) {
    return this.request(
      `/v1/collections/${encodeURIComponent(collection)}/search`,
      {
        method: "POST",
        body: {
          vector,
          top_k: topK,
          candidate_factor: candidateFactor,
        },
      }
    );
  }
}

class QCovert extends VectorDatabase {
  get name() {
    return "Q-COVERT";
  }

  normalize(namespace) {
    if (!namespace || typeof namespace !== "string") {
      throw new Error("No namespace value provided.");
    }
    const collection = `${process.env.QCOVERT_COLLECTION_PREFIX || ""}${namespace}`;
    if (
      collection.length > 95 ||
      !/^[A-Za-z0-9_-]+$/.test(collection)
    ) {
      throw new Error(
        "Q-COVERT collection names must contain 1-95 letters, digits, '-' or '_'."
      );
    }
    return collection;
  }

  requestTimeoutMs() {
    const value = Number(process.env.QCOVERT_REQUEST_TIMEOUT_MS);
    return Number.isFinite(value) && value > 0
      ? value
      : DEFAULT_REQUEST_TIMEOUT_MS;
  }

  generationTimeoutMs() {
    const value = Number(process.env.QCOVERT_GENERATION_TIMEOUT_MS);
    return Number.isFinite(value) && value > 0
      ? value
      : DEFAULT_GENERATION_TIMEOUT_MS;
  }

  generationPollMs() {
    const value = Number(process.env.QCOVERT_GENERATION_POLL_MS);
    return Number.isFinite(value) && value > 0
      ? value
      : DEFAULT_GENERATION_POLL_MS;
  }

  candidateFactor() {
    const value = Number(process.env.QCOVERT_CANDIDATE_FACTOR ?? 0.5);
    if (!Number.isFinite(value) || value <= 0 || value > 100) {
      throw new Error("QCOVERT_CANDIDATE_FACTOR must be in (0, 100].");
    }
    return value;
  }

  client() {
    return new QCovertClient({
      endpoint: process.env.QCOVERT_ENDPOINT,
      apiKey: process.env.QCOVERT_API_KEY,
      requestTimeoutMs: this.requestTimeoutMs(),
    });
  }

  async connect() {
    if (process.env.VECTOR_DB !== "qcovert") {
      throw new Error("Q-COVERT::Invalid ENV settings");
    }
    const client = this.client();
    const health = await client.health();
    if (health?.status !== "ok") {
      throw new Error("Q-COVERT returned an invalid health response.");
    }
    return { client };
  }

  async heartbeat() {
    await this.connect();
    return { heartbeat: Number(new Date()) };
  }

  async totalVectors() {
    const { client } = await this.connect();
    return (await client.globalStats())?.vector_count || 0;
  }

  async namespaceCount(namespace = null) {
    const { client } = await this.connect();
    const collection = await client.getCollection(this.normalize(namespace));
    return collection?.vector_count || 0;
  }

  async namespace(client, namespace = null) {
    const collectionName = this.normalize(namespace);
    const collection = await client.getCollection(collectionName);
    if (!collection) return null;
    const stats = await client.collectionStats(collectionName);
    return {
      name: namespace,
      collection: collectionName,
      vectorCount: collection.vector_count || 0,
      generationReady: stats?.generation_ready || false,
      state: collection.state,
      activeGeneration: collection.active_generation || null,
      stats,
    };
  }

  async hasNamespace(namespace = null) {
    if (!namespace) return false;
    const { client } = await this.connect();
    return this.namespaceExists(client, namespace);
  }

  async namespaceExists(client, namespace = null) {
    return !!(await client.getCollection(this.normalize(namespace)));
  }

  async deleteVectorsInNamespace(client, namespace = null) {
    await client.deleteCollection(this.normalize(namespace));
    return true;
  }

  embeddingModelName() {
    if (process.env.EMBEDDING_MODEL_PREF) {
      return process.env.EMBEDDING_MODEL_PREF;
    }
    const engine = process.env.EMBEDDING_ENGINE || "native";
    return engine === "native" ? "Xenova/all-MiniLM-L6-v2" : engine;
  }

  validateVector(values) {
    if (
      !Array.isArray(values) ||
      values.length !== QCOVERT_DIMENSION ||
      values.some((value) => typeof value !== "number" || !Number.isFinite(value))
    ) {
      throw new Error(
        `Q-COVERT requires exactly ${QCOVERT_DIMENSION} finite embedding values.`
      );
    }
    return values;
  }

  async ensureCollection(client, namespace) {
    const collection = this.normalize(namespace);
    const embeddingModel = this.embeddingModelName();
    const existing = await client.getCollection(collection);
    if (!existing) {
      await client.createCollection(collection, embeddingModel);
      return collection;
    }
    if (
      existing.dimension !== QCOVERT_DIMENSION ||
      existing.metric !== "l2" ||
      existing.embedding_model !== embeddingModel
    ) {
      throw new Error(
        `Q-COVERT collection ${collection} was created for ` +
          `${existing.dimension}D/${existing.metric}/${existing.embedding_model}; ` +
          `the current embedder requires ${QCOVERT_DIMENSION}D/l2/${embeddingModel}.`
      );
    }
    return collection;
  }

  async ensureSearchable(client, collection) {
    const deadline = Date.now() + this.generationTimeoutMs();
    let generationRequested = false;
    while (Date.now() < deadline) {
      const manifest = await client.getCollection(collection);
      if (!manifest) {
        throw new Error("Q-COVERT collection disappeared during generation.");
      }
      if (manifest.active_generation) return manifest;
      if (manifest.state === "failed" && !manifest.pending_generation) {
        throw new Error("Q-COVERT generation failed. Check qcovert_query logs.");
      }
      if (!manifest.pending_generation && !generationRequested) {
        try {
          await client.startGeneration(collection);
        } catch (error) {
          if (!(error instanceof QCovertHttpError) || error.status !== 409) {
            throw error;
          }
        }
        generationRequested = true;
      }
      await new Promise((resolve) => setTimeout(resolve, this.generationPollMs()));
    }
    throw new Error(
      `Q-COVERT generation did not become searchable within ${this.generationTimeoutMs()}ms.`
    );
  }

  cachedRecord(chunk, docId) {
    const values = chunk?.values ?? chunk?.vector;
    this.validateVector(values);
    const sourceMetadata = chunk?.metadata ?? chunk?.payload ?? {};
    const { id: _cachedId, ...metadata } = sourceMetadata;
    const id = uuidv4();
    return {
      vector: { id, values, metadata },
      documentVector: { docId, vectorId: id },
    };
  }

  async submitVectors(client, collection, vectors) {
    for (const batch of toChunks(vectors, DEFAULT_BATCH_SIZE)) {
      await client.upsert(
        collection,
        batch.map(({ id, values, metadata }) => ({
          id,
          vector: this.validateVector(values),
          metadata,
        }))
      );
    }
  }

  async addDocumentToNamespace(
    namespace,
    documentData = {},
    fullFilePath = null,
    skipCache = false
  ) {
    const { DocumentVectors } = require("../../../models/vectors");
    try {
      const { pageContent, docId, ...metadata } = documentData;
      if (!pageContent || pageContent.length === 0) return false;

      this.logger("Adding new vectorized document into namespace", namespace);
      const { client } = await this.connect();
      const collection = await this.ensureCollection(client, namespace);
      const documentVectors = [];
      const vectors = [];

      if (!skipCache) {
        const cacheResult = await cachedVectorInformation(fullFilePath);
        if (cacheResult.exists) {
          for (const chunkBatch of cacheResult.chunks) {
            for (const chunk of chunkBatch) {
              const cached = this.cachedRecord(chunk, docId);
              vectors.push(cached.vector);
              documentVectors.push(cached.documentVector);
            }
          }
          await this.submitVectors(client, collection, vectors);
          await this.ensureSearchable(client, collection);
          await DocumentVectors.bulkInsert(documentVectors);
          return { vectorized: true, error: null };
        }
      }

      const EmbedderEngine = getEmbeddingEngineSelection();
      const textSplitter = new TextSplitter({
        chunkSize: TextSplitter.determineMaxChunkSize(
          await SystemSettings.getValueOrFallback({
            label: "text_splitter_chunk_size",
          }),
          EmbedderEngine?.embeddingMaxChunkLength
        ),
        chunkOverlap: await SystemSettings.getValueOrFallback(
          { label: "text_splitter_chunk_overlap" },
          20
        ),
        chunkHeaderMeta: TextSplitter.buildHeaderMeta(metadata),
        chunkPrefix: EmbedderEngine?.embeddingPrefix,
      });
      const textChunks = await textSplitter.splitText(pageContent);
      const vectorValues = await EmbedderEngine.embedChunks(textChunks);
      if (!vectorValues || vectorValues.length !== textChunks.length) {
        throw new Error("Could not embed all document chunks.");
      }

      for (const [index, values] of vectorValues.entries()) {
        const id = uuidv4();
        vectors.push({
          id,
          values: this.validateVector(values),
          metadata: { ...metadata, text: textChunks[index] },
        });
        documentVectors.push({ docId, vectorId: id });
      }
      await this.submitVectors(client, collection, vectors);
      await this.ensureSearchable(client, collection);
      await storeVectorResult(toChunks(vectors, DEFAULT_BATCH_SIZE), fullFilePath);
      await DocumentVectors.bulkInsert(documentVectors);
      return { vectorized: true, error: null };
    } catch (error) {
      this.logger("addDocumentToNamespace", error.message);
      return { vectorized: false, error: error.message };
    }
  }

  async deleteDocumentFromNamespace(namespace, _docId) {
    const { client } = await this.connect();
    if (!(await this.namespaceExists(client, namespace))) return true;
    await client.deleteVectorsNoop(this.normalize(namespace));
    this.logger(
      "Per-document deletion is a Q-COVERT no-op; vectors remain searchable until the collection is rebuilt or deleted."
    );
    return true;
  }

  async similarityResponse({
    client,
    namespace,
    queryVector,
    similarityThreshold = 0.25,
    topN = 4,
    filterIdentifiers = [],
  }) {
    this.validateVector(queryVector);
    const vectorDbQueryStartedAt = Date.now();
    const response = await client.search(
      this.normalize(namespace),
      queryVector,
      topN,
      this.candidateFactor()
    );
    const result = {
      contextTexts: [],
      sourceDocuments: [],
      scores: [],
      timings: {
        vector_db_query_ms: Date.now() - vectorDbQueryStartedAt,
        qcovert_core_search_ms: Number.isFinite(response?.search_ms)
          ? response.search_ms
          : null,
      },
    };
    for (const hit of response?.results || []) {
      const metadata = hit?.metadata || {};
      if (hit.score < similarityThreshold) continue;
      if (filterIdentifiers.includes(sourceIdentifier(metadata))) continue;
      result.contextTexts.push(paperContext(metadata));
      result.sourceDocuments.push({ ...metadata, id: hit.id, score: hit.score });
      result.scores.push(hit.score);
    }
    return result;
  }

  async performSimilaritySearch({
    namespace = null,
    input = "",
    LLMConnector = null,
    similarityThreshold = 0.25,
    topN = 4,
    filterIdentifiers = [],
  }) {
    if (!namespace || !input || !LLMConnector) {
      throw new Error("Invalid request to performSimilaritySearch.");
    }
    const vectorRetrievalStartedAt = Date.now();
    const { client } = await this.connect();
    if (!(await this.namespaceExists(client, namespace))) {
      return {
        contextTexts: [],
        sources: [],
        message: "Invalid query - no documents found for workspace!",
      };
    }
    const queryEmbeddingStartedAt = Date.now();
    const queryVector = await LLMConnector.embedTextInput(input);
    const queryEmbeddingMs = Date.now() - queryEmbeddingStartedAt;
    const { contextTexts, sourceDocuments, timings: searchTimings } =
      await this.similarityResponse({
        client,
        namespace,
        queryVector,
        similarityThreshold,
        topN,
        filterIdentifiers,
      });
    const sources = sourceDocuments.map((metadata, index) => ({
      metadata: { ...metadata, text: contextTexts[index] },
    }));
    return {
      contextTexts,
      sources: this.curateSources(sources),
      message: false,
      timings: {
        query_embedding_ms: queryEmbeddingMs,
        vector_retrieval_ms: Date.now() - vectorRetrievalStartedAt,
        ...searchTimings,
      },
    };
  }

  async "namespace-stats"(reqBody = {}) {
    const { namespace = null } = reqBody;
    if (!namespace) throw new Error("namespace required");
    const { client } = await this.connect();
    const stats = await this.namespace(client, namespace);
    if (!stats) throw new Error("Namespace by that name does not exist.");
    return stats;
  }

  async "delete-namespace"(reqBody = {}) {
    const { namespace = null } = reqBody;
    if (!namespace) throw new Error("namespace required");
    const { client } = await this.connect();
    const details = await this.namespace(client, namespace);
    if (!details) throw new Error("Namespace by that name does not exist.");
    await this.deleteVectorsInNamespace(client, namespace);
    return {
      message: `Namespace ${namespace} was deleted along with ${details.vectorCount} vectors.`,
    };
  }

  async reset() {
    const { Workspace } = require("../../../models/workspace");
    const { client } = await this.connect();
    for (const workspace of await Workspace.where()) {
      if (await this.namespaceExists(client, workspace.slug)) {
        await this.deleteVectorsInNamespace(client, workspace.slug);
      }
    }
    return { reset: true };
  }

  curateSources(sources = []) {
    const documents = [];
    for (const source of sources) {
      const metadata = source?.metadata || source || {};
      if (Object.keys(metadata).length > 0) documents.push({ ...metadata });
    }
    return documents;
  }
}

module.exports = { QCovert, QCovertClient, QCovertHttpError, QCOVERT_DIMENSION };
