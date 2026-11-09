# Q-COVERT provider setup

The AnythingLLM provider connects only to the trusted `qcovert_query` HTTP
service. Never point it at the `qcovert_storage` binary protocol port.

## Requirements

- A running Q-COVERT `qcovert_storage` process.
- A running Q-COVERT `qcovert_query` process with an API key.
- An AnythingLLM embedding provider that produces exactly 384 finite values.
  The native `Xenova/all-MiniLM-L6-v2` embedder is compatible.

## Configuration

Configure the provider from **Settings > Vector Database > Q-COVERT**, or set:

```env
VECTOR_DB=qcovert
QCOVERT_ENDPOINT=http://127.0.0.1:8080
QCOVERT_API_KEY=replace-with-qcovert-api-key
QCOVERT_CANDIDATE_FACTOR=0.5
QCOVERT_COLLECTION_PREFIX=anythingllm_
```

When AnythingLLM runs in Docker and Q-COVERT runs on the host, use
`http://host.docker.internal:8080` as the endpoint. The endpoint and API key
are available in the settings UI. The collection prefix and timeout controls
are environment-only options:

```env
QCOVERT_REQUEST_TIMEOUT_MS=30000
QCOVERT_GENERATION_TIMEOUT_MS=1800000
QCOVERT_GENERATION_POLL_MS=1000
```

Each AnythingLLM workspace slug maps to one Q-COVERT collection. The first
document upload creates the collection, inserts all document chunks, starts the
initial ORAM generation, and waits for it to become active. This first upload
can therefore take substantially longer than later online inserts.

## Current deletion behavior

Q-COVERT does not yet implement individual vector deletion. Removing a
document in AnythingLLM removes the document record from the AnythingLLM UI,
but its existing vectors remain searchable in Q-COVERT. Deleting an entire
workspace namespace deletes its complete Q-COVERT collection, including its
trusted state and encrypted server trees.

