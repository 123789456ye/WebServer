#pragma once

#include "eventloop.h"

struct EventLoopThread {
public:
    EventLoopThread();
    ~EventLoopThread();
    
    // 启动事件循环线程
    EventLoop* start_loop();

private:
    // 线程函数
    void work();
    
    EventLoop* loop_;        // 事件循环
    bool exiting_;           // 是否退出
    std::thread thread_;     // 线程
    std::mutex mutex_;       // 互斥锁
    std::condition_variable cond_; // 条件变量
};