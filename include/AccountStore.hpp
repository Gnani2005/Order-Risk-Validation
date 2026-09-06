#pragma once
#include <unordered_map>
#include <mutex>
#include <array>
#include <cstdint>

// Thread-safe balance cache. The critical operation is tryDebit(): it must
// check "does this account have enough balance" and "subtract the cost"
// as a single atomic step. Doing the check and the debit as two separate
// locked operations reintroduces the exact TOCTOU race this system exists
// to prevent (two concurrent orders on the same account both pass the
// check before either debits, over-spending the account).
//
// Sharded locking: accounts are bucketed across N mutexes by accountId hash
// so unrelated accounts don't contend on the same lock, while operations
// on the *same* account are always fully serialized (correctness first,
// then reduce contention).
class AccountStore {
public:
    static constexpr size_t kShardCount = 64;

    explicit AccountStore(size_t reserveHint = 1024) {
        for (auto& shard : shards_) {
            shard.balances.reserve(reserveHint / kShardCount + 1);
        }
    }

    void setBalance(uint64_t accountId, double balance) {
        Shard& s = shardFor(accountId);
        std::lock_guard<std::mutex> lock(s.mtx);
        s.balances[accountId] = balance;
    }

    double getBalance(uint64_t accountId) const {
        const Shard& s = shardFor(accountId);
        std::lock_guard<std::mutex> lock(s.mtx);
        auto it = s.balances.find(accountId);
        return it == s.balances.end() ? 0.0 : it->second;
    }

    // Atomic check-and-debit. Returns true and applies the debit iff
    // sufficient balance existed at the moment of the call.
    bool tryDebit(uint64_t accountId, double amount) {
        Shard& s = shardFor(accountId);
        std::lock_guard<std::mutex> lock(s.mtx);
        auto it = s.balances.find(accountId);
        if (it == s.balances.end() || it->second < amount) {
            return false;
        }
        it->second -= amount;
        return true;
    }

    void credit(uint64_t accountId, double amount) {
        Shard& s = shardFor(accountId);
        std::lock_guard<std::mutex> lock(s.mtx);
        s.balances[accountId] += amount;
    }

private:
    struct Shard {
        mutable std::mutex mtx;
        std::unordered_map<uint64_t, double> balances;
    };

    Shard& shardFor(uint64_t accountId) {
        return shards_[accountId % kShardCount];
    }
    const Shard& shardFor(uint64_t accountId) const {
        return shards_[accountId % kShardCount];
    }

    std::array<Shard, kShardCount> shards_;
};
