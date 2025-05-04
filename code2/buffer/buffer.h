#ifndef BUFFER_H
#define BUFFER_H
#endif

#pragma once
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

    explicit Buffer(size_t initsize=kInitialSize): buffer_(initsize){}

    std::span<const char> readable_view() const noexcept {
        return {data() + read_pos_, readable_bytes()};
    }

    std::span<char> writable_view() noexcept {
        return {data() + write_pos_, writable_bytes()};
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
    requires std::ranges::contiguous_range<T>
    void append(const T& data) {
        append(std::ranges::data(data), std::ranges::size(data));
    }

    void append(const void* data,size_t len) {
        if(len == 0) return;
        ensure_writable(len);
        std::memcpy(begin_write(), data, len);
        has_written(len);
    }

    void append(const Buffer& other) {
        append(other.readable_view());
    }

    void retrieve(size_t len) noexcept {
        if(len >= readable_bytes()) {reset();}
        else {read_pos_ += len;}
    }

    void retrieve_until(const char* end) noexcept {
        retrieve(end - (data() + read_pos_));
    }

    void Buffer::retrieve_all() {
        reset();
    }

    std::string retrieve_string(size_t len) {
        std::string res(data() + read_pos_, len);
        retrieve_all();
        return res;
    }

    void shrink_to_fit() noexcept {
        buffer_.shrink_to_fit();
    }

    void reserve(size_t new_cap) {
        if(new_cap > buffer_.capacity())
            buffer_.reserve(std::min(new_cap, kMaxSize));
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

    char* begin_write() noexcept {
        return data() + write_pos_;
    }

    void ensure_writable(size_t len) {
        if(writable_bytes() < len)
            make_space(len);
    }

    void has_written(size_t len) noexcept {
        write_pos_ += len;
    }

    void make_space(size_t len) {
        const size_t writable = writable_bytes();
        const size_t prependable = prependable_bytes();

        if(writable + prependable < len)
        {
            const size_t new_size = write_pos_ + len;
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
