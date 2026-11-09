#include "qcovert/storage_protocol.h"

#include <cstring>

namespace qcovert {

std::string checked_identifier(const char* value, size_t capacity, const char* field_name) {
    const size_t length = strnlen(value, capacity);
    if (length == 0 || length == capacity) {
        throw std::invalid_argument(std::string(field_name) + " is not NUL terminated");
    }
    for (size_t i = 0; i < length; ++i) {
        const char ch = value[i];
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!safe) {
            throw std::invalid_argument(std::string(field_name) + " contains an unsafe character");
        }
    }
    return std::string(value, length);
}

const char* storage_status_message(StorageStatus status) {
    switch (status) {
        case StorageStatus::Ok: return "ok";
        case StorageStatus::InvalidHandshake: return "invalid handshake";
        case StorageStatus::InvalidIdentifier: return "invalid collection or generation identifier";
        case StorageStatus::InvalidConfig: return "invalid ORAM tree configuration";
        case StorageStatus::TreeAlreadyExists: return "tree already exists";
        case StorageStatus::TreeNotFound: return "tree not found";
        case StorageStatus::TreeLoadFailed: return "tree state cannot be loaded";
        case StorageStatus::InternalError: return "storage internal error";
        case StorageStatus::TreeBusy: return "tree is already open by another client";
        case StorageStatus::BackupAlreadyExists: return "storage backup already exists";
        case StorageStatus::BackupNotFound: return "storage backup does not exist";
    }
    return "unknown storage error";
}

} // namespace qcovert
