#include "qcovert/storage_protocol.h"
#include "qcovert/shutdown_signal.h"

#include "RemoteRing.h"
#include "net_io_channel.h"

#include <arpa/inet.h>
#include <cerrno>
#include <poll.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

using qcovert::StorageAction;
using qcovert::StorageHandshake;
using qcovert::StorageHandshakeReply;
using qcovert::StorageStatus;
using qcovert::StorageTreeKind;

struct ServerOptions {
    int port = 19091;
    fs::path root = "storage";
    size_t worker_threads = std::max(4U, std::thread::hardware_concurrency());
    size_t max_pending_connections = 0;
};

std::mutex tree_locks_mutex;
std::unordered_map<std::string, std::weak_ptr<std::mutex>> tree_locks;
std::mutex collection_locks_mutex;
std::unordered_map<std::string, std::weak_ptr<std::shared_mutex>> collection_locks;

std::shared_ptr<std::mutex> tree_lock_for(const fs::path& path) {
    const std::string key = fs::absolute(path).lexically_normal().string();
    std::lock_guard<std::mutex> guard(tree_locks_mutex);
    std::shared_ptr<std::mutex> lock = tree_locks[key].lock();
    if (!lock) {
        lock = std::make_shared<std::mutex>();
        tree_locks[key] = lock;
    }
    return lock;
}

std::shared_ptr<std::shared_mutex> collection_lock_for(const fs::path& path) {
    const std::string key = fs::absolute(path).lexically_normal().string();
    std::lock_guard<std::mutex> guard(collection_locks_mutex);
    std::shared_ptr<std::shared_mutex> lock = collection_locks[key].lock();
    if (!lock) {
        lock = std::make_shared<std::shared_mutex>();
        collection_locks[key] = lock;
    }
    return lock;
}

void set_socket_timeouts(int socket_fd) {
    const timeval timeout{30, 0};
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        throw std::runtime_error("cannot configure storage socket timeout");
    }
}

ServerOptions parse_args(int argc, char** argv) {
    ServerOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--port" && i + 1 < argc) {
            options.port = std::stoi(argv[++i]);
        } else if (arg == "--root" && i + 1 < argc) {
            options.root = argv[++i];
        } else if (arg == "--worker-threads" && i + 1 < argc) {
            options.worker_threads = std::stoull(argv[++i]);
        } else if (arg == "--max-pending-connections" && i + 1 < argc) {
            options.max_pending_connections = std::stoull(argv[++i]);
        } else if (arg == "--help") {
            std::cout << "Usage: qcovert_storage [--port PORT] [--root STORAGE_ROOT]"
                      << " [--worker-threads N] [--max-pending-connections N]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    if (options.port <= 0 || options.port > 65535 || options.worker_threads == 0) {
        throw std::invalid_argument("invalid storage-service options");
    }
    if (options.max_pending_connections == 0) {
        options.max_pending_connections = options.worker_threads * 16;
    }
    return options;
}

void send_reply(NetIO& io, StorageStatus status) {
    StorageHandshakeReply reply{};
    reply.status = static_cast<uint32_t>(status);
    io.send_data(&reply, sizeof(reply));
    io.flush();
}

StorageStatus validate_handshake(const StorageHandshake& request) {
    if (request.magic != qcovert::kStorageProtocolMagic ||
        request.version != qcovert::kStorageProtocolVersion) {
        return StorageStatus::InvalidHandshake;
    }
    const StorageAction action = static_cast<StorageAction>(request.action);
    const bool tree_action = action == StorageAction::Create || action == StorageAction::Open;
    const bool management_action = action == StorageAction::DeleteCollection ||
        action == StorageAction::BackupCollection ||
        action == StorageAction::RestoreCollection ||
        action == StorageAction::DeleteBackup;
    if ((!tree_action && !management_action) ||
        (tree_action &&
         request.tree_kind != static_cast<uint32_t>(StorageTreeKind::Cluster) &&
         request.tree_kind != static_cast<uint32_t>(StorageTreeKind::Ring))) {
        return StorageStatus::InvalidHandshake;
    }
    try {
        qcovert::checked_identifier(request.collection, sizeof(request.collection), "collection");
        qcovert::checked_identifier(request.generation, sizeof(request.generation), "generation");
    } catch (const std::exception&) {
        return StorageStatus::InvalidIdentifier;
    }
    if (tree_action &&
        (request.integrity != 0 || request.block_size <= 0 || request.real_bucket_size <= 0 ||
        request.dummy_size < 0 || request.evict_rate <= 0 ||
        request.num_levels < 2 || request.cached_levels < 0 ||
        request.cached_levels >= request.num_levels)) {
        return StorageStatus::InvalidConfig;
    }
    return StorageStatus::Ok;
}

fs::path collection_path(const fs::path& root, const StorageHandshake& request) {
    return root / qcovert::checked_identifier(
        request.collection, sizeof(request.collection), "collection");
}

fs::path backup_path(const fs::path& root, const StorageHandshake& request) {
    const std::string collection = qcovert::checked_identifier(
        request.collection, sizeof(request.collection), "collection");
    const std::string backup = qcovert::checked_identifier(
        request.generation, sizeof(request.generation), "backup name");
    return root / ".backups" / collection / backup;
}

fs::path temporary_sibling(const fs::path& path, const char* suffix) {
    return path.string() + suffix + "." +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

void copy_directory(const fs::path& source, const fs::path& destination) {
    std::error_code error;
    fs::remove_all(destination, error);
    error.clear();
    if (fs::exists(source)) {
        fs::copy(source, destination, fs::copy_options::recursive, error);
    } else {
        fs::create_directories(destination, error);
    }
    if (error) {
        fs::remove_all(destination, error);
        throw std::runtime_error("cannot copy storage collection: " + error.message());
    }
}

StorageStatus manage_collection(
        const fs::path& root,
        const StorageHandshake& request) {
    const StorageAction action = static_cast<StorageAction>(request.action);
    const fs::path collection = collection_path(root, request);
    const fs::path backup = backup_path(root, request);
    std::error_code error;
    if (action == StorageAction::DeleteCollection) {
        fs::remove_all(collection, error);
        if (error) return StorageStatus::InternalError;
        fs::remove_all(backup.parent_path(), error);
        return error ? StorageStatus::InternalError : StorageStatus::Ok;
    }
    if (action == StorageAction::BackupCollection) {
        if (fs::exists(backup)) return StorageStatus::BackupAlreadyExists;
        fs::create_directories(backup.parent_path(), error);
        if (error) return StorageStatus::InternalError;
        const fs::path staging = temporary_sibling(backup, ".building");
        try {
            copy_directory(collection, staging);
            fs::rename(staging, backup, error);
            if (error) {
                const std::string message = error.message();
                fs::remove_all(staging, error);
                std::cerr << "[qcovert-storage] backup publish error: " << message << "\n";
                return StorageStatus::InternalError;
            }
            return StorageStatus::Ok;
        } catch (...) {
            fs::remove_all(staging, error);
            return StorageStatus::InternalError;
        }
    }
    if (action == StorageAction::DeleteBackup) {
        fs::remove_all(backup, error);
        return error ? StorageStatus::InternalError : StorageStatus::Ok;
    }
    if (!fs::is_directory(backup)) return StorageStatus::BackupNotFound;

    const fs::path staging = temporary_sibling(collection, ".restoring");
    const fs::path previous = temporary_sibling(collection, ".previous");
    try {
        copy_directory(backup, staging);
        if (fs::exists(collection)) {
            fs::rename(collection, previous, error);
            if (error) throw std::runtime_error("cannot stage current storage collection");
        }
        fs::rename(staging, collection, error);
        if (error) {
            if (fs::exists(previous)) fs::rename(previous, collection, error);
            throw std::runtime_error("cannot publish restored storage collection");
        }
        fs::remove_all(previous, error);
        return StorageStatus::Ok;
    } catch (...) {
        fs::remove_all(staging, error);
        if (!fs::exists(collection) && fs::exists(previous)) {
            fs::rename(previous, collection, error);
        }
        return StorageStatus::InternalError;
    }
}

fs::path tree_state_path(const fs::path& root, const StorageHandshake& request) {
    const std::string collection = qcovert::checked_identifier(
        request.collection, sizeof(request.collection), "collection");
    const std::string generation = qcovert::checked_identifier(
        request.generation, sizeof(request.generation), "generation");
    const std::string tree = request.tree_kind == static_cast<uint32_t>(StorageTreeKind::Cluster)
        ? "cluster.ring"
        : "ring.ring";
    return root / collection / generation / tree;
}

void write_tree_manifest(const fs::path& state_path, const StorageHandshake& request) {
    const fs::path path = state_path.string() + ".json";
    const fs::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create storage tree manifest");
    }
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"storage_protocol_version\": " << qcovert::kStorageProtocolVersion << ",\n"
           << "  \"tree\": \""
           << (request.tree_kind == static_cast<uint32_t>(StorageTreeKind::Cluster)
                   ? "cluster" : "ring")
           << "\",\n"
           << "  \"block_size\": " << request.block_size << ",\n"
           << "  \"real_bucket_size\": " << request.real_bucket_size << ",\n"
           << "  \"dummy_size\": " << request.dummy_size << ",\n"
           << "  \"evict_rate\": " << request.evict_rate << ",\n"
           << "  \"num_levels\": " << request.num_levels << ",\n"
           << "  \"cached_levels\": " << request.cached_levels << "\n"
           << "}\n";
    output.close();
    if (!output) {
        throw std::runtime_error("cannot write storage tree manifest");
    }
    std::error_code error;
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, error);
    if (error) throw std::runtime_error("cannot secure storage tree manifest: " + error.message());
    fs::rename(temporary, path, error);
    if (error) throw std::runtime_error("cannot publish storage tree manifest: " + error.message());
}

double_oram::RingOramConfig config_from_handshake(const StorageHandshake& request) {
    // The server only needs this tree layout. num_blocks is informational here;
    // RemoteRing derives allocation from num_levels and bucket sizes.
    return double_oram::RingOramConfig(
        request.block_size,
        request.real_bucket_size,
        request.dummy_size,
        request.evict_rate,
        1,
        request.num_levels,
        request.cached_levels);
}

void serve_connection(int accepted_socket, const fs::path& root) {
    try {
        set_socket_timeouts(accepted_socket);
        NetIO io(accepted_socket, false, true);
        StorageHandshake request{};
        io.recv_data(&request, sizeof(request));
        const StorageStatus validation = validate_handshake(request);
        if (validation != StorageStatus::Ok) {
            send_reply(io, validation);
            return;
        }

        const fs::path collection = collection_path(root, request);
        const std::shared_ptr<std::shared_mutex> collection_mutex =
            collection_lock_for(collection);
        const StorageAction action = static_cast<StorageAction>(request.action);
        if (action == StorageAction::DeleteCollection ||
            action == StorageAction::BackupCollection ||
            action == StorageAction::RestoreCollection ||
            action == StorageAction::DeleteBackup) {
            std::unique_lock<std::shared_mutex> collection_guard(
                *collection_mutex, std::try_to_lock);
            if (!collection_guard.owns_lock()) {
                send_reply(io, StorageStatus::TreeBusy);
                return;
            }
            send_reply(io, manage_collection(root, request));
            return;
        }
        std::shared_lock<std::shared_mutex> collection_guard(*collection_mutex);

        const fs::path state_path = tree_state_path(root, request);
        const std::shared_ptr<std::mutex> tree_mutex = tree_lock_for(state_path);
        std::unique_lock<std::mutex> tree_guard(*tree_mutex, std::try_to_lock);
        if (!tree_guard.owns_lock()) {
            send_reply(io, StorageStatus::TreeBusy);
            return;
        }
        const bool exists = fs::exists(state_path);
        if (action == StorageAction::Create && exists) {
            send_reply(io, StorageStatus::TreeAlreadyExists);
            return;
        }
        if (action == StorageAction::Open && !exists) {
            send_reply(io, StorageStatus::TreeNotFound);
            return;
        }

        const double_oram::RingOramConfig config = config_from_handshake(request);
        auto storage = std::make_unique<double_oram::RemoteRing>(
            &io, config, true, true, request.integrity != 0);
        if (action == StorageAction::Open) {
            try {
                storage->load_server_state(state_path.c_str());
            } catch (...) {
                send_reply(io, StorageStatus::TreeLoadFailed);
                return;
            }
        }

        send_reply(io, StorageStatus::Ok);
        storage->run_server();

        try {
            fs::create_directories(state_path.parent_path());
            storage->save_server_state(state_path.c_str());
            write_tree_manifest(state_path, request);
            // Publish availability before acknowledging the checkpoint. The
            // client may immediately reopen the tree after receiving this ACK;
            // keeping the session mutex until function teardown creates a race
            // where that valid reopen observes TreeBusy.
            tree_guard.unlock();
            collection_guard.unlock();
            send_reply(io, StorageStatus::Ok);
        } catch (const std::exception& error) {
            std::cerr << "[qcovert-storage] checkpoint error: " << error.what() << "\n";
            try {
                send_reply(io, StorageStatus::InternalError);
            } catch (...) {
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "[qcovert-storage] connection error: " << error.what() << "\n";
    }
}

class StorageWorkerPool {
public:
    StorageWorkerPool(size_t worker_count, size_t max_pending, fs::path root)
        : max_pending_(max_pending), root_(std::move(root)) {
        workers_.reserve(worker_count);
        for (size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~StorageWorkerPool() {
        shutdown();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (joined_) return;
            stopping_ = true;
        }
        condition_.notify_all();
        for (std::thread& worker : workers_) if (worker.joinable()) worker.join();
        workers_.clear();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            joined_ = true;
        }
    }

    bool submit(int socket_fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || pending_.size() >= max_pending_) return false;
        pending_.push_back(socket_fd);
        condition_.notify_one();
        return true;
    }

private:
    size_t max_pending_;
    fs::path root_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<int> pending_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    bool joined_ = false;

    void worker_loop() {
        while (true) {
            int socket_fd = -1;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
                if (stopping_ && pending_.empty()) return;
                socket_fd = pending_.front();
                pending_.pop_front();
            }
            serve_connection(socket_fd, root_);
        }
    }
};

int create_listener(int port) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        throw std::runtime_error("cannot create storage listener socket");
    }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(listener, 128) < 0) {
        close(listener);
        throw std::runtime_error("cannot bind storage listener");
    }
    return listener;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const ServerOptions options = parse_args(argc, argv);
        fs::create_directories(options.root);
        const int listener = create_listener(options.port);
        qcovert::ShutdownSignal shutdown_signal;
        StorageWorkerPool workers(options.worker_threads, options.max_pending_connections, options.root);
        std::cout << "[qcovert-storage] listening on 0.0.0.0:" << options.port
                  << " root=" << fs::absolute(options.root) << "\n";
        std::cout << "[qcovert-storage] press Ctrl+C or send SIGTERM for graceful shutdown\n";

        while (true) {
            pollfd descriptors[2] = {
                {listener, POLLIN, 0},
                {shutdown_signal.fd(), POLLIN, 0},
            };
            const int ready = poll(descriptors, 2, -1);
            if (ready < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("storage listener poll failed");
            }
            if ((descriptors[1].revents & POLLIN) != 0 && shutdown_signal.consume()) break;
            if ((descriptors[0].revents & POLLIN) == 0) continue;
            const int accepted = accept(listener, nullptr, nullptr);
            if (accepted < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("storage listener accept failed");
            }
            if (!workers.submit(accepted)) close(accepted);
        }
        close(listener);
        std::cout << "[qcovert-storage] shutdown requested; draining ORAM sessions\n";
        workers.shutdown();
        std::cout << "[qcovert-storage] graceful shutdown complete\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "qcovert_storage failed: " << error.what() << "\n";
        return 1;
    }
}
