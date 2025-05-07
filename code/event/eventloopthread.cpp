//eventloopthread.cpp
#include "eventloopthread.h"

EventLoopThread::EventLoopThread()
    : loop_(nullptr),
      exiting_(false) {
}

EventLoopThread::~EventLoopThread() {
    exiting_ = true;
    if (loop_ != nullptr) {
        loop_->quit();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

EventLoop* EventLoopThread::start_loop() {
    thread_ = std::thread(&EventLoopThread::work, this);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (loop_ == nullptr) {
            cond_.wait(lock);
        }
    }
    
    return loop_;
}

void EventLoopThread::work() {
    EventLoop loop;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
    }
    
    loop.loop();
    
    //std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
}
