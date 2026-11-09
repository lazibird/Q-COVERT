#include "qcovert/shutdown_signal.h"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace qcovert {

namespace {

volatile sig_atomic_t shutdown_requested = 0;
int shutdown_write_fd = -1;

void handle_shutdown_signal(int) {
    const int saved_errno = errno;
    shutdown_requested = 1;
    if (shutdown_write_fd >= 0) {
        const unsigned char byte = 1;
        const ssize_t ignored = write(shutdown_write_fd, &byte, sizeof(byte));
        (void)ignored;
    }
    errno = saved_errno;
}

void set_nonblocking_close_on_exec(int fd) {
    const int status_flags = fcntl(fd, F_GETFL);
    const int descriptor_flags = fcntl(fd, F_GETFD);
    if (status_flags < 0 || descriptor_flags < 0 ||
        fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        throw std::runtime_error("cannot configure shutdown signal pipe");
    }
}

} // namespace

ShutdownSignal::ShutdownSignal() {
    if (shutdown_write_fd >= 0) {
        throw std::logic_error("only one ShutdownSignal may exist in a process");
    }
    if (pipe(pipe_fds_) != 0) {
        throw std::runtime_error("cannot create shutdown signal pipe");
    }
    try {
        set_nonblocking_close_on_exec(pipe_fds_[0]);
        set_nonblocking_close_on_exec(pipe_fds_[1]);

        struct sigaction action {};
        action.sa_handler = handle_shutdown_signal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        shutdown_requested = 0;
        shutdown_write_fd = pipe_fds_[1];
        if (sigaction(SIGINT, &action, &previous_sigint_) != 0) {
            throw std::runtime_error("cannot install shutdown signal handlers");
        }
        if (sigaction(SIGTERM, &action, &previous_sigterm_) != 0) {
            sigaction(SIGINT, &previous_sigint_, nullptr);
            throw std::runtime_error("cannot install shutdown signal handlers");
        }
    } catch (...) {
        shutdown_write_fd = -1;
        close(pipe_fds_[0]);
        close(pipe_fds_[1]);
        pipe_fds_[0] = -1;
        pipe_fds_[1] = -1;
        throw;
    }
}

ShutdownSignal::~ShutdownSignal() {
    shutdown_write_fd = -1;
    sigaction(SIGINT, &previous_sigint_, nullptr);
    sigaction(SIGTERM, &previous_sigterm_, nullptr);
    if (pipe_fds_[0] >= 0) close(pipe_fds_[0]);
    if (pipe_fds_[1] >= 0) close(pipe_fds_[1]);
}

int ShutdownSignal::fd() const {
    return pipe_fds_[0];
}

bool ShutdownSignal::consume() {
    unsigned char buffer[64];
    while (read(pipe_fds_[0], buffer, sizeof(buffer)) > 0) {
    }
    return shutdown_requested != 0;
}

} // namespace qcovert
