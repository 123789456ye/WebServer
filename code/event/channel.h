#pragma once

#include <functional>
#include <memory>
#include "epoller.h"
#include "eventloop.h"
#include "../http/httpconn.h"

struct EventLoop;

struct Channel {
public:
    using EventCallback = std::function<void()>;
    
    Channel() = default;
    Channel(EventLoop* loop, int fd);
    ~Channel() = default;
    
    void set_read_callback(EventCallback cb) { read_callback_ = std::move(cb); }
    void set_write_callback(EventCallback cb) { write_callback_ = std::move(cb); }
    void set_update_callback(EventCallback cb) { update_callback_ = std::move(cb); }
    void set_close_callback(EventCallback cb) { close_callback_ = std::move(cb); }
    void set_error_callback(EventCallback cb) { error_callback_ = std::move(cb); }
    
    void handle_event();
    
    int fd() const { return fd_; }
    void set_fd(int fd) {fd_ = fd;}
    
    int& events() { return events_; }
    void set_events(int events) { events_ = events; }
    
    void update();
    void remove(); 

    void disable_all() { events_ = 0; update(); }
    void enable_reading() { events_ |= EPOLLIN; update(); }
    void enable_writing() { events_ |= EPOLLOUT; update(); }

private:
    EventLoop* loop_;
    int fd_;
    int events_;
    
    EventCallback read_callback_;
    EventCallback write_callback_;
    EventCallback update_callback_;
    EventCallback close_callback_;
    EventCallback error_callback_;
};