#include "FixParser.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>

std::string buildNewOrderSingle(uint64_t account, const std::string& symbol,
                                 char side, uint32_t qty, double price, const std::string& clOrdId) {
    std::string body =
        "35=D" + std::string(1, FixParser::SOH) +
        "55=" + symbol + std::string(1, FixParser::SOH) +
        "54=" + std::string(1, side) + std::string(1, FixParser::SOH) +
        "38=" + std::to_string(qty) + std::string(1, FixParser::SOH) +
        "44=" + std::to_string(price) + std::string(1, FixParser::SOH) +
        "1=" + std::to_string(account) + std::string(1, FixParser::SOH) +
        "11=" + clOrdId + std::string(1, FixParser::SOH);
    return FixParser::buildMessage(body);
}

int main(int argc, char** argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : 9000;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    std::cout << "[client] connected to server on port " << port << "\n";

    struct TestCase { uint64_t account; std::string symbol; char side; uint32_t qty; double price; std::string label; };
    std::vector<TestCase> cases = {
        {1, "BTC-USD", '1', 1, 6000.0, "valid BUY, sufficient balance"},
        {2, "BTC-USD", '1', 1000, 6000.0, "BUY too large -- should reject insufficient_balance"},
        {3, "DOGE-XYZ", '1', 1, 100.0, "unknown symbol -- should reject unknown_symbol"},
        {4, "ETH-USD", '2', 5, 3000.0, "valid SELL"},
    };

    for (size_t i = 0; i < cases.size(); ++i) {
        auto& tc = cases[i];
        std::string msg = buildNewOrderSingle(tc.account, tc.symbol, tc.side, tc.qty, tc.price,
                                               "clord-" + std::to_string(i));
        std::cout << "\n[client] sending: " << tc.label << "\n";
        send(sock, msg.data(), msg.size(), 0);

        char buf[4096];
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            std::string raw(buf, n);
            auto fields = FixParser::splitFields(raw);
            std::string msgType = fields.count(35) ? fields[35] : "?";
            std::string status = fields.count(39) ? fields[39] : "?";
            std::string reason = fields.count(58) ? fields[58] : "";
            std::cout << "[client] response: MsgType=" << msgType
                      << " OrdStatus=" << status
                      << (reason.empty() ? "" : (" Reason=" + reason)) << "\n";
        } else {
            std::cout << "[client] no response / connection closed\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Deliberately send a message SPLIT ACROSS TWO writes, with a delay in
    // between -- proves the event loop's buffering handles a message that
    // doesn't arrive in a single recv() call, not just the easy case.
    std::cout << "\n[client] sending a message split across two writes (framing test)\n";
    std::string splitMsg = buildNewOrderSingle(5, "SOL-USD", '1', 2, 150.0, "clord-split");
    size_t half = splitMsg.size() / 2;
    send(sock, splitMsg.data(), half, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send(sock, splitMsg.data() + half, splitMsg.size() - half, 0);

    char buf[4096];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0) {
        std::string raw(buf, n);
        auto fields = FixParser::splitFields(raw);
        std::cout << "[client] split-message response: MsgType=" << fields[35]
                  << " OrdStatus=" << fields[39] << "\n";
    } else {
        std::cout << "[client] FAILED -- no response to split message\n";
    }

    close(sock);
    return 0;
}
