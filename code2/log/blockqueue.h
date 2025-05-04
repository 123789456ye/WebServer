#ifndef BLOCKQUEUE_H
#define BLOCKQUEUE_H
#endif
#pragma once

#include <mutex>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <cassert>

using std::lock_guard;
using std::mutex;

template<class T>
class BlockDeque {
public:
    explicit BlockDeque(size_t max_capacity = 1000):
    capacity_(max_capacity),is_closed_(false) {
        assert(max_capacity > 0);
    }

    ~BlockDeque() {close();}

    BlockDeque(const BlockDeque&) = delete;
    BlockDeque& operator=(const BlockDeque&) = delete;

    void clear() {
        lock_guard<mutex> lock(mtx_);
        deq_.clear();
    }

    bool empty() {
        lock_guard<mutex> lock(mtx_);
        return deq_.empty();
    }

    bool full() {
        lock_guard<mutex> lock(mtx_);
        return deq_.size() >= capacity_;
    }

    void close() {
        clear();
        is_closed_ = true;
        cond_consumer_.notify_all();
        cond_producer_.notify_all();
    }

    size_t size() {
        lock_guard<mutex> lock(mtx_);
        return deq_.size();
    }

    size_t capacity() {
        return capacity_;
    }

    T front() {
        lock_guard<mutex> lock(mtx_);
        return deq_.front();
    }

    T back() {
        lock_guard<mutex> lock(mtx_);
        return deq_.back();
    }

    void push_back(const T &item) {
        unique_lock<mutex> lock(mtx_);
        cond_producer_.wait(lock,[this]{return deq_.size() < capacity_;})
        deq_.push_back(item);
        cond_consumer_.notify_one();
    }

    void push_front(const T &item) {
        unique_lock<mutex> lock(mtx_);
        cond_producer_.wait(lock,[this]{return deq_.size() < capacity_;})
        deq_.push_front(item);
        cond_consumer_.notify_one();
    }

    bool pop(T &item) {
        unique_lock<mutex> lock(mtx_);
        cond_consumer_.wait(lock,[this]{return is_closed_|| !deq_.empty();})
        if(is_closed_) return false;
        item = deq_.front();
        deq_.pop_front();
        cond_conproducer_.notify_one();
        return true;
    }

    bool pop(T &item, int timeout) {
        unique_lock<mutex> lock(mtx_);
        bool success = cond_consumer_.wait_for(lock, std::chrono::seconds(timeout), [this]{return is_closed_|| !deq_.empty();})
        if(!success || is_closed_) return false;
        item = deq_.front();
        deq_.pop_front();
        cond_producer_.notify_one();
        return true;
    }

    void flush() {
        cond_consumer_.notify_one();
    }

private:
    std::deque<T> deq_;
    size_t capacity_;

    bool is_closed_;

    std::mutex mtx_;
    std::condition_variable cond_consumer_;
    std::condition_variable cond_producer_;
};
