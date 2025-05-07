//epoller.cpp
#include "epoller.h"

Epoller::Epoller(int max_events) : epoll_fd_(epoll_create(512)), events_(max_events) {}

Epoller::~Epoller() {
    close(epoll_fd_);
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

    epoll_event ev{0};
    return epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &ev) == 0;
}

int Epoller::wait(int timeout_ms) {
    return epoll_wait(epoll_fd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
}

int Epoller::get_event_fd(size_t i) const {
    return events_[i].data.fd;
}

uint32_t Epoller::get_events(size_t i) const {
    return events_[i].events;
}