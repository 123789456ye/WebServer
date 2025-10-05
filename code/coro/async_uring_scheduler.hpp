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
#include <queue>
#include <mutex>
#include <condition_variable>

#include "stdexec/execution.hpp"
#include "exec/task.hpp"

namespace coro {

namespace stdx = stdexec;

// I/O operation types for io_uring
enum class UringOp : uint8_t {
    Read,
    Write,
    Accept,
    Connect,
    Close
};

// Forward declarations
class AsyncUringScheduler;
class MultiCoreScheduler;

// Completion callback to resume coroutines
struct CompletionCallback {
    std::function<void(int result, std::error_code ec)> callback;

    CompletionCallback() = default;
    CompletionCallback(std::function<void(int, std::error_code)> cb) : callback(std::move(cb)) {}
};

// Modern stdexec-compliant async io_uring scheduler
class AsyncUringScheduler {
public:
    explicit AsyncUringScheduler(unsigned queue_depth = 256);
    ~AsyncUringScheduler();

    AsyncUringScheduler(const AsyncUringScheduler&) = delete;
    AsyncUringScheduler& operator=(const AsyncUringScheduler&) = delete;

    // Process completion events (call from event loop)
    void process_completions();

    // Run the event loop - this actually waits for io_uring completions
    exec::task<void> run();

    // Stop the scheduler
    void stop() { running_.store(false); }

    // Submit an operation with completion callback
    void submit_operation(UringOp op, int fd, void* buffer, size_t size,
                         CompletionCallback callback, int flags = 0);

private:
    // Modern sender for schedule operations
    struct ScheduleSender {
        AsyncUringScheduler* scheduler_;

        using sender_concept = stdx::sender_t;
        using completion_signatures = stdx::completion_signatures<stdx::set_value_t()>;

        template<typename Receiver>
        auto connect(Receiver&& receiver) const noexcept {
            struct Operation {
                AsyncUringScheduler* scheduler_;
                [[no_unique_address]] std::decay_t<Receiver> receiver_;

                using operation_state_concept = stdx::operation_state_t;

                void start() noexcept {
                    // Schedule operations complete immediately - they just switch execution context
                    stdx::set_value(std::move(receiver_));
                }
            };

            return Operation{scheduler_, std::forward<Receiver>(receiver)};
        }

        auto get_env() const noexcept {
            struct Env {
                AsyncUringScheduler* scheduler_;

                auto query(stdx::get_completion_scheduler_t<stdx::set_value_t>) const noexcept -> AsyncUringScheduler& {
                    return *scheduler_;
                }
            };
            return Env{scheduler_};
        }
    };

    // PROPER Awaitable sender for I/O operations - this one actually suspends!
    template<UringOp Op>
    struct IoSender {
        AsyncUringScheduler* scheduler_;
        int fd_;
        void* buffer_;
        size_t size_;
        int flags_;

        using sender_concept = stdx::sender_t;
        using completion_signatures = stdx::completion_signatures<
            stdx::set_value_t(int),    // Success with result
            stdx::set_error_t(std::exception_ptr)  // Error
        >;

        template<typename Receiver>
        auto connect(Receiver&& receiver) const noexcept {
            struct Operation {
                AsyncUringScheduler* scheduler_;
                int fd_;
                void* buffer_;
                size_t size_;
                int flags_;
                [[no_unique_address]] std::decay_t<Receiver> receiver_;

                using operation_state_concept = stdx::operation_state_t;

                void start() noexcept {
                    try {
                        // THIS IS THE KEY FIX: Don't complete immediately!
                        // Instead, submit to io_uring and the completion will happen later

                        CompletionCallback callback{[receiver = std::move(receiver_)](int result, std::error_code ec) mutable {
                            if (ec) {
                                auto ex = std::make_exception_ptr(std::system_error(ec));
                                stdx::set_error(std::move(receiver), ex);
                            } else {
                                stdx::set_value(std::move(receiver), result);
                            }
                        }};

                        scheduler_->submit_operation(Op, fd_, buffer_, size_, std::move(callback), flags_);

                        // Coroutine is now suspended until io_uring completion occurs!

                    } catch (...) {
                        stdx::set_error(std::move(receiver_), std::current_exception());
                    }
                }
            };

            return Operation{scheduler_, fd_, buffer_, size_, flags_, std::forward<Receiver>(receiver)};
        }

        auto get_env() const noexcept {
            struct Env {
                AsyncUringScheduler* scheduler_;
                auto query(stdx::get_completion_scheduler_t<stdx::set_value_t>) const noexcept -> AsyncUringScheduler& {
                    return *scheduler_;
                }
            };
            return Env{scheduler_};
        }
    };

public:
    // Modern scheduler concept compliance
    using scheduler_concept = stdx::scheduler_t;

    auto schedule() const noexcept -> ScheduleSender {
        return {const_cast<AsyncUringScheduler*>(this)};
    }

    bool operator==(const AsyncUringScheduler& other) const noexcept {
        return this == &other;
    }

    // Factory methods for I/O senders
    auto async_read(int fd, void* buffer, size_t size) const noexcept {
        return IoSender<UringOp::Read>{const_cast<AsyncUringScheduler*>(this), fd, buffer, size, 0};
    }

    auto async_write(int fd, const void* buffer, size_t size) const noexcept {
        return IoSender<UringOp::Write>{const_cast<AsyncUringScheduler*>(this), fd, const_cast<void*>(buffer), size, 0};
    }

    auto async_accept(int listen_fd, sockaddr* addr = nullptr, socklen_t* addrlen = nullptr) const noexcept {
        return IoSender<UringOp::Accept>{const_cast<AsyncUringScheduler*>(this), listen_fd, addr, addrlen ? *addrlen : 0, 0};
    }

    auto async_connect(int fd, const sockaddr* addr, socklen_t addrlen) const noexcept {
        return IoSender<UringOp::Connect>{const_cast<AsyncUringScheduler*>(this), fd, const_cast<sockaddr*>(addr), addrlen, 0};
    }

    auto async_close(int fd) const noexcept {
        return IoSender<UringOp::Close>{const_cast<AsyncUringScheduler*>(this), fd, nullptr, 0, 0};
    }

private:
    struct io_uring ring_;
    std::unordered_map<uint64_t, CompletionCallback> pending_ops_;
    uint64_t next_user_data_ = 1;
    std::atomic<bool> running_{false};

    uint64_t generate_user_data() { return next_user_data_++; }
};

// Multi-core scheduler that manages multiple AsyncUringScheduler instances
class MultiCoreScheduler {
public:
    explicit MultiCoreScheduler(unsigned num_threads = std::thread::hardware_concurrency());
    ~MultiCoreScheduler();

    // Get scheduler for current thread (round-robin)
    AsyncUringScheduler& get_scheduler();

    // Start all schedulers
    void start();

    // Stop all schedulers
    void stop();

    // Factory methods that delegate to thread-local schedulers
    auto async_read(int fd, void* buffer, size_t size) {
        return get_scheduler().async_read(fd, buffer, size);
    }

    auto async_write(int fd, const void* buffer, size_t size) {
        return get_scheduler().async_write(fd, buffer, size);
    }

    auto async_accept(int listen_fd, sockaddr* addr = nullptr, socklen_t* addrlen = nullptr) {
        return get_scheduler().async_accept(listen_fd, addr, addrlen);
    }

private:
    std::vector<std::unique_ptr<AsyncUringScheduler>> schedulers_;
    std::vector<std::jthread> threads_;
    std::atomic<size_t> next_scheduler_{0};
    std::atomic<bool> running_{false};
};

// Implementation
inline AsyncUringScheduler::AsyncUringScheduler(unsigned queue_depth) {
    int ret = io_uring_queue_init(queue_depth, &ring_, 0);
    if (ret < 0) {
        throw std::system_error(-ret, std::system_category(), "io_uring_queue_init");
    }
}

inline AsyncUringScheduler::~AsyncUringScheduler() {
    stop();
    io_uring_queue_exit(&ring_);
}

inline void AsyncUringScheduler::submit_operation(UringOp op, int fd, void* buffer, size_t size,
                                                 CompletionCallback callback, int flags) {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        // Ring is full, try to submit pending operations first
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            throw std::runtime_error("Failed to get SQE even after submit");
        }
    }

    uint64_t user_data = generate_user_data();
    pending_ops_[user_data] = std::move(callback);

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

    // Submit the operation
    io_uring_submit(&ring_);
}

inline void AsyncUringScheduler::process_completions() {
    io_uring_cqe* cqe;

    // Process all available completions
    while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
        uint64_t user_data = reinterpret_cast<uint64_t>(io_uring_cqe_get_data(cqe));
        int result = cqe->res;

        auto it = pending_ops_.find(user_data);
        if (it != pending_ops_.end()) {
            auto callback = std::move(it->second);
            pending_ops_.erase(it);

            // Convert result to error_code if needed
            std::error_code ec;
            if (result < 0) {
                ec = std::error_code(-result, std::system_category());
                result = 0; // Don't pass negative result
            }

            // Execute completion callback - this resumes the coroutine!
            if (callback.callback) {
                callback.callback(result, ec);
            }
        }

        io_uring_cqe_seen(&ring_, cqe);
    }
}

inline exec::task<void> AsyncUringScheduler::run() {
    running_.store(true);

    while (running_.load()) {
        // REAL async behavior: actually wait for completions!
        io_uring_cqe* cqe;

        // This blocks until at least one completion is available
        int ret = io_uring_wait_cqe(&ring_, &cqe);
        if (ret < 0) {
            if (ret == -EAGAIN || ret == -EINTR) {
                continue; // Spurious wakeup, try again
            }
            throw std::system_error(-ret, std::system_category(), "io_uring_wait_cqe");
        }

        // Process this completion and any others that are ready
        process_completions();

        // Yield control back to the coroutine framework
        co_await stdx::just();
    }

    co_return;
}

// Multi-core implementation
inline MultiCoreScheduler::MultiCoreScheduler(unsigned num_threads) {
    for (unsigned i = 0; i < num_threads; ++i) {
        schedulers_.emplace_back(std::make_unique<AsyncUringScheduler>());
    }
}

inline MultiCoreScheduler::~MultiCoreScheduler() {
    stop();
}

inline AsyncUringScheduler& MultiCoreScheduler::get_scheduler() {
    // Simple round-robin distribution
    size_t idx = next_scheduler_.fetch_add(1) % schedulers_.size();
    return *schedulers_[idx];
}

inline void MultiCoreScheduler::start() {
    if (running_.exchange(true)) {
        return; // Already running
    }

    // Start each scheduler in its own thread
    for (auto& scheduler : schedulers_) {
        threads_.emplace_back([&scheduler]() {
            stdx::sync_wait(scheduler->run());
        });
    }
}

inline void MultiCoreScheduler::stop() {
    if (!running_.exchange(false)) {
        return; // Already stopped
    }

    // Stop all schedulers
    for (auto& scheduler : schedulers_) {
        scheduler->stop();
    }

    // Wait for all threads to finish
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
}

// High-level stdexec integration
namespace io {

// Convert io_uring senders to coroutine tasks
template<stdx::sender Sender>
exec::task<int> as_task(Sender&& sender) {
    // This works because exec::task can await stdexec senders
    co_return co_await std::forward<Sender>(sender);
}

// Convenience functions for multi-core scheduler
inline auto async_read(MultiCoreScheduler& mc_scheduler, int fd, void* buffer, size_t size) {
    return mc_scheduler.async_read(fd, buffer, size);
}

inline auto async_write(MultiCoreScheduler& mc_scheduler, int fd, const void* buffer, size_t size) {
    return mc_scheduler.async_write(fd, buffer, size);
}

inline auto async_accept(MultiCoreScheduler& mc_scheduler, int listen_fd,
                        sockaddr* addr = nullptr, socklen_t* addrlen = nullptr) {
    return mc_scheduler.async_accept(listen_fd, addr, addrlen);
}

} // namespace io

} // namespace coro