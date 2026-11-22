#pragma once

#include "avcrt_adts.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>

namespace avcrt::adts {

// Take an independent nonblocking writer for a pipe. Do not set O_NONBLOCK
// on a dup: the caller's descriptor would inherit that change.
inline int duplicate_output(int fd) {
    const int held = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (held < 0) return -1;
    struct stat st{};
    const int flags = fcntl(held, F_GETFL);
    if (flags < 0 || fstat(held, &st) != 0 || (flags & O_APPEND)) { close(held); return -1; }
    if (S_ISREG(st.st_mode) && (flags & O_ACCMODE) != O_RDONLY &&
        st.st_size == 0 && lseek(held, 0, SEEK_CUR) == 0) return held;
    if (!S_ISFIFO(st.st_mode) || (flags & O_ACCMODE) != O_WRONLY) { close(held); return -1; }
    char path[48];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", held);
    const int writer = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    close(held);
    if (writer < 0) return -1;
    struct stat reopened{};
    pollfd item{writer, POLLOUT, 0};
    // Require space for a complete ADTS AU in an atomic pipe write; the
    // caller can enforce the real PIPE_BUF bound on each individual AU.
    if (fstat(writer, &reopened) != 0 || !S_ISFIFO(reopened.st_mode) ||
        st.st_dev != reopened.st_dev || st.st_ino != reopened.st_ino ||
        fpathconf(writer, _PC_PIPE_BUF) < long(kHeader + 1) || poll(&item, 1, 0) < 0 ||
        (item.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        close(writer);
        return -1;
    }
    return writer;
}

// Worker must have SIGPIPE blocked. Never retry after a partial AU, or wait
// indefinitely for a reader. Only call permit() when ready to publish.
template<class Permit>
bool write_frame(int fd, const uint8_t* data, size_t size, long pipe_buf, Permit permit) {
    if (size < kHeader + 1 || size > kMaxFrame || (pipe_buf > 0 && size > size_t(pipe_buf)))
        return false;
    for (int retry = 0; retry < 8; ++retry) {
        if (!permit()) return false;
        const ssize_t n = write(fd, data, size);
        if (n == ssize_t(size)) return true;
        if (n >= 0 || (errno != EINTR && (pipe_buf <= 0 || errno != EAGAIN))) return false;
        if (errno == EAGAIN) {
            pollfd item{fd, POLLOUT, 0};
            if (poll(&item, 1, 20) > 0 && (item.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
        }
    }
    return false;
}

} // namespace avcrt::adts
