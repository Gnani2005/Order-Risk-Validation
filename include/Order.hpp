#pragma once
#include <string>
#include <cstdint>
#include <chrono>

enum class Side { BUY, SELL };

// Tier 1 stand-in for a parsed FIX NewOrderSingle (tag 35=D) message.
// Real FIX parsing (tag=value pairs, checksum validation) is a Tier 2
// upgrade that plugs in here without touching the engine or the validator
// logic below -- only how an Order gets constructed changes.
struct Order {
    uint64_t orderId;
    uint64_t accountId;
    std::string symbol;
    Side side;
    uint32_t qty;
    double price;

    std::chrono::steady_clock::time_point enqueueTime;
};

struct ValidationResult {
    uint64_t orderId;
    uint64_t accountId;
    bool accepted;
    std::string reason;   // populated on rejection
    double latencyMicros; // enqueue -> validation complete
};
