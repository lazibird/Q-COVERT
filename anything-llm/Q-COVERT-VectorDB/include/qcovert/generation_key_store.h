#ifndef QCOVERT_GENERATION_KEY_STORE_H
#define QCOVERT_GENERATION_KEY_STORE_H

#include "evp.h"

#include <filesystem>
#include <string>

namespace qcovert {

/** Trusted-only persistent storage for one independent AES key per generation. */
class GenerationKeyStore {
public:
    /** Create a key store below the trusted query-service catalog root. */
    explicit GenerationKeyStore(std::filesystem::path catalog_root);

    /** Generate a new key using OpenSSL and persist it with mode 0600. */
    AeadKey create(const std::string& collection, const std::string& generation);

    /** Load an existing key and reject malformed or over-permissive files. */
    AeadKey load(const std::string& collection, const std::string& generation) const;

    /** Return the trusted key path for diagnostics and tests. */
    std::filesystem::path key_path(const std::string& collection,
                                   const std::string& generation) const;

private:
    std::filesystem::path root_;
};

} // namespace qcovert

#endif // QCOVERT_GENERATION_KEY_STORE_H
