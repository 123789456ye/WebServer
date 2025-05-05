#pragma once
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <functional>
class ThreadPool {
public:
    explicit ThreadPool(size_t thread_count = std::thread::hardware_concurrency())
    : pool_(std::make_shared<Pool>()) {
        workers_.reserve(thread_count);
        for(size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([pool = pool_](std::stop_token st){
                std::unique_lock lck(pool->mtx);
                
                while(!st.stop_requested()) {
                    if(!pool->tasks.empty()) {
                        auto task = std::move(pool->tasks.front());
                        pool->tasks.pop();

                        lck.unlock();
                        task();
                        lck.lock();
                    }
                    else {
                        pool->cond.wait(lck, st, [&pool]{return !pool->tasks.empty();});
                    }
                }
            });
        }
    }

    ThreadPool() = default;
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = default;

    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool& operator=(ThreadPool&&) = default;
    
    ~ThreadPool() {
        for (auto& worker : workers_) {
            worker.request_stop();
        }
        pool_->cond.notify_all();
    }

    template<class F>
    void add_task(F&& task) {
        {
            std::lock_guard<std::mutex> locker(pool_->mtx);
            pool_->tasks.emplace(std::forward<F>(task));
        }
        pool_->cond.notify_one();
    }

private:
    struct Pool {
        std::mutex mtx;
        std::condition_variable_any cond;
        std::queue<std::function<void()>> tasks;
    };
    std::shared_ptr<Pool> pool_;
    std::vector<std::jthread> workers_;
};
