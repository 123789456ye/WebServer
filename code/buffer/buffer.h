#pragma once
#include <cassert>
#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <concepts>
#include <cstring>
#include <atomic>
#include <sys/uio.h>

class Buffer {
public:
    static constexpr size_t kInitialSize = 1024;
    static constexpr size_t kMaxSize = 64 * 1024 * 1024; 
    static constexpr size_t kExtraSize = 65536; 

    explicit Buffer(size_t initsize = kInitialSize): buffer_(initsize){}

    std::span<const char> readable_view() const noexcept {
        return {data() + read_pos_, readable_bytes()};
    }

    std::span<char> writable_view() noexcept {
        return {begin_write(), writable_bytes()};
    }

    size_t readable_bytes() const noexcept {
        return write_pos_ - read_pos_;
    }

    size_t writable_bytes() const noexcept {
        return buffer_.size() - write_pos_;
    }

    size_t prependable_bytes() const noexcept {
        return read_pos_;
    }

    template<typename T>
    void append(const T* data, size_t len) {
        assert(data);
        ensure_writable(len);
        std::memcpy(begin_write(), data, len);
        has_written(len);
    }

    void append(const std::string& str) {
        append(str.data(), str.length());
    }   

    void retrieve(size_t len) noexcept {
        read_pos_ += len;
    }

    void retrieve_until(const char* end) noexcept {
        retrieve(end - begin_read());
    }

    void retrieve_all() noexcept {
        reset();
    }

    std::string retrieve_allstring() {
        std::string res(begin_read(), readable_bytes());
        retrieve_all();
        return res;
    }

    char* begin_read() noexcept {
        return data() + read_pos_;
    }

    char* begin_write() noexcept {
        return data() + write_pos_;
    }

    void has_written(size_t len) noexcept {
        write_pos_ += len;
    }

    ssize_t read_fd(int fd, int* saved_errno) noexcept;
    ssize_t write_fd(int fd, int* saved_errno) noexcept;

private:
    void reset() noexcept {
        read_pos_ = write_pos_ = 0;
    }

    char* data() noexcept {
        return buffer_.data();
    }

    const char* data() const noexcept {
        return buffer_.data();
    }

    void ensure_writable(size_t len) {
        if(writable_bytes() < len)
            make_space(len);
    }

    void make_space(size_t len) {
        const size_t writable = writable_bytes();
        const size_t prependable = prependable_bytes();

        if(writable + prependable < len)
        {
            const size_t new_size = write_pos_ + len + 1;
            if(new_size > kMaxSize) {
                throw std::length_error("Buffer size exceed maximum");
            }
            buffer_.resize(new_size);
        }
        else {
            size_t readable = readable_bytes();
            std::memmove(data(), data() + read_pos_, readable);
            read_pos_ = 0;
            write_pos_ = readable;
        }
    }

    std::vector<char> buffer_;
    std::atomic<std::size_t> read_pos_{0};
    std::atomic<std::size_t> write_pos_{0};
};
