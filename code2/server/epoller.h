#pragma once

#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <stdexcept>
#include <string>
#include <cerrno>
#include <system_error>

class Epoller {
public:
    explicit Epoller(int max_events = 1024);
    ~Epoller();

    Epoller(const Epoller&) = delete;
    Epoller& operator=(const Epoller&) = delete;

    bool add_fd(int fd, uint32_t events);
    bool mod_fd(int fd, uint32_t events);
    bool del_fd(int fd);

    int wait(int timeout_ms = -1);

    int get_event_fd(size_t i) const;

    uint32_t get_events(size_t i) const;

    size_t size() const noexcept { return events_.size(); }

private:
    int epoll_fd_;                   
    std::vector<epoll_event> events_; 
};
