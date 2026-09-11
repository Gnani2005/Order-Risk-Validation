#pragma once
// Which job queue the pool uses. Default: the lock-free ring.
// Build with -DHFT_MUTEX_QUEUE to get the original mutex+condvar TaskQueue
// (the hft_engine_mutex target does this, for A/B benchmarking).
#ifdef HFT_MUTEX_QUEUE
#include "TaskQueue.hpp"
template <typename T> using JobQueue = TaskQueue<T>;
#else
#include "LockFreeTaskQueue.hpp"
template <typename T> using JobQueue = LockFreeTaskQueue<T>;
#endif
#include <thread>
#include <vector>
#include "InlineJob.hpp"
#include <atomic>
#include <iostream>
#include <future>

// Payload-agnostic thread pool. Jobs are type-erased InlineJob callables
// (like std::function<void()>, but stored inline -- no heap allocation).
// submit() never blocks waiting for execution -- it only blocks if the
// internal queue is momentarily full (backpressure), never on job completion.
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads = std::thread::hardware_concurrency(),
                        size_t queueCapacity = 1 << 16)
        : queue_(queueCapacity) {
        if (numThreads == 0) numThreads = 1;
        workers_.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    // No copies, no moves -- the pool owns live OS threads.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ~ThreadPool() {
        shutdown(/*drainRemaining=*/true);
    }

    template <typename F>
    bool submit(F&& job) {
        return queue_.push(InlineJob(std::forward<F>(job)));
    }

    // Submit and get a future for the result -- handy for tests/benchmarks
    // that need to know when a specific job finished.
    template <typename F>
    auto submitWithFuture(F&& job) -> std::future<decltype(job())> {
        using ReturnT = decltype(job());
        auto taskPtr = std::make_shared<std::packaged_task<ReturnT()>>(std::forward<F>(job));
        std::future<ReturnT> fut = taskPtr->get_future();
        queue_.push([taskPtr] { (*taskPtr)(); });
        return fut;
    }

    void shutdown(bool drainRemaining) {
        bool expected = false;
        if (!shuttingDown_.compare_exchange_strong(expected, true)) {
            return; // already shut down -- idempotent
        }
        if (!drainRemaining) {
            // best-effort: nothing dequeued after this point matters much
            // since workers will still finish whatever they're mid-execution on
        }
        queue_.shutdown();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    size_t queueDepth() const { return queue_.size(); }
    size_t workerCount() const { return workers_.size(); }
    size_t completedCount() const { return completed_.load(std::memory_order_relaxed); }

private:
    void workerLoop() {
        while (true) {
            std::optional<InlineJob> job = queue_.pop();
            if (!job.has_value()) break; // shutdown + drained
            try {
                (*job)();
            } catch (const std::exception& e) {
                std::cerr << "[ThreadPool] job threw exception: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[ThreadPool] job threw unknown exception\n";
            }
            completed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    JobQueue<InlineJob> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> shuttingDown_{false};
    std::atomic<size_t> completed_{0};
};
