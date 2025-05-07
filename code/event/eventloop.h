#pragma once

#include <functional>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <unordered_map>
#include "channel.h"
#include "../timer/heaptimer.h"

struct Channel;

// 事件循环类：Reactor核心
class EventLoop {
public:
    EventLoop();
    ~EventLoop();
    
    void loop(HeapTimer* timer = nullptr, int timeout_ms = 10000);
    void quit();
    
    bool is_in_loop_thread() const { 
        if(quit_) return false;
        return thread_id_ == std::this_thread::get_id(); 
    }
    
    void run_in_loop(std::function<void()> cb);
    
    void queue_in_loop(std::function<void()> cb);
    
    // 唤醒事件循环
    void wakeup();
    
    void update_channel(Channel* channel);
    void remove_channel(Channel* channel);
    void modify_channel(Channel* channel);

private:
    static int create_eventfd();
    // 处理唤醒事件
    void handle_update();
    void handle_wakeup();
    
    // 执行待处理任务
    void do_pending_functors();
    
    std::unique_ptr<Epoller> epoller_;            // IO复用
    int wakeup_fd_;                              // 唤醒fd
    std::unique_ptr<Channel> wakeup_channel_;    // 唤醒通道
    std::atomic<bool> quit_;                     // 是否退出
    
    std::thread::id thread_id_;                  // 事件循环所在线程ID
    
    std::mutex mutex_;                           // 保护任务队列
    std::mutex mutex_channel_;
    std::vector<std::function<void()>> pending_functors_; // 待处理任务
    std::atomic<bool> calling_pending_functors_; // 是否正在处理任务
    
    // 通道映射表
    std::unordered_map<int, Channel*> channels_;
};

