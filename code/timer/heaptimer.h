#pragma once
#include <queue>
#include <unordered_map>
#include <time.h>
#include <algorithm>
#include <arpa/inet.h> 
#include <functional> 
#include <assert.h> 
#include <chrono>
#include "../log/log.h"

typedef std::function<void()> TimeoutCallBack;
typedef std::chrono::high_resolution_clock Clock;
typedef std::chrono::milliseconds MS;
typedef Clock::time_point TimeStamp;

struct TimerNode {
    int id;
    TimeStamp expires;
    TimeoutCallBack cb;
    bool operator<(const TimerNode& t) const {
        return expires < t.expires;
    }
};
class HeapTimer {
public:
    HeapTimer() { heap_.reserve(64); }
    ~HeapTimer() { clear(); }
    HeapTimer(const HeapTimer&) = delete;
    HeapTimer& operator=(const HeapTimer&) = delete;
    
    void adjust(int id, int new_timeout);
    void add(int id, int timeout, const TimeoutCallBack& cb);

    void do_work(int id);

    void clear();
    void tick();

    void pop();

    int get_next_tick();

private:
    void del_(size_t i);
    
    void siftup_(size_t i);

    bool siftdown_(size_t index, size_t n);

    void swap_node_(size_t i, size_t j);

    std::vector<TimerNode> heap_;

    std::unordered_map<int, size_t> ref_;

    std::mutex mtx_;
};
