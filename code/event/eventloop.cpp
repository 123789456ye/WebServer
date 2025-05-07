// eventloop.cpp
#include "eventloop.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>


// 创建eventfd用于线程间通知
int EventLoop::create_eventfd() {
    return eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

// EventLoop实现
EventLoop::EventLoop()
    : epoller_(new Epoller()),
      wakeup_fd_(create_eventfd()),
      wakeup_channel_(new Channel(this, wakeup_fd_)),
      quit_(false),
      thread_id_(std::this_thread::get_id()),
      calling_pending_functors_(false){
    
    // 设置唤醒通道的回调
    wakeup_channel_->set_events(EPOLLIN | EPOLLET);
    wakeup_channel_->set_read_callback(std::bind(&EventLoop::handle_wakeup, this));
    wakeup_channel_->set_update_callback(std::bind(&EventLoop::handle_update, this));
    epoller_->add_fd(wakeup_channel_->fd(), wakeup_channel_->events());
}

EventLoop::~EventLoop() {
    quit_ = true;
    close(wakeup_fd_);
}

void EventLoop::loop(HeapTimer* timer, int timeout) {
    quit_ = false;
    int time_ms = -1;
    while (!quit_) {
        if(timer != nullptr) time_ms = timer->get_next_tick();
        else time_ms = timeout;
        int num_events = epoller_->wait(time_ms);
        
        for (int i = 0; i < num_events; ++i) {
            int fd = epoller_->get_event_fd(i);
            std::lock_guard<std::mutex> lock(mutex_);            
            auto it = channels_.find(fd);
            if (it != channels_.end()) {
                it->second->handle_event();
            }
        }
        
        do_pending_functors();
    }
}

void EventLoop::quit() {
    quit_ = true;
    if (!is_in_loop_thread()) {
        wakeup();
    }
}

void EventLoop::run_in_loop(std::function<void()> cb) {
    if (is_in_loop_thread()) {
        cb();
    } else {
        queue_in_loop(std::move(cb));
    }
}

void EventLoop::queue_in_loop(std::function<void()> cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_functors_.emplace_back(std::move(cb));
    }
    
    if (!is_in_loop_thread() || calling_pending_functors_) {
        wakeup();
    }
}

void EventLoop::wakeup() {
    uint64_t one = 1;
    ssize_t n = write(wakeup_fd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        std::cerr << "EventLoop::wakeup() writes " << n << " bytes instead of 8" << std::endl;
    }
}

void EventLoop::handle_wakeup() {
    uint64_t one = 1;
    ssize_t n = read(wakeup_fd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        std::cerr << "EventLoop::handleRead() reads " << n << " bytes instead of 8" << std::endl;
    }
    wakeup_channel_->set_events(EPOLLIN | EPOLLET);
}

void EventLoop::do_pending_functors() {
    std::vector<std::function<void()>> functors;
    calling_pending_functors_ = true;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pending_functors_);
    }
    
    for (const auto& functor : functors) {
        functor();
    }
    
    calling_pending_functors_ = false;
}

void EventLoop::handle_update() {
    modify_channel(wakeup_channel_.get());
}

void EventLoop::update_channel(Channel* channel) {
    int fd = channel->fd();
    
    if (channel->events() != 0) {

        if(channels_.count(fd))
            epoller_->mod_fd(fd, channel->events());
        else
            epoller_->add_fd(fd, channel->events());
    } 
    else {
        epoller_->del_fd(fd);
    }

    channels_[fd] = channel;
}

void EventLoop::remove_channel(Channel* channel) {
    int fd = channel->fd();
    {
        std::lock_guard<std::mutex> lck(mutex_channel_);
        channels_.erase(fd);
    }
    epoller_->del_fd(fd);
}

void EventLoop::modify_channel(Channel* channel) {
    epoller_->mod_fd(channel->fd(), channel->events());
}