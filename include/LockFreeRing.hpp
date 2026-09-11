#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

// Bounded lock-free ring buffer (Dmitry Vyukov's bounded MPMC design).
// Non-blocking: try_push/try_pop return false instead of waiting.
// Safe for any number of producers and consumers; this project uses it with
// one producer and N consumers (SPMC).
//
// How it works: every cell carries a sequence number saying whose turn it is.
// For the lap that reaches the cell at position `pos`:
//   seq == pos       -> cell is free, a producer may fill it
//   seq == pos + 1   -> cell is full, a consumer may empty it
// A thread claims a position with a CAS on tail_/head_, then owns that cell
// alone until it publishes the next seq value with a release store.
template <typename T>
class LockFreeRing {
public:
    explicit LockFreeRing(size_t minCapacity) {
        size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;   // power of two -> `& mask_` instead of `%`
        mask_ = cap - 1;
        cells_.reset(new Cell[cap]);
        for (size_t i = 0; i < cap; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
    }

    // Moves `item` in and returns true, or returns false if full
    // (item is left untouched, so the caller can retry).
    bool try_push(T& item) {
        size_t pos = tail_.load(std::memory_order_relaxed);
        while (true) {
            Cell& cell = cells_[pos & mask_];
            size_t seq = cell.seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {                                  // free: try to claim it
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    cell.data = std::move(item);
                    cell.seq.store(pos + 1, std::memory_order_release);   // publish to consumers
                    return true;
                }                                             // lost the race; CAS reloaded pos
            } else if (diff < 0) {
                return false;                                 // full: this cell not freed yet
            } else {
                pos = tail_.load(std::memory_order_relaxed);  // someone else moved ahead
            }
        }
    }

    // Moves the oldest item into `out` and returns true, or returns false if empty.
    bool try_pop(T& out) {
        size_t pos = head_.load(std::memory_order_relaxed);
        while (true) {
            Cell& cell = cells_[pos & mask_];
            size_t seq = cell.seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {                                  // full: try to claim it
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = std::move(cell.data);
                    // Drop whatever the job captured (e.g. EventLoop's
                    // shared_ptr<Connection>) now, not when this cell is next
                    // overwritten -- otherwise sockets would stay open.
                    cell.data = T{};
                    cell.seq.store(pos + mask_ + 1, std::memory_order_release);  // free for next lap
                    return true;
                }
            } else if (diff < 0) {
                return false;                                 // empty
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    // Snapshot only -- may be stale by the time it returns.
    size_t approx_size() const {
        size_t t = tail_.load(std::memory_order_relaxed);
        size_t h = head_.load(std::memory_order_relaxed);
        return t > h ? t - h : 0;
    }

private:
    struct Cell {
        std::atomic<size_t> seq;
        T data;
    };

    std::unique_ptr<Cell[]> cells_;
    size_t mask_ = 0;
    // Producers write tail_, consumers write head_. Separate cache lines so
    // they don't keep invalidating each other (false sharing).
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::atomic<size_t> head_{0};
};
