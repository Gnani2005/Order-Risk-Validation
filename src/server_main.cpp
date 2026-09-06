#include "ThreadPool.hpp"
#include "AccountStore.hpp"
#include "RiskValidator.hpp"
#include "EventLoop.hpp"
#include <iostream>
#include <csignal>
#include <thread>

static EventLoop* g_loop = nullptr;

void handleSigint(int) {
    if (g_loop) g_loop->stop();
}

int main(int argc, char** argv) {
    int port = 9000;
    if (argc > 1) port = std::atoi(argv[1]);

    AccountStore store;
    // Seed a handful of demo accounts. A real deployment would load this
    // from a real balance service; Tier 1 already showed how the same
    // AccountStore plugs into a synthetic in-memory feed instead.
    for (uint64_t acct = 1; acct <= 100; ++acct) {
        store.setBalance(acct, 100000.0);
    }

    RiskValidator validator(store);
    ThreadPool pool(std::thread::hardware_concurrency());

    signal(SIGINT, handleSigint);

    EventLoop loop(port, pool, validator);
    g_loop = &loop;
    loop.run(); // blocks until stop() is called (e.g. Ctrl+C)

    std::cout << "[server] shut down cleanly\n";
    return 0;
}
