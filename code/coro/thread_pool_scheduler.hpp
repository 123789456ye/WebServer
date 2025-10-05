#pragma once

#include <liburing.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <coroutine>
#include <unordered_map>
#include <functional>
#include <system_error>
#include <cassert>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <random>

#include "stdexec/execution.hpp"
#include "exec/task.hpp"

namespace coro {

namespace stdx = stdexec;

// Forward declarations
class ThreadPoolScheduler;

// Task types that can be executed by the thread pool
struct Task {
    std::function<void()> function;

    Task() = default;
    Task(std::function<void()> f) : function(std::move(f)) {}
};

// Work-stealing deque for efficient task distribution
class WorkStealingQueue {
public:
    WorkStealingQueue() = default;

    // Push task to the back (called by owner thread)
    void push(Task task) {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
        cv_.notify_one();
    }

    // Pop task from the back (called by owner thread)
    bool pop(Task& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tasks_.empty()) {
            return false;
        }
        task = std::move(tasks_.back());
        tasks_.pop_back();
        return true;
    }

    // Steal task from the front (called by other threads)
    bool steal(Task& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tasks_.empty()) {
            return false;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
        return true;
    }

    // Wait for tasks to become available
    void wait_for_tasks() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !tasks_.empty() || should_stop_; });
    }

    // Signal all waiting threads to stop
    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        should_stop_ = true;
        cv_.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    bool should_stop_ = false;
};

// I/O operation types
enum class UringOp : uint8_t {
    Read,
    Write,
    Accept,
    Connect,
    Close
};

// Completion callback for I/O operations
struct CompletionCallback {
    std::function<void(int result, std::error_code ec)> callback;

    CompletionCallback() = default;
    CompletionCallback(std::function<void(int, std::error_code)> cb) : callback(std::move(cb)) {}
};

// Thread pool scheduler with work stealing and shared io_uring
class ThreadPoolScheduler {
public:
    explicit ThreadPoolScheduler(unsigned num_threads = std::thread::hardware_concurrency(),
                                unsigned queue_depth = 1024);
    ~ThreadPoolScheduler();

    ThreadPoolScheduler(const ThreadPoolScheduler&) = delete;
    ThreadPoolScheduler& operator=(const ThreadPoolScheduler&) = delete;

    // Start the thread pool
    void start();

    // Stop the thread pool
    void stop();

    // Submit a task to the thread pool
    void submit_task(Task task);

    // Submit an I/O operation
    void submit_operation(UringOp op, int fd, void* buffer, size_t size,
                         CompletionCallback callback, int flags = 0);

private:
    // Modern sender for schedule operations
    struct ScheduleSender {
        ThreadPoolScheduler* scheduler_;

        using sender_concept = stdx::sender_t;
        using completion_signatures = stdx::completion_signatures<stdx::set_value_t()>;

        template<typename Receiver>
        auto connect(Receiver&& receiver) const noexcept {
            struct Operation {
                ThreadPoolScheduler* scheduler_;
                [[no_unique_address]] std::decay_t<Receiver> receiver_;

                using operation_state_concept = stdx::operation_state_t;

                void start() noexcept {
                    // Schedule operations: submit to thread pool for execution
                    scheduler_->submit_task(Task{[receiver = std::move(receiver_)]() mutable {
                        stdx::set_value(std::move(receiver));
                    }});
                }
            };

            return Operation{scheduler_, std::forward<Receiver>(receiver)};
        }

        auto get_env() const noexcept {
            struct Env {
                ThreadPoolScheduler* scheduler_;

                auto query(stdx::get_completion_scheduler_t<stdx::set_value_t>) const noexcept -> ThreadPoolScheduler& {
                    return *scheduler_;
                }
            };
            return Env{scheduler_};
        }
    };

    // I/O sender that submits to shared io_uring
    template<UringOp Op>
    struct IoSender {
        ThreadPoolScheduler* scheduler_;
        int fd_;
        void* buffer_;
        size_t size_;
        int flags_;

        using sender_concept = stdx::sender_t;
        using completion_signatures = stdx::completion_signatures<
            stdx::set_value_t(int),
            stdx::set_error_t(std::exception_ptr)
        >;

        template<typename Receiver>
        auto connect(Receiver&& receiver) const noexcept {
            struct Operation {
                ThreadPoolScheduler* scheduler_;
                int fd_;
                void* buffer_;
                size_t size_;
                int flags_;
                [[no_unique_address]] std::decay_t<Receiver> receiver_;

                using operation_state_concept = stdx::operation_state_t;

                void start() noexcept {
                    try {
                        // Submit I/O operation to shared io_uring
                        CompletionCallback callback{[receiver = std::move(receiver_)](int result, std::error_code ec) mutable {
                            if (ec) {
                                auto ex = std::make_exception_ptr(std::system_error(ec));
                                stdx::set_error(std::move(receiver), ex);
                            } else {
                                stdx::set_value(std::move(receiver), result);
                            }
                        }};

                        scheduler_->submit_operation(Op, fd_, buffer_, size_, std::move(callback), flags_);

                    } catch (...) {
                        stdx::set_error(std::move(receiver_), std::current_exception());
                    }
                }
            };

            return Operation{scheduler_, fd_, buffer_, size_, flags_, std::forward<Receiver>(receiver)};
        }

        auto get_env() const noexcept {
            struct Env {
                ThreadPoolScheduler* scheduler_;
                auto query(stdx::get_completion_scheduler_t<stdx::set_value_t>) const noexcept -> ThreadPoolScheduler& {
                    return *scheduler_;
                }
            };
            return Env{scheduler_};
        }
    };

public:
    // Scheduler concept compliance
    using scheduler_concept = stdx::scheduler_t;

    auto schedule() const noexcept -> ScheduleSender {
        return {const_cast<ThreadPoolScheduler*>(this)};
    }

    bool operator==(const ThreadPoolScheduler& other) const noexcept {
        return this == &other;
    }

    // I/O operation factories
    auto async_read(int fd, void* buffer, size_t size) const noexcept {
        return IoSender<UringOp::Read>{const_cast<ThreadPoolScheduler*>(this), fd, buffer, size, 0};
    }

    auto async_write(int fd, const void* buffer, size_t size) const noexcept {
        return IoSender<UringOp::Write>{const_cast<ThreadPoolScheduler*>(this), fd, const_cast<void*>(buffer), size, 0};
    }

    auto async_accept(int listen_fd, sockaddr* addr = nullptr, socklen_t* addrlen = nullptr) const noexcept {
        return IoSender<UringOp::Accept>{const_cast<ThreadPoolScheduler*>(this), listen_fd, addr, addrlen ? *addrlen : 0, 0};
    }

    auto async_connect(int fd, const sockaddr* addr, socklen_t addrlen) const noexcept {
        return IoSender<UringOp::Connect>{const_cast<ThreadPoolScheduler*>(this), fd, const_cast<sockaddr*>(addr), addrlen, 0};
    }

    auto async_close(int fd) const noexcept {
        return IoSender<UringOp::Close>{const_cast<ThreadPoolScheduler*>(this), fd, nullptr, 0, 0};
    }

private:
    // Worker thread function
    void worker_thread(size_t thread_id);

    // Try to get work (own queue first, then steal from others)
    bool try_get_work(size_t thread_id, Task& task);

    // Process io_uring completions (called by any thread)
    void process_completions();

    // Thread pool state
    std::vector<std::unique_ptr<WorkStealingQueue>> work_queues_;
    std::vector<std::jthread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> next_queue_{0};

    // Shared io_uring instance
    struct io_uring ring_;
    std::mutex ring_mutex_;  // Protect io_uring submissions
    std::unordered_map<uint64_t, CompletionCallback> pending_ops_;
    std::mutex pending_ops_mutex_;  // Protect pending operations map
    uint64_t next_user_data_ = 1;

    // Random number generator for work stealing (moved to member variables)
    std::mt19937 gen_;

    uint64_t generate_user_data() { return next_user_data_++; }
};

// Implementation
inline ThreadPoolScheduler::ThreadPoolScheduler(unsigned num_threads, unsigned queue_depth)
    : gen_(std::random_device{}()) {  // Initialize the random generator
    // Initialize io_uring
    int ret = io_uring_queue_init(queue_depth, &ring_, 0);
    if (ret < 0) {
        throw std::system_error(-ret, std::system_category(), "io_uring_queue_init");
    }

    // Create work-stealing queues for each thread
    for (unsigned i = 0; i < num_threads; ++i) {
        work_queues_.emplace_back(std::make_unique<WorkStealingQueue>());
    }
}

inline ThreadPoolScheduler::~ThreadPoolScheduler() {
    stop();
    io_uring_queue_exit(&ring_);
}

inline void ThreadPoolScheduler::start() {
    if (running_.exchange(true)) {
        return; // Already running
    }

    std::cout << "DEBUG: ThreadPoolScheduler::start() - starting " << work_queues_.size() << " worker threads\n";

    // Start worker threads
    for (size_t i = 0; i < work_queues_.size(); ++i) {
        workers_.emplace_back([this, i]() {
            std::cout << "DEBUG: Worker thread " << i << " starting\n";
            worker_thread(i);
            std::cout << "DEBUG: Worker thread " << i << " exiting\n";
        });
    }

    std::cout << "DEBUG: ThreadPoolScheduler::start() - all worker threads launched\n";
}

inline void ThreadPoolScheduler::stop() {
    if (!running_.exchange(false)) {
        return; // Already stopped
    }

    // Signal all queues to stop
    for (auto& queue : work_queues_) {
        queue->stop();
    }

    // Wait for all workers to finish
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

inline void ThreadPoolScheduler::submit_task(Task task) {
    if (!running_.load()) {
        std::cout << "DEBUG: submit_task() called but scheduler not running - DROPPING TASK!\n";
        return;
    }

    // Round-robin task distribution
    size_t queue_idx = next_queue_.fetch_add(1) % work_queues_.size();
    std::cout << "DEBUG: submit_task() - submitting task to queue " << queue_idx << "\n";
    work_queues_[queue_idx]->push(std::move(task));
}

inline void ThreadPoolScheduler::submit_operation(UringOp op, int fd, void* buffer, size_t size,
                                                 CompletionCallback callback, int flags) {
    // Thread-safe io_uring submission
    std::lock_guard<std::mutex> ring_lock(ring_mutex_);

    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        // Try to submit pending operations
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            throw std::runtime_error("Failed to get SQE");
        }
    }

    uint64_t user_data = generate_user_data();

    // Thread-safe pending operations management
    {
        std::lock_guard<std::mutex> pending_lock(pending_ops_mutex_);
        pending_ops_[user_data] = std::move(callback);
    }

    // Setup io_uring operation
    switch (op) {
        case UringOp::Read:
            io_uring_prep_read(sqe, fd, buffer, size, 0);
            break;
        case UringOp::Write:
            io_uring_prep_write(sqe, fd, buffer, size, 0);
            break;
        case UringOp::Accept:
            io_uring_prep_accept(sqe, fd, static_cast<sockaddr*>(buffer),
                               reinterpret_cast<socklen_t*>(&size), flags);
            break;
        case UringOp::Connect:
            io_uring_prep_connect(sqe, fd, static_cast<const sockaddr*>(buffer), size);
            break;
        case UringOp::Close:
            io_uring_prep_close(sqe, fd);
            break;
    }

    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(user_data));
    io_uring_submit(&ring_);
}

inline void ThreadPoolScheduler::worker_thread(size_t thread_id) {
    std::cout << "DEBUG: Worker thread " << thread_id << " entered worker_thread()\n";
    Task task;

    while (running_.load()) {
        bool got_work = false;

        // Try to get work (own queue first, then steal)
        if (try_get_work(thread_id, task)) {
            std::cout << "DEBUG: Worker thread " << thread_id << " executing task\n";
            task.function();
            got_work = true;
        }

        // Check for io_uring completions (any thread can process these)
        process_completions();

        // If no work found, wait a bit or wait for tasks
        if (!got_work) {
            work_queues_[thread_id]->wait_for_tasks();
        }
    }
    std::cout << "DEBUG: Worker thread " << thread_id << " exiting worker_thread()\n";
}

inline bool ThreadPoolScheduler::try_get_work(size_t thread_id, Task& task) {
    // First try own queue (LIFO for better cache locality)
    if (work_queues_[thread_id]->pop(task)) {
        return true;
    }

    // Then try to steal from other queues (random to avoid contention)
    std::uniform_int_distribution<size_t> dist(0, work_queues_.size() - 1);

    for (size_t attempts = 0; attempts < work_queues_.size(); ++attempts) {
        size_t victim = dist(gen_);
        if (victim != thread_id && work_queues_[victim]->steal(task)) {
            return true;
        }
    }

    return false;
}

inline void ThreadPoolScheduler::process_completions() {
    std::lock_guard<std::mutex> ring_lock(ring_mutex_);

    io_uring_cqe* cqe;
    while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
        uint64_t user_data = reinterpret_cast<uint64_t>(io_uring_cqe_get_data(cqe));
        int result = cqe->res;

        CompletionCallback callback;
        {
            std::lock_guard<std::mutex> pending_lock(pending_ops_mutex_);
            auto it = pending_ops_.find(user_data);
            if (it != pending_ops_.end()) {
                callback = std::move(it->second);
                pending_ops_.erase(it);
            }
        }

        if (callback.callback) {
            std::error_code ec;
            if (result < 0) {
                ec = std::error_code(-result, std::system_category());
                result = 0;
            }

            // Execute completion callback as a task in the thread pool
            submit_task(Task{[callback = std::move(callback), result, ec]() {
                callback.callback(result, ec);
            }});
        }

        io_uring_cqe_seen(&ring_, cqe);
    }
}

// High-level integration
namespace io {

template<stdx::sender Sender>
exec::task<int> as_task(Sender&& sender) {
    co_return co_await std::forward<Sender>(sender);
}

inline auto async_read(ThreadPoolScheduler& scheduler, int fd, void* buffer, size_t size) {
    return scheduler.async_read(fd, buffer, size);
}

inline auto async_write(ThreadPoolScheduler& scheduler, int fd, const void* buffer, size_t size) {
    return scheduler.async_write(fd, buffer, size);
}

inline auto async_accept(ThreadPoolScheduler& scheduler, int listen_fd,
                        sockaddr* addr = nullptr, socklen_t* addrlen = nullptr) {
    return scheduler.async_accept(listen_fd, addr, addrlen);
}

} // namespace io

} // namespace coro