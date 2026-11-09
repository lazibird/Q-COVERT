#ifndef QCOVERT_STORAGE_CONNECTION_H
#define QCOVERT_STORAGE_CONNECTION_H

#include "qcovert/storage_protocol.h"
#include "evp.h"

#include <memory>
#include <string>

class NetIO;

namespace double_oram {
class RemoteRing;
struct RingOramConfig;
}

namespace qcovert {

/** One authenticated logical connection from the trusted query service to a storage tree. */
class StorageConnection {
public:
    /** Establish a new connection and issue a create/open storage handshake. */
    static StorageConnection connect(
        const std::string& host,
        int port,
        const StorageHandshake& handshake);

    /** Execute a collection-level storage management action. */
    static void manage_collection(
        const std::string& host,
        int port,
        StorageAction action,
        const std::string& collection,
        const std::string& backup_name = "management");

    StorageConnection(StorageConnection&&) noexcept;
    StorageConnection& operator=(StorageConnection&&) noexcept;
    StorageConnection(const StorageConnection&) = delete;
    StorageConnection& operator=(const StorageConnection&) = delete;
    ~StorageConnection();

    /** Construct a RemoteRing client that uses this connection's existing protocol stream. */
    std::unique_ptr<double_oram::RemoteRing> make_remote_ring(
        const double_oram::RingOramConfig& config,
        bool integrity,
        const AeadKey& encryption_key) const;

    /** End the session and wait until the storage daemon confirms its checkpoint. */
    void close();

private:
    explicit StorageConnection(std::unique_ptr<NetIO> io);

    std::unique_ptr<NetIO> io_;
    bool closed_ = false;
};

} // namespace qcovert

#endif // QCOVERT_STORAGE_CONNECTION_H
