#include "ThreadPool.hpp"
#include "AccountStore.hpp"
#include "RiskValidator.hpp"
#include "OrderGenerator.hpp"
#include "ResultsCollector.hpp"
#include "Stats.hpp"

#include <iostream>
#include <iomanip>
#include <set>
#include <thread>
#include <chrono>

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------
// Test 1: Correctness vs. sequential baseline (independent accounts).
// Every order touches a distinct account, so execution order across
// threads cannot change any individual outcome -- the accepted-order-id
// set from the pool run must exactly match the single-threaded baseline.
// ---------------------------------------------------------------------
bool testCorrectnessVsSequential() {
    std::cout << "\n=== Test 1: Correctness vs sequential baseline ===\n";
    const size_t N = 20000;
    const uint64_t accountStart = 1;

    OrderGenerator gen;
    std::vector<Order> orders = gen.generateIndependent(N, accountStart);

    // --- sequential baseline ---
    AccountStore seqStore;
    for (auto& o : orders) seqStore.setBalance(o.accountId, 100000.0);
    RiskValidator seqValidator(seqStore);
    std::set<uint64_t> seqAccepted;
    for (auto& o : orders) {
        o.enqueueTime = Clock::now();
        auto r = seqValidator.validate(o);
        if (r.accepted) seqAccepted.insert(r.orderId);
    }

    // --- pooled run ---
    AccountStore poolStore;
    for (auto& o : orders) poolStore.setBalance(o.accountId, 100000.0);
    RiskValidator poolValidator(poolStore);
    ResultsCollector collector;
    {
        ThreadPool pool(std::thread::hardware_concurrency());
        for (auto o : orders) {
            o.enqueueTime = Clock::now();
            pool.submit([o, &poolValidator, &collector] {
                collector.add(poolValidator.validate(o));
            });
        }
    } // pool destructor drains + joins here

    std::set<uint64_t> poolAccepted;
    for (auto& r : collector.takeAll()) {
        if (r.accepted) poolAccepted.insert(r.orderId);
    }

    bool match = (seqAccepted == poolAccepted);
    std::cout << "Sequential accepted: " << seqAccepted.size()
              << " | Pool accepted: " << poolAccepted.size()
              << " | Sets match: " << (match ? "YES" : "NO -- BUG") << "\n";
    return match;
}

// ---------------------------------------------------------------------
// Test 2: Double-spend invariant under concurrent conflicting orders.
// Each account starts with 10000, receives two concurrent BUY orders
// of 6000 each -- at most ONE may be accepted per account, ever.
// ---------------------------------------------------------------------
bool testNoDoubleSpend() {
    std::cout << "\n=== Test 2: Double-spend race invariant ===\n";
    const size_t pairCount = 5000;
    const uint64_t accountStart = 1'000'000;
    const double startingBalance = 10000.0;

    OrderGenerator gen;
    std::vector<Order> orders = gen.generateConflictingPairs(pairCount, accountStart);

    AccountStore store;
    for (size_t i = 0; i < pairCount; ++i) {
        store.setBalance(accountStart + i, startingBalance);
    }
    RiskValidator validator(store);
    ResultsCollector collector;
    {
        ThreadPool pool(std::thread::hardware_concurrency());
        for (auto o : orders) {
            o.enqueueTime = Clock::now();
            pool.submit([o, &validator, &collector] {
                collector.add(validator.validate(o));
            });
        }
    }

    // Count accepted orders per account -- must never exceed 1.
    std::unordered_map<uint64_t, int> acceptedPerAccount;
    for (auto& r : collector.takeAll()) {
        if (r.accepted) acceptedPerAccount[r.accountId]++;
    }
    bool anyDoubleSpend = false;
    for (auto& [acct, count] : acceptedPerAccount) {
        if (count > 1) {
            anyDoubleSpend = true;
            std::cout << "  DOUBLE-SPEND on account " << acct << ": " << count << " accepted\n";
        }
    }
    // Also sanity-check: no account balance ever went negative.
    bool anyNegative = false;
    for (size_t i = 0; i < pairCount; ++i) {
        if (store.getBalance(accountStart + i) < 0.0) anyNegative = true;
    }

    std::cout << "Pairs tested: " << pairCount
              << " | Double-spends found: " << (anyDoubleSpend ? "YES -- BUG" : "NONE")
              << " | Negative balances: " << (anyNegative ? "YES -- BUG" : "NONE") << "\n";
    return !anyDoubleSpend && !anyNegative;
}

// ---------------------------------------------------------------------
// Test 3: Benchmark sweep -- throughput + latency percentiles vs thread count.
// ---------------------------------------------------------------------
void runBenchmark(size_t numOrders, size_t numThreads) {
    OrderGenerator gen;
    std::vector<Order> orders = gen.generateIndependent(numOrders, 5'000'000);

    AccountStore store;
    for (auto& o : orders) store.setBalance(o.accountId, 100000.0);
    RiskValidator validator(store);

    // Per-worker-thread local buffers -- zero shared locking in the hot
    // loop. A shared mutex around a single latencies vector (the original
    // version of this benchmark) becomes the actual bottleneck at higher
    // thread counts, since every worker fights over one lock per order --
    // that measures the harness, not the engine. Sharding by thread avoids
    // this: each buffer is only ever touched by the one thread that owns it.
    std::vector<std::vector<double>> perThreadLatencies(numThreads);
    std::atomic<size_t> nextSlot{0};

    auto start = Clock::now();
    {
        ThreadPool pool(numThreads);
        for (auto o : orders) {
            o.enqueueTime = Clock::now();
            pool.submit([o, &validator, &perThreadLatencies, &nextSlot] {
                // Assign each worker thread a stable slot the first time
                // it runs a job, reused for every job after (no lock needed
                // -- each thread only ever writes to its own slot index).
                static thread_local int mySlot = -1;
                if (mySlot == -1) {
                    mySlot = static_cast<int>(nextSlot.fetch_add(1, std::memory_order_relaxed));
                }
                auto r = validator.validate(o);
                perThreadLatencies[mySlot].push_back(r.latencyMicros);
            });
        }
    }
    auto end = Clock::now();

    std::vector<double> latencies;
    latencies.reserve(numOrders);
    for (auto& v : perThreadLatencies) {
        latencies.insert(latencies.end(), v.begin(), v.end());
    }

    double wallMs = std::chrono::duration<double, std::milli>(end - start).count();
    double throughput = numOrders / (wallMs / 1000.0);
    LatencyStats stats = computeLatencyStats(latencies);

    std::cout << std::left << std::setw(10) << numThreads
              << std::setw(14) << std::fixed << std::setprecision(1) << wallMs
              << std::setw(16) << std::setprecision(0) << throughput
              << std::setw(10) << std::setprecision(1) << stats.p50
              << std::setw(10) << stats.p99
              << std::setw(10) << stats.p999
              << std::setw(10) << stats.max
              << "\n";
}

void benchmarkSweep() {
    std::cout << "\n=== Test 3: Throughput/latency vs thread count ===\n";
    const size_t numOrders = 200000;
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;

    std::cout << std::left << std::setw(10) << "Threads"
              << std::setw(14) << "WallMs"
              << std::setw(16) << "Orders/sec"
              << std::setw(10) << "p50us"
              << std::setw(10) << "p99us"
              << std::setw(10) << "p999us"
              << std::setw(10) << "MaxUs"
              << "\n";

    for (size_t threads : {size_t(1), size_t(2), size_t(4), size_t(hw)}) {
        runBenchmark(numOrders, threads);
    }
}

// ---------------------------------------------------------------------
// Test 4: Burst test -- simulate a sudden volatility spike in order rate.
// ---------------------------------------------------------------------
void burstTest() {
    std::cout << "\n=== Test 4: Burst load (volatility spike simulation) ===\n";
    OrderGenerator gen;
    AccountStore store;
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;

    ThreadPool pool(hw);
    RiskValidator validator(store);

    // Steady baseline trickle
    std::vector<Order> steady = gen.generateIndependent(2000, 9'000'000);
    for (auto& o : steady) store.setBalance(o.accountId, 100000.0);

    // Sudden burst
    std::vector<Order> burst = gen.generateIndependent(100000, 9'500'000);
    for (auto& o : burst) store.setBalance(o.accountId, 100000.0);

    // Same per-thread-slot approach as runBenchmark() -- avoids a shared
    // mutex on every single order's latency recording. Two SEPARATE slot
    // counters (steadyNextSlot / burstNextSlot) are required here, not one
    // shared counter -- a shared counter would let the same physical thread
    // claim two different slot indices (one for its first steady job, a
    // higher one for its first burst job), which could push the index past
    // the end of a `hw`-sized vector. Keeping the counters independent
    // caps each at exactly `hw`, matching the vector sizes below.
    std::vector<std::vector<double>> steadySlots(hw), burstSlots(hw);
    std::atomic<size_t> steadyNextSlot{0};
    std::atomic<size_t> burstNextSlot{0};

    for (auto o : steady) {
        o.enqueueTime = Clock::now();
        pool.submit([o, &validator, &steadySlots, &steadyNextSlot] {
            static thread_local int mySlot = -1;
            if (mySlot == -1) mySlot = static_cast<int>(steadyNextSlot.fetch_add(1, std::memory_order_relaxed));
            auto r = validator.validate(o);
            steadySlots[mySlot].push_back(r.latencyMicros);
        });
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    std::cout << "Queue depth before burst: " << pool.queueDepth() << "\n";
    auto burstStart = Clock::now();
    for (auto o : burst) {
        o.enqueueTime = Clock::now();
        pool.submit([o, &validator, &burstSlots, &burstNextSlot] {
            static thread_local int mySlot = -1;
            if (mySlot == -1) mySlot = static_cast<int>(burstNextSlot.fetch_add(1, std::memory_order_relaxed));
            auto r = validator.validate(o);
            burstSlots[mySlot].push_back(r.latencyMicros);
        });
    }
    std::cout << "Queue depth right after enqueueing burst: " << pool.queueDepth() << "\n";

    pool.shutdown(true); // wait for burst to drain
    auto burstEnd = Clock::now();
    double burstWallMs = std::chrono::duration<double, std::milli>(burstEnd - burstStart).count();

    std::vector<double> steadyLatencies, burstLatencies;
    for (auto& v : steadySlots) steadyLatencies.insert(steadyLatencies.end(), v.begin(), v.end());
    for (auto& v : burstSlots) burstLatencies.insert(burstLatencies.end(), v.begin(), v.end());

    auto steadyStats = computeLatencyStats(steadyLatencies);
    auto burstStats = computeLatencyStats(burstLatencies);

    std::cout << "Steady-state latency  -> p50: " << steadyStats.p50
              << "us | p99: " << steadyStats.p99 << "us\n";
    std::cout << "Burst latency          -> p50: " << burstStats.p50
              << "us | p99: " << burstStats.p99 << "us | max: " << burstStats.max << "us\n";
    std::cout << "Burst of " << burst.size() << " orders drained in "
              << burstWallMs << " ms\n";
}

int main() {
    bool t1 = testCorrectnessVsSequential();
    bool t2 = testNoDoubleSpend();
    benchmarkSweep();
    burstTest();

    std::cout << "\n=== Summary ===\n";
    std::cout << "Correctness vs sequential: " << (t1 ? "PASS" : "FAIL") << "\n";
    std::cout << "No double-spend invariant: " << (t2 ? "PASS" : "FAIL") << "\n";

    return (t1 && t2) ? 0 : 1;
}
