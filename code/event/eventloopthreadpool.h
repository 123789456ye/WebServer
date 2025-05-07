#pragma once

#include <functional>
#include <memory>
#include <vector>
#include "eventloopthread.h"

struct EventLoopThreadPool {
public:
    EventLoopThreadPool(EventLoop* base_loop, int thread_num);
    ~EventLoopThreadPool() = default;
    
    void start();
    
    EventLoop* get_next_loop();

private:
    EventLoop* base_loop_;   // 主事件循环
    bool started_;           // 是否已启动
    int thread_num_;         // 线程数量
    int next_loop_;          // 下一个循环索引
    
    std::vector<std::unique_ptr<EventLoopThread>> threads_; // 线程池
    std::vector<EventLoop*> loops_;                         // 事件循环池
};

