#pragma once
#include "FixParser.hpp"
#include "ThreadPool.hpp"
#include "RiskValidator.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>

// One TCP connection's state. Held by shared_ptr so a worker-thread job
// that's mid-validation can still safely write the response even if the
// event loop has since closed and forgotten this connection.
//
// KNOWN SIMPLIFICATION (documented deliberately, not hidden): responses are
// written directly from worker threads via send(), guarded only by
// writeMutex_, rather than routed back through the epoll loop with
// EPOLLOUT/backpressure handling. This is fine for short FIX acks under
// normal load, and fine for a demo/loopback test client, but a production
// system would queue outbound bytes and let the event loop own all writes
// to avoid partial-write and fd-reuse-after-close hazards.
struct Connection {
    int fd;
    std::string recvBuffer;
    std::mutex writeMutex;
    std::atomic<bool> alive{true};

    // Coordinates closing the real fd until every dispatched job for this
    // connection has finished sending its response. Without this, a
    // client that sends a message and disconnects quickly (e.g. `nc file
    // < msg`) can have the connection torn down before a worker thread
    // gets a chance to write the response -- exactly the bug this fixes.
    std::atomic<int> pendingJobs{0};
    std::atomic<bool> peerClosed{false};
    std::atomic<bool> fdClosed{false};

    explicit Connection(int fd_) : fd(fd_) {}

    void safeSend(const std::string& data) {
        std::lock_guard<std::mutex> lock(writeMutex);
        if (!alive.load(std::memory_order_acquire)) return;
        ssize_t sent = 0;
        while (sent < static_cast<ssize_t>(data.size())) {
            ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) return; // best-effort; peer gone or buffer full -- drop
            sent += n;
        }
    }

    // Call after peerClosed is set, and after every pendingJobs decrement.
    // Only the call that observes peerClosed==true AND pendingJobs==0
    // actually performs the close, guarded so it happens exactly once.
    void tryFinalize() {
        if (peerClosed.load(std::memory_order_acquire) &&
            pendingJobs.load(std::memory_order_acquire) == 0) {
            bool expected = false;
            if (fdClosed.compare_exchange_strong(expected, true)) {
                alive.store(false, std::memory_order_release);
                close(fd);
            }
        }
    }
};

class EventLoop {
public:
    EventLoop(int port, ThreadPool& pool, RiskValidator& validator)
        : port_(port), pool_(pool), validator_(validator) {}

    ~EventLoop() { stop(); }

    void run() {
        setupListenSocket();
        epollFd_ = epoll_create1(0);
        if (epollFd_ < 0) { perror("epoll_create1"); return; }

        registerFd(listenFd_, EPOLLIN);

        std::cout << "[EventLoop] listening on port " << port_ << std::endl;

        constexpr int MAX_EVENTS = 64;
        epoll_event events[MAX_EVENTS];

        running_ = true;
        while (running_) {
            int n = epoll_wait(epollFd_, events, MAX_EVENTS, 200 /*ms*/);
            for (int i = 0; i < n; ++i) {
                if (events[i].data.fd == listenFd_) {
                    acceptNewConnections();
                } else {
                    handleReadable(events[i].data.fd);
                }
            }
        }
    }

    void stop() {
        running_ = false;
        if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
        if (epollFd_ >= 0) { close(epollFd_); epollFd_ = -1; }
        for (auto& [fd, conn] : connections_) {
            conn->alive.store(false, std::memory_order_release);
            close(fd);
        }
        connections_.clear();
    }

private:
    void setupListenSocket() {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind"); return;
        }
        if (listen(listenFd_, SOMAXCONN) < 0) {
            perror("listen"); return;
        }
        setNonBlocking(listenFd_);
    }

    static void setNonBlocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    void registerFd(int fd, uint32_t events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev);
    }

    void acceptNewConnections() {
        while (true) {
            sockaddr_in clientAddr{};
            socklen_t len = sizeof(clientAddr);
            int clientFd = accept(listenFd_, (sockaddr*)&clientAddr, &len);
            if (clientFd < 0) break; // EAGAIN -- no more pending connections

            setNonBlocking(clientFd);
            int one = 1;
            setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); // low latency

            registerFd(clientFd, EPOLLIN);
            connections_[clientFd] = std::make_shared<Connection>(clientFd);
        }
    }

    void handleReadable(int fd) {
        auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        std::shared_ptr<Connection> conn = it->second;

        bool peerClosed = false;
        char tmp[4096];
        while (true) {
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
            if (n > 0) {
                conn->recvBuffer.append(tmp, n);
            } else if (n == 0) {
                peerClosed = true;
                break; // peer closed its write side -- stop reading, but
                       // still process whatever we already buffered below
            } else {
                break; // EAGAIN -- drained everything available right now
            }
        }

        // If the peer has closed, deregister from epoll and stop routing
        // to this fd from the event loop BEFORE dispatching any pending
        // work to worker threads. A worker's tryFinalize() may call
        // close(fd) as soon as its job finishes, so epoll_ctl(DEL) must
        // happen-before that dispatch, not after -- otherwise the main
        // thread's epoll_ctl call and a worker thread's close() call can
        // race on the same fd concurrently (a real race TSan caught
        // during testing of an earlier version of this fix).
        if (peerClosed) {
            conn->peerClosed.store(true, std::memory_order_release);
            epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
            connections_.erase(fd); // event loop no longer routes to this fd;
                                     // any in-flight worker jobs still hold
                                     // the Connection alive via shared_ptr
        }

        // Always try to extract and dispatch complete messages, even if
        // the peer has already closed -- a client that sends a message
        // and immediately disconnects (very common: short test clients,
        // `nc file < msg`, fire-and-forget load generators) must still
        // get a response, not have it silently dropped just because EOF
        // arrived in the same read batch as the message.
        while (true) {
            auto rawMsgOpt = FixParser::tryExtractMessage(conn->recvBuffer);
            if (!rawMsgOpt.has_value()) break;
            dispatchToWorker(conn, *rawMsgOpt);
        }

        if (peerClosed) {
            conn->tryFinalize(); // closes the real fd now if nothing is pending
        }
    }

    void dispatchToWorker(std::shared_ptr<Connection> conn, std::string rawMsg) {
        conn->pendingJobs.fetch_add(1, std::memory_order_relaxed);
        pool_.submit([conn, rawMsg, this] {
            std::string rejectReason;
            auto orderOpt = FixParser::parseOrder(rawMsg, rejectReason);

            if (!orderOpt.has_value()) {
                std::string nak = FixParser::buildMessage(
                    "35=3" + std::string(1, FixParser::SOH) + // MsgType=Reject
                    "58=" + rejectReason + std::string(1, FixParser::SOH));
                conn->safeSend(nak);
            } else {
                ValidationResult result = validator_.validate(*orderOpt);

                std::string msgType = "35=8"; // ExecutionReport either way
                std::string ordStatus = result.accepted ? "39=0" : "39=8"; // 0=New 8=Rejected
                std::string body = msgType + std::string(1, FixParser::SOH) +
                                    "37=" + std::to_string(result.orderId) + std::string(1, FixParser::SOH) +
                                    ordStatus + std::string(1, FixParser::SOH);
                if (!result.accepted) {
                    body += "58=" + result.reason + std::string(1, FixParser::SOH);
                }
                conn->safeSend(FixParser::buildMessage(body));
            }

            conn->pendingJobs.fetch_sub(1, std::memory_order_acq_rel);
            conn->tryFinalize();
        });
    }

    void closeConnection(int fd) {
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            it->second->alive.store(false, std::memory_order_release);
            epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            connections_.erase(it);
        }
    }

    int port_;
    int listenFd_ = -1;
    int epollFd_ = -1;
    std::atomic<bool> running_{false};
    ThreadPool& pool_;
    RiskValidator& validator_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
};
