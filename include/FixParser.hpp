#pragma once
#include "Order.hpp"
#include <string>
#include <optional>
#include <unordered_map>
#include <atomic>
#include <chrono>

// Minimal but real FIX 4.2-style parser. Supports exactly one message type
// (NewOrderSingle, 35=D) -- this is a deliberate scope cut, not an
// oversight; see the Tier 2 plan for what a fuller implementation would add
// (Cancel/Replace, session-level messages, etc).
//
// Wire format: tag=value pairs separated by SOH (0x01), e.g.
//   8=FIX.4.2\x019=112\x0135=D\x0155=BTC-USD\x0154=1\x0138=1\x0144=6000.00\x011=42\x0111=ord-1\x0110=128\x01
//
// Framing is done the way real FIX does it: tag 9 (BodyLength) tells you
// how many bytes follow before the checksum field, so you know exactly
// where a message ends without needing an out-of-band delimiter.
class FixParser {
public:
    static constexpr char SOH = '\x01';

    struct ParseError {
        std::string reason;
    };

    // Attempts to pull ONE complete, checksum-valid raw FIX message out of
    // the front of `buffer`. On success, returns the raw message text and
    // erases those bytes from `buffer`. Returns std::nullopt if the buffer
    // doesn't yet contain a complete message (caller should wait for more
    // bytes from recv()).
    static std::optional<std::string> tryExtractMessage(std::string& buffer) {
        // Find "8=" at the very start -- every FIX message starts with
        // BeginString. If the buffer doesn't start with it yet, either
        // we're mid-garbage (framing desync) or waiting for more bytes.
        if (buffer.size() < 2 || buffer.compare(0, 2, "8=") != 0) {
            return std::nullopt;
        }

        // Find tag 9 (BodyLength). It must appear right after tag 8's
        // SOH-terminated value.
        size_t firstSoh = buffer.find(SOH);
        if (firstSoh == std::string::npos) return std::nullopt;

        size_t bodyLenTagStart = firstSoh + 1;
        if (buffer.compare(bodyLenTagStart, 2, "9=") != 0) return std::nullopt;

        size_t bodyLenValStart = bodyLenTagStart + 2;
        size_t bodyLenSoh = buffer.find(SOH, bodyLenValStart);
        if (bodyLenSoh == std::string::npos) return std::nullopt;

        size_t bodyLength = 0;
        try {
            bodyLength = std::stoul(buffer.substr(bodyLenValStart, bodyLenSoh - bodyLenValStart));
        } catch (...) {
            return std::nullopt; // malformed length -- wait for resync, caller should log+drop
        }

        // Body starts right after tag 9's SOH, and runs for `bodyLength`
        // bytes, followed by the checksum field "10=XXX" + SOH (always
        // exactly 7 bytes: "10=" + 3 digits + SOH).
        size_t bodyStart = bodyLenSoh + 1;
        size_t checksumFieldLen = 7;
        size_t totalMsgLen = bodyStart + bodyLength + checksumFieldLen;

        if (buffer.size() < totalMsgLen) {
            return std::nullopt; // haven't received the full message yet
        }

        std::string rawMessage = buffer.substr(0, totalMsgLen);
        buffer.erase(0, totalMsgLen);
        return rawMessage;
    }

    // Splits a raw message into tag->value pairs. Does NOT validate
    // checksum -- call verifyChecksum() separately so callers can produce
    // a specific rejection reason.
    static std::unordered_map<int, std::string> splitFields(const std::string& raw) {
        std::unordered_map<int, std::string> fields;
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t eq = raw.find('=', pos);
            if (eq == std::string::npos) break;
            size_t soh = raw.find(SOH, eq);
            if (soh == std::string::npos) break;
            try {
                int tag = std::stoi(raw.substr(pos, eq - pos));
                std::string value = raw.substr(eq + 1, soh - eq - 1);
                fields[tag] = value;
            } catch (...) {
                // unparsable tag number -- skip this field, keep going
            }
            pos = soh + 1;
        }
        return fields;
    }

    // FIX checksum: sum of all bytes UP TO (not including) the "10="
    // field, mod 256, formatted as exactly 3 digits.
    static bool verifyChecksum(const std::string& raw) {
        size_t checksumTagPos = raw.rfind("10=");
        if (checksumTagPos == std::string::npos) return false;

        unsigned long sum = 0;
        for (size_t i = 0; i < checksumTagPos; ++i) {
            sum += static_cast<unsigned char>(raw[i]);
        }
        int computed = static_cast<int>(sum % 256);

        size_t valStart = checksumTagPos + 3;
        size_t valEnd = raw.find(SOH, valStart);
        if (valEnd == std::string::npos) return false;
        std::string checksumStr = raw.substr(valStart, valEnd - valStart);

        int received = 0;
        try {
            received = std::stoi(checksumStr);
        } catch (...) {
            return false;
        }
        return computed == received;
    }

    // Full pipeline: raw bytes -> validated Order, or a specific rejection
    // reason. `orderId` is server-assigned (FIX ClOrdID, tag 11, is a
    // string in the real spec -- kept separately if you want it; this
    // Tier-2 scope maps straight onto the same uint64 orderId field Tier 1
    // already used, assigned by an incrementing counter here).
    static std::optional<Order> parseOrder(const std::string& raw, std::string& rejectReasonOut) {
        if (!verifyChecksum(raw)) {
            rejectReasonOut = "bad_checksum";
            return std::nullopt;
        }
        auto fields = splitFields(raw);

        auto it35 = fields.find(35);
        if (it35 == fields.end() || it35->second != "D") {
            rejectReasonOut = "unsupported_msgtype";
            return std::nullopt;
        }

        auto has = [&](int tag) { return fields.find(tag) != fields.end(); };
        if (!has(55) || !has(54) || !has(38) || !has(44) || !has(1)) {
            rejectReasonOut = "malformed_message";
            return std::nullopt;
        }

        Order o;
        try {
            o.orderId = nextOrderId_.fetch_add(1, std::memory_order_relaxed);
            o.accountId = std::stoull(fields[1]);
            o.symbol = fields[55];
            o.side = (fields[54] == "1") ? Side::BUY : Side::SELL;
            o.qty = static_cast<uint32_t>(std::stoul(fields[38]));
            o.price = std::stod(fields[44]);
        } catch (...) {
            rejectReasonOut = "malformed_message";
            return std::nullopt;
        }
        o.enqueueTime = std::chrono::steady_clock::now();
        return o;
    }

    // Helper for the test client / server acks: builds a valid FIX message
    // with correct BodyLength and checksum, given already-formed body
    // content (everything between tag 9 and tag 10).
    static std::string buildMessage(const std::string& bodyFields) {
        std::string body = bodyFields; // caller includes trailing SOH on each field
        std::string withLen = "9=" + std::to_string(body.size()) + std::string(1, SOH) + body;
        std::string head = "8=FIX.4.2" + std::string(1, SOH);
        std::string msgNoChecksum = head + withLen;

        unsigned long sum = 0;
        for (char c : msgNoChecksum) sum += static_cast<unsigned char>(c);
        int checksum = static_cast<int>(sum % 256);

        char checksumBuf[8];
        snprintf(checksumBuf, sizeof(checksumBuf), "%03d", checksum);
        return msgNoChecksum + "10=" + std::string(checksumBuf) + std::string(1, SOH);
    }

private:
    static inline std::atomic<uint64_t> nextOrderId_{1};
};
