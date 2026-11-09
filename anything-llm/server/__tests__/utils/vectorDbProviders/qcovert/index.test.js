const {
  QCovert,
  QCovertClient,
  QCovertHttpError,
  QCOVERT_DIMENSION,
} = require("../../../../utils/vectorDbProviders/qcovert");
const { getVectorDbClass } = require("../../../../utils/helpers");

function response(status, payload) {
  return {
    status,
    ok: status >= 200 && status < 300,
    text: jest.fn().mockResolvedValue(
      payload === null ? "" : JSON.stringify(payload)
    ),
  };
}

describe("QCovertClient", () => {
  let originalFetch;

  beforeEach(() => {
    originalFetch = global.fetch;
    global.fetch = jest.fn();
  });

  afterEach(() => {
    global.fetch = originalFetch;
  });

  it("sends authenticated JSON requests to the configured service", async () => {
    global.fetch.mockResolvedValue(response(200, { status: "ok" }));
    const client = new QCovertClient({
      endpoint: "http://qcovert-query:8080/",
      apiKey: "secret",
    });

    await client.upsert("workspace_1", [
      { id: "vector-1", vector: [1], metadata: { text: "test" } },
    ]);

    expect(global.fetch).toHaveBeenCalledWith(
      "http://qcovert-query:8080/v1/collections/workspace_1/vectors",
      expect.objectContaining({
        method: "POST",
        headers: {
          Authorization: "Bearer secret",
          "Content-Type": "application/json",
        },
        body: JSON.stringify({
          records: [
            { id: "vector-1", vector: [1], metadata: { text: "test" } },
          ],
        }),
      })
    );
  });

  it("maps Q-COVERT errors to a typed HTTP error", async () => {
    global.fetch.mockResolvedValue(
      response(409, {
        error: { code: "generation_busy", message: "already building" },
      })
    );
    const client = new QCovertClient({
      endpoint: "http://127.0.0.1:8080",
      apiKey: "secret",
    });

    await expect(client.startGeneration("workspace_1")).rejects.toMatchObject({
      name: "QCovertHttpError",
      status: 409,
      code: "generation_busy",
    });
  });
});

describe("QCovert provider", () => {
  const managedEnvKeys = [
    "VECTOR_DB",
    "QCOVERT_ENDPOINT",
    "QCOVERT_API_KEY",
    "QCOVERT_GENERATION_POLL_MS",
    "QCOVERT_CANDIDATE_FACTOR",
    "QCOVERT_COLLECTION_PREFIX",
    "EMBEDDING_ENGINE",
    "EMBEDDING_MODEL_PREF",
  ];
  let originalEnvValues;

  beforeEach(() => {
    originalEnvValues = Object.fromEntries(
      managedEnvKeys.map((key) => [key, process.env[key]])
    );
    process.env.VECTOR_DB = "qcovert";
    process.env.QCOVERT_ENDPOINT = "http://127.0.0.1:8080";
    process.env.QCOVERT_API_KEY = "secret";
    process.env.QCOVERT_GENERATION_POLL_MS = "1";
    process.env.QCOVERT_CANDIDATE_FACTOR = "0.5";
  });

  afterEach(() => {
    for (const key of managedEnvKeys) {
      if (originalEnvValues[key] === undefined) delete process.env[key];
      else process.env[key] = originalEnvValues[key];
    }
  });

  it("normalizes workspace slugs and rejects invalid collection names", () => {
    const provider = new QCovert();
    process.env.QCOVERT_COLLECTION_PREFIX = "anythingllm_";
    expect(provider.normalize("workspace-1")).toBe("anythingllm_workspace-1");
    expect(() => provider.normalize("workspace/1")).toThrow(
      "Q-COVERT collection names"
    );
  });

  it("accepts only finite 384-dimensional embeddings", () => {
    const provider = new QCovert();
    const vector = Array(QCOVERT_DIMENSION).fill(0.25);
    expect(provider.validateVector(vector)).toBe(vector);
    expect(() => provider.validateVector(vector.slice(1))).toThrow(
      "exactly 384"
    );
    vector[0] = Number.NaN;
    expect(() => provider.validateVector(vector)).toThrow("finite");
  });

  it("is selected by the AnythingLLM vector database factory", () => {
    expect(getVectorDbClass("qcovert")).toBeInstanceOf(QCovert);
  });

  it("submits large document payloads in bounded batches", async () => {
    const provider = new QCovert();
    const client = { upsert: jest.fn().mockResolvedValue({}) };
    const values = Array(QCOVERT_DIMENSION).fill(0);
    const vectors = Array.from({ length: 201 }, (_, index) => ({
      id: `vector-${index}`,
      values,
      metadata: { text: `chunk-${index}` },
    }));

    await provider.submitVectors(client, "workspace_1", vectors);

    expect(client.upsert).toHaveBeenCalledTimes(3);
    expect(client.upsert.mock.calls.map((call) => call[1].length)).toEqual([
      100, 100, 1,
    ]);
  });

  it("rejects an existing collection from another embedding space", async () => {
    const provider = new QCovert();
    const client = {
      getCollection: jest.fn().mockResolvedValue({
        dimension: QCOVERT_DIMENSION,
        metric: "l2",
        embedding_model: "another-384d-model",
      }),
    };

    await expect(
      provider.ensureCollection(client, "workspace_1")
    ).rejects.toThrow("another-384d-model");
  });

  it("starts the first generation and waits until it is active", async () => {
    const provider = new QCovert();
    const client = {
      getCollection: jest
        .fn()
        .mockResolvedValueOnce({ state: "ready", active_generation: "" })
        .mockResolvedValueOnce({
          state: "active",
          active_generation: "generation_1",
        }),
      startGeneration: jest.fn().mockResolvedValue({ job_id: "job-1" }),
    };

    const manifest = await provider.ensureSearchable(client, "workspace_1");

    expect(client.startGeneration).toHaveBeenCalledWith("workspace_1");
    expect(manifest.active_generation).toBe("generation_1");
  });

  it("maps search hits to AnythingLLM context and applies score filtering", async () => {
    const provider = new QCovert();
    const queryVector = Array(QCOVERT_DIMENSION).fill(0);
    const client = {
      search: jest.fn().mockResolvedValue({
        results: [
          {
            id: "vector-1",
            score: 0.8,
            metadata: { text: "kept", title: "source-a" },
          },
          {
            id: "vector-2",
            score: 0.2,
            metadata: { text: "filtered", title: "source-b" },
          },
        ],
      }),
    };

    const result = await provider.similarityResponse({
      client,
      namespace: "workspace_1",
      queryVector,
      similarityThreshold: 0.5,
      topN: 10,
    });

    expect(client.search).toHaveBeenCalledWith(
      "workspace_1",
      queryVector,
      10,
      0.5
    );
    expect(result.contextTexts).toEqual(["kept"]);
    expect(result.scores).toEqual([0.8]);
    expect(result.sourceDocuments[0]).toMatchObject({
      id: "vector-1",
      score: 0.8,
      title: "source-a",
    });
  });

  it("preserves the explicit no-op contract for per-document deletion", async () => {
    const provider = new QCovert();
    const client = {
      deleteVectorsNoop: jest.fn().mockResolvedValue({ noop: true }),
    };
    provider.connect = jest.fn().mockResolvedValue({ client });
    provider.namespaceExists = jest.fn().mockResolvedValue(true);

    await expect(
      provider.deleteDocumentFromNamespace("workspace_1", "document-1")
    ).resolves.toBe(true);
    expect(client.deleteVectorsNoop).toHaveBeenCalledWith("workspace_1");
  });

  it("recognizes typed generation conflicts", () => {
    const error = new QCovertHttpError("busy", 409, "generation_busy");
    expect(error).toBeInstanceOf(Error);
    expect(error.status).toBe(409);
  });
});
