# HFT-Style Risk Validation Engine (Tier 1)

A payload-agnostic parallel task execution engine (producer-consumer thread
pool) with a cryptocurrency order risk-validation payload plugged in.

This is the **Tier 1** scope agreed in planning: mutex/condvar-based queue,
in-memory synthetic order generator standing in for a real FIX/socket feed,
a simplified fixed-format order message instead of real FIX tag=value
parsing, and a stub balance-check "matching engine." Every one of these can
be swapped for the real thing later without touching the engine core — see
"Upgrade path" below.

## Architecture

```
OrderGenerator --submit()--> ThreadPool --> TaskQueue --> N worker threads
                                                              |
                                                              v
                                                       RiskValidator
                                                              |
                                                              v
                                                   AccountStore (sharded,
                                                   atomic check-and-debit)
```

- `TaskQueue.hpp` — bounded, blocking, thread-safe queue (mutex + 2 condvars).
- `ThreadPool.hpp` — fixed worker pool, RAII lifecycle, exception-safe workers.
- `AccountStore.hpp` — sharded balance cache; `tryDebit()` is the single
  atomic check-and-debit operation that prevents the double-spend race.
- `RiskValidator.hpp` — the payload: validates one order (symbol whitelist,
  qty/price sanity, balance check for BUY orders).
- `OrderGenerator.hpp` — synthetic order producer (independent-account mode
  for correctness testing, conflicting-pair mode for race testing).
- `Stats.hpp` — latency percentile calculation for benchmarking.
- `src/main.cpp` — driver: correctness test, race-invariant test, benchmark
  sweep, burst test.

## Build & run

Requires CMake ≥ 3.16 and a C++17 compiler.

```bash
# normal release build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
./hft_engine
```

### Sanitizer builds (do these too — this is the point of the exercise)

```bash
# ThreadSanitizer -- verifies no data races
mkdir build_tsan && cd build_tsan
cmake .. -DSANITIZER=thread -DCMAKE_BUILD_TYPE=Debug
make
./hft_engine

# AddressSanitizer + UndefinedBehaviorSanitizer -- verifies no leaks/UB
mkdir build_asan && cd build_asan
cmake .. -DSANITIZER=address -DCMAKE_BUILD_TYPE=Debug
make
./hft_engine
```

Both sanitizer builds were run during development and are clean (no
`WARNING: ThreadSanitizer: data race` / no ASan leak or error reports).

## What `main.cpp` actually tests

1. **Correctness vs. sequential baseline** — 20,000 orders on independent
   accounts run through both a single-threaded reference validator and the
   thread pool. The set of accepted order IDs must match exactly, since
   independent accounts can't be affected by execution order.

2. **Double-spend race invariant** — 5,000 accounts each get two concurrent
   conflicting BUY orders (each affordable alone, not together). Asserts
   that **at most one** of each pair is ever accepted, and no balance ever
   goes negative. This is the actual race condition the risk-check layer
   exists to prevent (TOCTOU on balance check-then-debit) — the test would
   fail if `AccountStore::tryDebit` didn't do the check-and-subtract as one
   atomic critical section.

3. **Benchmark sweep** — same 200,000-order workload run at 1, 2, 4, and
   `hardware_concurrency()` threads, reporting wall time, throughput, and
   p50/p99/p99.9/max latency.

4. **Burst test** — a steady trickle of orders followed by a sudden spike of
   100,000 orders, measuring queue depth and latency degradation under the
   spike, and how long it takes to drain.

## ⚠️ Honest note on the benchmark numbers in this sandbox

This code was built and run in a **single-core (`nproc` = 1) sandbox**, so
the benchmark sweep here shows 2/4-thread runs performing *worse* than
1-thread — that's real, correct behavior for that hardware: with no actual
parallelism available, extra threads only add context-switch and lock
contention overhead. **Run this on your own multi-core machine** and you
should see throughput scale up through 4 threads before plateauing
(Amdahl's Law) — that plateau, and where it happens, is what you want to
capture and explain in an interview. Re-run `benchmarkSweep()` and paste
your real multi-core numbers into a `benchmarks/results.md` before you
submit this anywhere.

## ⚠️ Known scaling limitation of the mutex-based queue (found via benchmarking, not hidden)

Benchmarking on real multi-core hardware surfaced a genuine finding, not a
bug: **throughput can get *worse* as thread count increases**, because
`TaskQueue::push()` and `TaskQueue::pop()` share one `std::mutex`. Each
validated order does only a few hundred nanoseconds of real work (a hash
lookup, a couple of comparisons) — at that granularity, the cost of every
thread fighting over one shared lock can exceed the cost of the work
itself, so adding more worker threads adds more lock contention without
adding proportional useful throughput.

This is a well-known failure mode of naive mutex+condvar queues under
fine-grained tasks, and it's the direct motivation for Phase 11 in the
Tier 2 plan (lock-free / sharded queues) — this project's benchmark is
what makes that motivation concrete rather than assumed. Worth stating
plainly in an interview: *"I measured this, found the queue's shared lock
doesn't scale past a few threads for tasks this cheap, and that's what a
lock-free upgrade would fix."*

A separate, now-fixed issue: the `ThreadPool`'s internal queue was
originally capped at 100,000 slots while the benchmark submits 200,000
orders per sweep — with a slow single-threaded consumer, the queue filled
and stayed full, so most of the "latency" measured was actually queueing
delay from an undersized buffer, not processing time. The queue capacity
now defaults to 1,000,000, which removes that artifact so Test 3's latency
numbers reflect real scheduling + processing time instead.

## Upgrade path (Tier 1 → Tier 2)

| Component | Tier 1 (this repo) | Tier 2 swap-in |
|---|---|---|
| Producer | `OrderGenerator` (in-memory) | Real TCP socket listener |
| Message format | `Order` struct | Real FIX tag=value parser → `Order` |
| Queue | `TaskQueue` (mutex+condvar) | Lock-free MPMC (atomics/CAS) — ✅ done in Tier 3 |
| Matching engine | none (validator only) | Real LOB, price-time priority |

None of these require changing `ThreadPool.hpp` or `RiskValidator.hpp`'s
interface — that's the payload-agnostic guarantee the engine was built for.

## Known simplifications (say these out loud in an interview, don't hide them)

- SELL orders skip inventory/position checks (Tier 1 only checks BUY balance).
- Symbol whitelist is a hardcoded 3-symbol set, not real reference data.
- No persistence — account state is in-memory only, lost on restart.
- ~~Queue is mutex-based, not lock-free~~ — **done in Tier 3**: the default build
  uses a lock-free ring (`include/LockFreeTaskQueue.hpp`); the original `TaskQueue`
  is kept as an A/B baseline (`hft_engine_mutex`). See `HANDBOOK.md` §19.
