#include "epoller.h"

Epoller::Epoller(int max_events) : epoll_fd_(-1), events_(max_events) {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::system_error(errno, std::system_category(), "epoll_create1 failed");
    }
}

Epoller::~Epoller() {
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

bool Epoller::add_fd(int fd, uint32_t events) {
    if (fd < 0) return false;

    epoll_event ev{};  
    ev.data.fd = fd;
    ev.events = events;
    
    return epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool Epoller::mod_fd(int fd, uint32_t events) {
    if (fd < 0) return false;

    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events;
    
    return epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool Epoller::del_fd(int fd) {
    if (fd < 0) return false;

    return epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
}

int Epoller::wait(int timeout_ms) {
    int events_count = epoll_wait(epoll_fd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
    
    if (events_count < 0 && errno != EINTR) {
        throw std::system_error(errno, std::system_category(), "epoll_wait failed");
    }
    
    return events_count;
}

int Epoller::get_event_fd(size_t i) const {
    if (i >= events_.size()) {
        throw std::out_of_range("Event index out of range");
    }
    return events_[i].data.fd;
}

uint32_t Epoller::get_events(size_t i) const {
    if (i >= events_.size()) {
        throw std::out_of_range("Event index out of range");
    }
    return events_[i].events;
}