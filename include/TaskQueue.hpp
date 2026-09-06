#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>

// Thread-safe bounded blocking queue.
// push() blocks while full; pop() blocks while empty.
// shutdown() wakes every blocked thread; pop() then returns std::nullopt
// instead of blocking forever, and push() stops accepting new work.
template <typename T>
class TaskQueue {
public:
    explicit TaskQueue(size_t capacity = 10000) : capacity_(capacity) {}

    // Returns false if the queue has been shut down (item not accepted).
    bool push(T item) {
        std::unique_lock<std::mutex> lock(mtx_);
        notFull_.wait(lock, [this] { return queue_.size() < capacity_ || shutdown_; });
        if (shutdown_) return false;
        queue_.push(std::move(item));
        lock.unlock();
        notEmpty_.notify_one();
        return true;
    }

    // Returns std::nullopt once shutdown AND the queue is drained.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mtx_);
        notEmpty_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
        if (queue_.empty()) {
            // shutdown_ must be true here, nothing left to hand out
            return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        notFull_.notify_one();
        return item;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            shutdown_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::queue<T> queue_;
    size_t capacity_;
    bool shutdown_ = false;
};
