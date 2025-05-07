//channel.cpp
#include "channel.h"
#include "eventloop.h"

Channel::Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd), events_(0) {}

void Channel::update() {
    loop_->update_channel(this);
}

void Channel::remove() {
    loop_->remove_channel(this);
}

void Channel::handle_event() {
    if ((events_ & EPOLLHUP) && !(events_ & EPOLLIN)) {
        if(close_callback_) close_callback_();
        events_ = 0;
        return;
    }
    if (events_ & EPOLLERR) {
        if(error_callback_) error_callback_();
        events_ = 0;
        return;
    } 
    //触发可读事件 | 高优先级可读 | 对端（客户端）关闭连接
    if (events_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) 
        if(read_callback_) 
            read_callback_();
    if (events_ & EPOLLOUT) 
        if(write_callback_) 
            write_callback_();

    if(update_callback_) 
        update_callback_();
    LOG_DEBUG("Handle event on %d", events_);
}

    