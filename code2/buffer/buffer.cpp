#include "buffer.h"

inline ssize_t Buffer::read_fd(int fd, int* saved_errno) noexcept {
    alignas(alignof(std::max_align_t)) char extra_buffer[kExtraSize];

    iovec vec[2];
    const size_t writable = writable_bytes();

    vec[0].iov_base = begin_write();
    vec[0].iov_len = writable;
    vec[1].iov_base = extra_buffer;
    vec[1].iov_len = sizeof(extra_buffer);

    const ssize_t n = readv(fd, vec, 2);
    if(n < 0) {
        *saved_errno = errno;
    }
    else if(static_cast<size_t>(n) <= writable) {
        write_pos_ += n;
    }
    else {
        write_pos_ = buffer_.size();
        append(extra_buffer, n - writable);
    }
    return n;
}

inline ssize_t Buffer::write_fd(int fd, int* saved_errno) noexcept {
    const auto view = readable_view();
    const ssize_t n = write(fd, view.data(), view.size());
    if(n < 0) {
        *saved_errno = errno;
        return n;
    }
    read_pos_ += n;
    return n;
}