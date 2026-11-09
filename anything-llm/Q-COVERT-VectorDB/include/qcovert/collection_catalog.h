#ifndef QCOVERT_COLLECTION_CATALOG_H
#define QCOVERT_COLLECTION_CATALOG_H

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace qcovert {

/** Immutable schema version for a trusted-side collection manifest. */
constexpr uint32_t kCollectionManifestSchemaVersion = 2;
/** The first Q-COVERT service release supports AnythingLLM's 384-dimension embedder. */
constexpr uint32_t kSupportedEmbeddingDimension = 384;

/** Persistent lifecycle states visible through the query-service API. */
enum class CollectionState {
    /** The collection exists but has no active Q-COVERT generation. */
    Empty,
    /** A generation job is preparing an immutable snapshot. */
    Building,
    /** IVF artifacts exist, but encrypted ORAM trees are not ready for search. */
    Prepared,
    /** A validated generation is active and can serve Q-COVERT searches. */
    Active,
    /** The most recent generation job failed; the prior active generation remains usable. */
    Failed,
};

/** Durable preparation stage for the generation that is not yet active. */
enum class GenerationStage {
    None,
    Building,
    IvfReady,
    OramReady,
    Validated,
};

/** Trusted-side collection metadata persisted in collection.json. */
struct CollectionManifest {
    /** Manifest schema version. */
    uint32_t schema_version = kCollectionManifestSchemaVersion;
    /** Collection identifier used by the HTTP API and storage handshake. */
    std::string name;
    /** Embedding vector dimension. The first release requires 384. */
    uint32_t dimension = kSupportedEmbeddingDimension;
    /** Distance metric. The first release only accepts l2. */
    std::string metric = "l2";
    /** Informational embedding model identifier supplied by the caller. */
    std::string embedding_model;
    /** Total accepted vectors across the active data log and pending append log. */
    uint64_t vector_count = 0;
    /** Current lifecycle state. */
    CollectionState state = CollectionState::Empty;
    /** Identifier of the active immutable generation, empty before the first build. */
    std::string active_generation;
    /** Generation currently being prepared without replacing active_generation. */
    std::string pending_generation;
    /** Last durably completed preparation stage for pending_generation. */
    GenerationStage pending_generation_stage = GenerationStage::None;
    /** Monotonic sequence used to allocate the next generation identifier. */
    uint64_t next_generation = 1;
    /** Creation time in Unix milliseconds. */
    uint64_t created_at_unix_ms = 0;
    /** Last manifest update time in Unix milliseconds. */
    uint64_t updated_at_unix_ms = 0;
};

/** Result returned by create_collection so the HTTP layer can choose 201 or 200. */
struct CreateCollectionResult {
    /** True only when this call created a new manifest. */
    bool created = false;
    /** The persisted collection manifest. */
    CollectionManifest manifest;
};

/**
 * Trusted-side collection catalog.
 *
 * This class owns no plaintext vectors and never connects to qcovert-storage.
 * It validates collection configuration and atomically persists collection.json.
 */
class CollectionCatalog {
public:
    /** Create a catalog rooted at a trusted local filesystem directory. */
    explicit CollectionCatalog(std::filesystem::path root);

    /** Create a collection or return its existing compatible manifest. */
    CreateCollectionResult create_collection(
            const std::string& name,
            uint32_t dimension,
            const std::string& metric,
            const std::string& embedding_model);

    /** Read one collection manifest. Throws std::out_of_range when it does not exist. */
    CollectionManifest get_collection(const std::string& name) const;

    /** Return true when a valid collection manifest exists. */
    bool exists(const std::string& name) const;

    /** List all valid trusted-side collections. */
    std::vector<CollectionManifest> list_collections() const;

    /** Permanently remove one idle collection and all trusted-side state. */
    void delete_collection(const std::string& name);

    /** Create an immutable trusted-side backup of one idle collection. */
    void backup_collection(const std::string& name, const std::string& backup_name);

    /** Replace one idle collection with a previously created trusted-side backup. */
    CollectionManifest restore_collection(
        const std::string& name, const std::string& backup_name);

    /** List stable backup identifiers for one collection. */
    std::vector<std::string> list_backups(const std::string& name) const;

    /** Remove one named trusted-side backup; used to roll back a failed backup. */
    void delete_backup(const std::string& name, const std::string& backup_name);

    /** Persist the current distinct vector count after a successful log append. */
    CollectionManifest set_vector_count(const std::string& name, uint64_t vector_count);

    /** Allocate the next immutable generation and mark its snapshot job as building. */
    CollectionManifest begin_generation(const std::string& name, std::string& generation_id);

    /** Atomically select a fully written generation snapshot as the active generation. */
    CollectionManifest activate_generation(
            const std::string& name,
            const std::string& generation_id,
            uint64_t vector_count);

    /** Validate and activate an OramReady generation in one manifest replacement. */
    CollectionManifest validate_and_activate_generation(
            const std::string& name,
            const std::string& generation_id,
            uint64_t vector_count);

    /** Advance a pending generation monotonically; activation requires Validated. */
    CollectionManifest advance_generation(
            const std::string& name,
            const std::string& generation_id,
            GenerationStage stage);

    /** Mark a generation attempt failed while preserving any prior active generation. */
    CollectionManifest fail_generation(const std::string& name);

    /** Return the trusted collection directory for a validated identifier. */
    std::filesystem::path collection_directory(const std::string& name) const;

    /** Return the catalog root. */
    const std::filesystem::path& root() const;

    /** Validate collection identifiers before they become filesystem path components. */
    static std::string validate_collection_name(const std::string& name);
    /** Validate a backup identifier before it becomes a filesystem component. */
    static std::string validate_backup_name(const std::string& name);
    /** Convert a lifecycle enum to its stable manifest/API representation. */
    static const char* collection_state_name(CollectionState state);
    /** Parse a lifecycle state from a persisted manifest. */
    static CollectionState parse_collection_state(const std::string& value);
    /** Convert a generation stage to its stable manifest/API representation. */
    static const char* generation_stage_name(GenerationStage stage);
    /** Parse a generation stage from a persisted manifest. */
    static GenerationStage parse_generation_stage(const std::string& value);

private:
    std::filesystem::path root_;
    mutable std::mutex mutex_;

    std::filesystem::path manifest_path(const std::string& name) const;
    CollectionManifest load_manifest_locked(const std::string& name) const;
    void write_manifest_locked(const CollectionManifest& manifest) const;
};

} // namespace qcovert

#endif // QCOVERT_COLLECTION_CATALOG_H
