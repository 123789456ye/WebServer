#ifndef BUFFER_H
#define BUFFER_H
#include <cstring>   //perror
#include <iostream>
#include <unistd.h>  // write
#include <sys/uio.h> //readv
#include <vector> //readv
#include <atomic>
#include <span>
#include <string_view>
#include <assert.h>
class Buffer {
public:
    explicit Buffer(int initBuffSize = 1024);
    ~Buffer() = default;

    size_t WritableBytes() const noexcept;       
    size_t ReadableBytes() const noexcept;
    size_t PrependableBytes() const noexcept;

    const char* Peek() const noexcept;
    void EnsureWriteable(size_t len);
    void HasWritten(size_t len);

    void Retrieve(size_t len);
    void RetrieveUntil(const char* end);

    void RetrieveAll() ;
    std::string RetrieveAllToStr();

    const char* BeginWriteConst() const noexcept;
    char* BeginWrite() noexcept;

    void Append(const char* data, size_t len);
    void Append(const void* data, size_t len);
    void Append(const std::string& str);
    void Append(const Buffer& other);

    ssize_t ReadFd(int fd, int* Errno);
    ssize_t WriteFd(int fd, int* Errno);

private:
    char* BeginPtr_() noexcept;
    const char* BeginPtr_() const noexcept;
    void MakeSpace_(size_t len);

    std::vector<char> buffer_;
    std::atomic<std::size_t> readPos_;
    std::atomic<std::size_t> writePos_;
};

#endif //BUFFER_H