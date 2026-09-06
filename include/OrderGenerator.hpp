#pragma once
#include "Order.hpp"
#include <vector>
#include <random>
#include <string>

// Fabricates orders in-memory instead of listening on a real socket.
// This is the Tier 1 stand-in noted in the build plan -- swapping this
// for a real TCP listener + FIX parser later does not require touching
// ThreadPool, TaskQueue, AccountStore, or RiskValidator.
class OrderGenerator {
public:
    OrderGenerator(uint64_t seed = 42) : rng_(seed) {}

    // Independent accounts, mix of valid/invalid orders, for the
    // correctness-vs-sequential-baseline test.
    std::vector<Order> generateIndependent(size_t count, uint64_t accountIdStart) {
        std::vector<Order> orders;
        orders.reserve(count);
        std::uniform_int_distribution<int> symbolPick(0, 3); // 3 valid + 1 invalid
        std::uniform_int_distribution<int> sidePick(0, 1);
        std::uniform_real_distribution<double> priceDist(1.0, 50000.0);
        std::uniform_int_distribution<uint32_t> qtyDist(1, 10);
        const char* symbols[] = {"BTC-USD", "ETH-USD", "SOL-USD", "DOGE-XYZ"}; // last = invalid

        for (size_t i = 0; i < count; ++i) {
            Order o;
            o.orderId = nextOrderId_++;
            o.accountId = accountIdStart + i; // each order = fresh independent account
            o.symbol = symbols[symbolPick(rng_)];
            o.side = sidePick(rng_) == 0 ? Side::BUY : Side::SELL;
            o.qty = qtyDist(rng_);
            o.price = priceDist(rng_);
            orders.push_back(o);
        }
        return orders;
    }

    // Many conflicting BUY pairs on shared accounts, sized so a single
    // order is affordable but two together are not -- this is the
    // deliberate double-spend stress case.
    std::vector<Order> generateConflictingPairs(size_t pairCount, uint64_t accountIdStart) {
        std::vector<Order> orders;
        orders.reserve(pairCount * 2);
        for (size_t i = 0; i < pairCount; ++i) {
            uint64_t acct = accountIdStart + i;
            for (int j = 0; j < 2; ++j) {
                Order o;
                o.orderId = nextOrderId_++;
                o.accountId = acct;
                o.symbol = "BTC-USD";
                o.side = Side::BUY;
                o.qty = 1;
                o.price = 6000.0; // balance will be set to 10000 -> one fits, two don't
                orders.push_back(o);
            }
        }
        return orders;
    }

private:
    std::mt19937_64 rng_;
    uint64_t nextOrderId_ = 1;
};
