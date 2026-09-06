#pragma once
#include "Order.hpp"
#include "AccountStore.hpp"
#include <unordered_set>
#include <chrono>

// Payload logic: validates one order against cached account state.
// This is intentionally the ONLY class that knows anything about trading --
// ThreadPool/TaskQueue above have no idea what an "order" is. Swap this
// class out and the engine underneath is unaffected (the payload-agnostic
// guarantee from Phase 6 of the engine plan).
class RiskValidator {
public:
    explicit RiskValidator(AccountStore& store) : store_(store) {
        // Tier 1: small fixed symbol whitelist standing in for a real
        // instrument/reference-data lookup.
        allowedSymbols_ = {"BTC-USD", "ETH-USD", "SOL-USD"};
    }

    ValidationResult validate(const Order& order) const {
        using namespace std::chrono;
        ValidationResult result;
        result.orderId = order.orderId;
        result.accountId = order.accountId;

        if (order.qty == 0) {
            result.accepted = false;
            result.reason = "zero_quantity";
        } else if (order.price <= 0.0) {
            result.accepted = false;
            result.reason = "invalid_price";
        } else if (allowedSymbols_.find(order.symbol) == allowedSymbols_.end()) {
            result.accepted = false;
            result.reason = "unknown_symbol";
        } else if (order.side == Side::BUY) {
            double cost = static_cast<double>(order.qty) * order.price;
            // Single atomic check-and-debit call -- see AccountStore comment.
            if (store_.tryDebit(order.accountId, cost)) {
                result.accepted = true;
            } else {
                result.accepted = false;
                result.reason = "insufficient_balance";
            }
        } else { // SELL -- Tier 1 simplification: no position/inventory check,
                 // just accept (a real system would validate held quantity here).
            result.accepted = true;
        }

        auto now = steady_clock::now();
        result.latencyMicros =
            duration_cast<duration<double, std::micro>>(now - order.enqueueTime).count();
        return result;
    }

private:
    AccountStore& store_;
    std::unordered_set<std::string> allowedSymbols_;
};
