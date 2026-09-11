#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// Thread-safe balance store with NO locks.
//
// The critical operation is tryDebit(): "is there enough balance?" and
// "subtract the cost" must happen as ONE atomic step. Otherwise two
// concurrent orders on the same account can both pass the check before
// either debits -- the TOCTOU double-spend this system exists to prevent.
//
// Earlier version: 64 sharded mutexes. This version: every balance is its
// own std::atomic<double>, and tryDebit() is a compare-and-swap loop.
//
// RULE: register every account (setBalance) BEFORE trading starts. After
// that the map's shape never changes, so find() is safe from any thread
// without a lock -- only the balances change, and those are atomic.
// All current callers do this: the tests seed accounts before submitting
// orders, and server_main seeds its accounts before starting the pool.
class AccountStore {
public:
    static_assert(std::atomic<double>::is_always_lock_free,
                  "atomic<double> must be lock-free on this platform");

    explicit AccountStore(size_t reserveHint = 1024) { balances_.reserve(reserveHint); }

    // Adding a NEW account is setup-phase only (see RULE above).
    void setBalance(uint64_t accountId, double balance) {
        auto [it, inserted] = balances_.try_emplace(accountId, balance);
        if (!inserted) it->second.store(balance);
    }

    double getBalance(uint64_t accountId) const {
        auto it = balances_.find(accountId);
        return it == balances_.end() ? 0.0 : it->second.load();
    }

    // Atomic check-and-debit. Returns true and applies the debit iff the
    // balance was sufficient at the moment the CAS succeeded.
    bool tryDebit(uint64_t accountId, double amount) {
        auto it = balances_.find(accountId);
        if (it == balances_.end()) return false;
        std::atomic<double>& bal = it->second;
        double cur = bal.load();
        do {
            if (cur < amount) return false;                      // check...
        } while (!bal.compare_exchange_weak(cur, cur - amount)); // ...and debit, as one step
        // If another thread changed the balance between our load and the
        // CAS, the CAS fails, `cur` is refreshed to the new value, and we
        // check again. ABA is harmless: we compare the value itself, not a
        // pointer, so "same value" really does mean "still enough money".
        return true;
    }

    // Adding a NEW account is setup-phase only; atomic for existing ones.
    void credit(uint64_t accountId, double amount) {
        auto it = balances_.find(accountId);
        if (it == balances_.end()) { balances_.try_emplace(accountId, amount); return; }
        double cur = it->second.load();
        while (!it->second.compare_exchange_weak(cur, cur + amount)) {}
    }

private:
    std::unordered_map<uint64_t, std::atomic<double>> balances_;
};
