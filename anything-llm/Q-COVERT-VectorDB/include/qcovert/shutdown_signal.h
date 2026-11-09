#ifndef QCOVERT_SHUTDOWN_SIGNAL_H
#define QCOVERT_SHUTDOWN_SIGNAL_H

#include <csignal>

namespace qcovert {

/**
 * Converts SIGINT/SIGTERM into a pollable file descriptor. The signal handler
 * only writes one byte, so all non-signal-safe shutdown work stays in main().
 */
class ShutdownSignal {
public:
    ShutdownSignal();
    ~ShutdownSignal();

    ShutdownSignal(const ShutdownSignal&) = delete;
    ShutdownSignal& operator=(const ShutdownSignal&) = delete;

    /** Read end suitable for poll(2). */
    int fd() const;

    /** Consume pending wakeup bytes and report whether shutdown was requested. */
    bool consume();

private:
    int pipe_fds_[2] = {-1, -1};
    struct sigaction previous_sigint_ {};
    struct sigaction previous_sigterm_ {};
};

} // namespace qcovert

#endif // QCOVERT_SHUTDOWN_SIGNAL_H
