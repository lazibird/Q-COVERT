#include "qcovert/generation_key_store.h"

#include "qcovert/collection_catalog.h"

#include <openssl/rand.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace qcovert {

namespace {

void write_all(int fd, const unsigned char* data, size_t size) {
    while (size != 0) {
        const ssize_t count = write(fd, data, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("cannot write generation key");
        data += count;
        size -= static_cast<size_t>(count);
    }
}

void read_all(int fd, unsigned char* data, size_t size) {
    while (size != 0) {
        const ssize_t count = read(fd, data, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("truncated generation key");
        data += count;
        size -= static_cast<size_t>(count);
    }
}

std::string validate_generation(const std::string& generation) {
    if (generation.empty() || generation.size() > 95) {
        throw std::invalid_argument("generation must contain 1 to 95 characters");
    }
    for (const char ch : generation) {
        const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                           (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!valid) throw std::invalid_argument("generation contains an unsafe character");
    }
    return generation;
}

} // namespace

GenerationKeyStore::GenerationKeyStore(fs::path catalog_root)
    : root_(std::move(catalog_root)) {
    if (root_.empty()) throw std::invalid_argument("key-store root must not be empty");
}

fs::path GenerationKeyStore::key_path(const std::string& collection,
                                      const std::string& generation) const {
    return root_ / CollectionCatalog::validate_collection_name(collection) /
           "keys" / (validate_generation(generation) + ".key");
}

AeadKey GenerationKeyStore::create(const std::string& collection,
                                   const std::string& generation) {
    AeadKey key{};
    if (RAND_bytes(key.data(), static_cast<int>(key.size())) != 1) {
        throw std::runtime_error("OpenSSL could not generate a generation key");
    }
    const fs::path path = key_path(collection, generation);
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) throw std::runtime_error("cannot create key directory: " + error.message());
    fs::permissions(path.parent_path(), fs::perms::owner_all,
                    fs::perm_options::replace, error);
    if (error) throw std::runtime_error("cannot secure key directory: " + error.message());
    if (fs::exists(path)) throw std::logic_error("generation key already exists: " + path.string());

    const fs::path temporary = path.string() + ".tmp";
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("cannot create generation key: " + std::string(std::strerror(errno)));
    try {
        write_all(fd, key.data(), key.size());
        if (fsync(fd) != 0) throw std::runtime_error("cannot fsync generation key");
        if (close(fd) != 0) throw std::runtime_error("cannot close generation key");
        if (rename(temporary.c_str(), path.c_str()) != 0) {
            throw std::runtime_error("cannot install generation key: " + std::string(std::strerror(errno)));
        }
        const int directory_fd = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd < 0 || fsync(directory_fd) != 0) {
            if (directory_fd >= 0) close(directory_fd);
            throw std::runtime_error("cannot fsync generation key directory");
        }
        close(directory_fd);
    } catch (...) {
        close(fd);
        unlink(temporary.c_str());
        OPENSSL_cleanse(key.data(), key.size());
        throw;
    }
    return key;
}

AeadKey GenerationKeyStore::load(const std::string& collection,
                                 const std::string& generation) const {
    const fs::path path = key_path(collection, generation);
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) throw std::runtime_error("cannot open generation key");
    AeadKey key{};
    try {
        struct stat info{};
        if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size != static_cast<off_t>(key.size())) {
            throw std::runtime_error("generation key is missing or malformed: " + path.string());
        }
        if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            throw std::runtime_error("generation key permissions must be 0600: " + path.string());
        }
        read_all(fd, key.data(), key.size());
        unsigned char extra = 0;
        if (read(fd, &extra, 1) != 0) throw std::runtime_error("generation key has trailing bytes");
        close(fd);
    } catch (...) {
        close(fd);
        OPENSSL_cleanse(key.data(), key.size());
        throw;
    }
    return key;
}

} // namespace qcovert
