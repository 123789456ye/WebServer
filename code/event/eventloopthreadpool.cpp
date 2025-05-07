//eventloopthreadpoo.cpp
#include "eventloopthreadpool.h"

EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop, int thread_num)
    : base_loop_(base_loop),
      started_(false),
      thread_num_(thread_num),
      next_loop_(0) {
        threads_.reserve(thread_num);
        loops_.reserve(thread_num);
}

void EventLoopThreadPool::start() {
    started_ = true;
    
    for (int i = 0; i < thread_num_; ++i) {
        auto t = std::make_unique<EventLoopThread>();
        loops_.push_back(t->start_loop());
        threads_.push_back(std::move(t));
    }
}

EventLoop* EventLoopThreadPool::get_next_loop() {
    EventLoop* loop = base_loop_;
    
    if (!loops_.empty()) {
        loop = loops_[next_loop_];
        next_loop_ = (next_loop_ + 1) % thread_num_;
    }
    
    return loop;
}