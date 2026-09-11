#pragma once
#include "LockFreeRing.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

// Drop-in replacement for TaskQueue: same five members, same behaviour
// (push blocks while full, pop blocks while empty, shutdown drains).
//
// Jobs travel through a lock-free ring. The mutex + condvar below are used
// ONLY to put an idle worker to sleep -- never on the normal push/pop path.
// A worker spins briefly before sleeping, and the producer only pays for a
// wake-up (a syscall) when a worker is asleep AND none is spinning. At most
// half the cores spin at once, so the producer always has a core to run on.
template <typename T>
class LockFreeTaskQueue {
public:
    explicit LockFreeTaskQueue(size_t capacity = 10000) : ring_(capacity) {}

    // Returns false if the queue has been shut down (item not accepted).
    bool push(T item) {
        pushersInFlight_.fetch_add(1);
        if (shutdown_.load()) { pushersInFlight_.fetch_sub(1); return false; }
        while (!ring_.try_push(item)) {
            if (shutdown_.load()) { pushersInFlight_.fetch_sub(1); return false; }
            std::this_thread::yield();   // full: backpressure, let workers catch up
        }
        pushersInFlight_.fetch_sub(1);

        // Wake-up handshake, producer half (pop() step 2 is the other half).
        // fetch_add(0) instead of a plain load: read-modify-writes on the
        // SAME variable are totally ordered, so either we see the sleeper's
        // +1 (and wake it), or our RMW comes first -- and then the sleeper's
        // +1 synchronizes with it, so its re-check is guaranteed to see the
        // item we just published.
        // Skip the wake-up if a worker is still spinning: it will see the
        // item. (A spinner decrements spinners_ BEFORE its sleepers_ +1, so
        // any spinner we count here re-checks after our RMW and must see it.)
        if (sleepers_.fetch_add(0) > 0 && spinners_.load() == 0) {
            // Taking the lock means a sleeper is either still before its
            // predicate check (it will see the item) or fully waiting (it
            // will get this notify) -- never in between.
            { std::lock_guard<std::mutex> lock(parkMtx_); }
            parkCv_.notify_one();
        }
        return true;
    }

    // Returns std::nullopt once shutdown AND drained.
    std::optional<T> pop() {
        T item;
        while (true) {
            // 1. Fast path: spin briefly -- only if a spin slot is free. With
            //    every worker spinning, the producer starved (measured: it got
            //    0.7% of the CPU with 16 workers on 16 hardware threads).
            if (spinners_.fetch_add(1) < maxSpinners_) {
                for (int i = 0; i < kSpins; ++i) {
                    if (ring_.try_pop(item)) { spinners_.fetch_sub(1); return item; }
                    cpuRelax();
                }
            }
            spinners_.fetch_sub(1);   // must come BEFORE sleepers_ +1 (see push)

            // 2. About to sleep: announce it FIRST, then check once more.
            //    Paired with push()'s fetch_add(0) this guarantees that either
            //    the producer sees sleepers_ > 0 and wakes us, or we see its
            //    item right here. Without it, a push landing between our last
            //    check and our wait would be missed -> worker sleeps forever.
            sleepers_.fetch_add(1);
            if (ring_.try_pop(item)) { sleepers_.fetch_sub(1); return item; }

            // 3. Exit only when shut down, no push is half-done, and the
            //    ring is empty -- queued work is drained, never dropped.
            if (shutdown_.load() && pushersInFlight_.load() == 0) {
                sleepers_.fetch_sub(1);
                if (ring_.try_pop(item)) return item;
                return std::nullopt;
            }

            // 4. Sleep until there is work or we are shutting down.
            {
                std::unique_lock<std::mutex> lock(parkMtx_);
                parkCv_.wait(lock, [this] { return ring_.approx_size() > 0 || shutdown_.load(); });
            }
            sleepers_.fetch_sub(1);
        }
    }

    void shutdown() {
        shutdown_.store(true);
        { std::lock_guard<std::mutex> lock(parkMtx_); }
        parkCv_.notify_all();
    }

    size_t size() const { return ring_.approx_size(); }

private:
    // Tries before a worker goes to sleep. Measured: 256 was too short
    // (workers parked on almost every order); 4096 with cpuRelax() gave the
    // best 4-thread result.
    static constexpr int kSpins = 4096;

    // Tells the CPU "this is a spin-wait": saves power and frees pipeline
    // resources for the sibling hyperthread. Measured on the 4-thread
    // benchmark: 4096 spins WITH this ran ~7x faster than without it.
    static void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }

    LockFreeRing<T> ring_;
    std::atomic<bool> shutdown_{false};
    std::atomic<int> sleepers_{0};         // workers asleep (or about to be)
    std::atomic<int> spinners_{0};         // workers in the spin phase
    const int maxSpinners_ = std::max(1, static_cast<int>(std::thread::hardware_concurrency() / 2));
    std::atomic<int> pushersInFlight_{0};  // pushes started but not finished
    std::mutex parkMtx_;                   // sleep/wake only
    std::condition_variable parkCv_;
};
