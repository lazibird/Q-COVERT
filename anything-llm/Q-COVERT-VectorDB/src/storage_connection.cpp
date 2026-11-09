#include "qcovert/storage_connection.h"

#include "RemoteRing.h"
#include "net_io_channel.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace qcovert {

namespace {

void copy_identifier(char* destination, size_t capacity, const std::string& value, const char* field_name) {
    if (value.empty() || value.size() >= capacity) {
        throw std::invalid_argument(std::string(field_name) + " must contain 1 to " +
                                    std::to_string(capacity - 1) + " characters");
    }
    std::fill(destination, destination + capacity, '\0');
    std::memcpy(destination, value.data(), value.size());
}

} // namespace

StorageConnection::StorageConnection(std::unique_ptr<NetIO> io) : io_(std::move(io)) {}

StorageConnection StorageConnection::connect(
    const std::string& host,
    int port,
    const StorageHandshake& handshake) {
    if (host.empty() || port <= 0 || port > 65535) {
        throw std::invalid_argument("storage host and port must be valid");
    }
    if (handshake.magic != kStorageProtocolMagic ||
        handshake.version != kStorageProtocolVersion) {
        throw std::invalid_argument("storage handshake version is invalid");
    }
    checked_identifier(handshake.collection, sizeof(handshake.collection), "collection");
    checked_identifier(handshake.generation, sizeof(handshake.generation), "generation");

    auto io = std::make_unique<NetIO>(host.c_str(), port, false, true);
    io->send_data(&handshake, sizeof(handshake));
    StorageHandshakeReply reply{};
    io->recv_data(&reply, sizeof(reply));
    const StorageStatus status = static_cast<StorageStatus>(reply.status);
    if (status != StorageStatus::Ok) {
        throw std::runtime_error(std::string("storage handshake failed: ") +
                                 storage_status_message(status));
    }
    return StorageConnection(std::move(io));
}

void StorageConnection::manage_collection(
        const std::string& host,
        int port,
        StorageAction action,
        const std::string& collection,
        const std::string& backup_name) {
    if (host.empty() || port <= 0 || port > 65535) {
        throw std::invalid_argument("storage host and port must be valid");
    }
    if (action != StorageAction::DeleteCollection &&
        action != StorageAction::BackupCollection &&
        action != StorageAction::RestoreCollection &&
        action != StorageAction::DeleteBackup) {
        throw std::invalid_argument("invalid collection management action");
    }
    StorageHandshake handshake{};
    handshake.action = static_cast<uint32_t>(action);
    copy_identifier(
        handshake.collection, sizeof(handshake.collection), collection, "collection");
    copy_identifier(
        handshake.generation, sizeof(handshake.generation), backup_name, "backup name");

    NetIO io(host.c_str(), port, false, true);
    io.send_data(&handshake, sizeof(handshake));
    io.flush();
    StorageHandshakeReply reply{};
    io.recv_data(&reply, sizeof(reply));
    const StorageStatus status = static_cast<StorageStatus>(reply.status);
    if (status != StorageStatus::Ok) {
        throw std::runtime_error(std::string("storage management failed: ") +
                                 storage_status_message(status));
    }
}

StorageConnection::StorageConnection(StorageConnection&& other) noexcept = default;
StorageConnection& StorageConnection::operator=(StorageConnection&& other) noexcept = default;

StorageConnection::~StorageConnection() {
    try {
        close();
    } catch (...) {
        // Destructors cannot report a failed checkpoint. Call close() explicitly
        // when durable completion is required.
    }
}

std::unique_ptr<double_oram::RemoteRing> StorageConnection::make_remote_ring(
    const double_oram::RingOramConfig& config,
    bool integrity,
    const AeadKey& encryption_key) const {
    if (closed_ || !io_) {
        throw std::logic_error("storage connection is closed");
    }
    if (integrity) {
        throw std::invalid_argument("legacy RemoteRing integrity mode is disabled; AES-GCM is mandatory");
    }
    return std::make_unique<double_oram::RemoteRing>(
        io_.get(), config, false, true, integrity, &encryption_key);
}

void StorageConnection::close() {
    if (closed_ || !io_) {
        return;
    }
    // RemoteRing's private RequestType enum currently assigns End = 10. Keep
    // this value here until the core exposes a public protocol constant.
    int end_request = 10;
    io_->send_data(&end_request, sizeof(end_request));
    io_->flush();
    // Never retry End if the acknowledgement is lost; the server has already
    // left the RemoteRing request loop and is checkpointing this session.
    closed_ = true;

    StorageHandshakeReply reply{};
    io_->recv_data(&reply, sizeof(reply));
    const StorageStatus status = static_cast<StorageStatus>(reply.status);
    if (status != StorageStatus::Ok) {
        throw std::runtime_error(std::string("storage checkpoint failed: ") +
                                 storage_status_message(status));
    }
}

} // namespace qcovert
