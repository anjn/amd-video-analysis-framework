#pragma once
#include <cerrno>
#include <cstdint>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
namespace amdva::nvr::mc2ipc {
// Private same-host protocol. fd 3 carries binary data, leaving stdout/stderr for runtime logs.
inline void transfer(int fd, void *data, size_t bytes, bool writing, int timeout_ms = 120000) {
    auto *p = static_cast<char *>(data);
    while (bytes) {
        pollfd event{fd, short(writing ? POLLOUT : POLLIN), 0};
        int ready;
        do {
            ready = poll(&event, 1, timeout_ms);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0)
            throw std::runtime_error("MobileCLIP worker timeout or I/O failure");
        ssize_t n = writing ? send(fd, p, bytes, MSG_NOSIGNAL) : recv(fd, p, bytes, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw std::runtime_error("MobileCLIP worker disconnected");
        p += n;
        bytes -= n;
    }
}
inline uint32_t read(int fd, int timeout_ms = 120000) {
    uint32_t v;
    transfer(fd, &v, sizeof(v), false, timeout_ms);
    return v;
}
inline void write(int fd, uint32_t v) { transfer(fd, &v, sizeof(v), true); }
} // namespace amdva::nvr::mc2ipc
