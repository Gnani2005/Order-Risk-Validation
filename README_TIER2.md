# HFT Risk Engine — Tier 2 (Real Sockets + Real FIX Parsing)

This extends the Tier 1 engine (unchanged: `ThreadPool`, `TaskQueue`,
`AccountStore`, `RiskValidator`) with:

- **Phase 9**: a real epoll-based, non-blocking TCP server (`EventLoop.hpp`)
- **Phase 10**: a real FIX 4.2-style parser (`FixParser.hpp`) — tag=value
  parsing, BodyLength-based message framing, checksum validation

Nothing in `ThreadPool.hpp`, `TaskQueue.hpp`, `AccountStore.hpp`, or
`RiskValidator.hpp` changed. That's the payload-agnostic design paying off —
the engine core didn't need to know or care that its input source changed
from an in-memory generator to a real socket.

## New files

| File | What it does |
|---|---|
| `include/FixParser.hpp` | Extracts complete FIX messages from a byte buffer, validates checksums, parses `NewOrderSingle` (35=D) into the same `Order` struct Tier 1 used |
| `include/EventLoop.hpp` | epoll event loop: accepts connections, reads bytes, frames messages, hands parsed orders to the existing `ThreadPool` |
| `src/server_main.cpp` | Ties `AccountStore` + `RiskValidator` + `ThreadPool` + `EventLoop` together into a runnable server |
| `src/test_client.cpp` | A real TCP client that sends real FIX bytes and prints the server's responses — this is how the whole pipeline was verified end-to-end, not just assumed to work |

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

This produces three binaries: `hft_engine` (the Tier 1 test suite, unchanged),
`hft_server` (the new Tier 2 server), and `hft_test_client`.

## Run it end-to-end

**Terminal 1 — start the server:**
```bash
./hft_server 9000
```
You should see: `[EventLoop] listening on port 9000`
Leave this running.

**Terminal 2 — run the test client against it:**
```bash
./hft_test_client 9000
```

Expected output: five test cases, each printing the FIX response the server
sent back — a valid BUY (accepted), an oversized BUY (rejected,
`insufficient_balance`), an unknown symbol (rejected, `unknown_symbol`), a
valid SELL (accepted), and a message deliberately split across two separate
`send()` calls to prove the framing logic handles a message arriving in
pieces (this is the actual hard part of writing a TCP protocol parser —
most beginner attempts assume one `recv()` = one message, which is false).

Press `Ctrl+C` in Terminal 1 to stop the server (it has a SIGINT handler
that calls `EventLoop::stop()` for a clean shutdown, closing all sockets).

## Manually testing with your own client

Since the server speaks real FIX over a real TCP socket, you can also
connect with `nc` (netcat) if you construct a raw message by hand — though
this is fiddly since FIX uses the non-printable SOH (`\x01`) byte as a
delimiter, which is why `test_client.cpp` exists as the primary way to
interact with it. If you want to try `nc` anyway:

```bash
printf '8=FIX.4.2\x019=43\x0135=D\x0155=BTC-USD\x0154=1\x0138=1\x0144=6000\x011=1\x0111=x\x0110=087\x01' | nc -q 1 127.0.0.1 9000 | tr '\001' '|'
```
This exact command was tested against the actual server and confirmed working
(`-q 1` makes `nc` quit after sending instead of hanging forever waiting for
the server to close the still-open session; `tr '\001' '|'` makes the
otherwise-invisible SOH byte visible so the response is readable). Expect:
```
8=FIX.4.2|9=15|35=8|37=...|39=0|10=...|
```
(Getting the checksum digit right by hand is tedious — this is exactly why
`FixParser::buildMessage()` exists, to compute it correctly. The test
client is the reliable way to test; treat manual `nc` testing as a novelty,
not your main verification path.)

## What's still simplified (say these honestly in an interview)

- Only `NewOrderSingle` (35=D) is supported — no Cancel/Replace, no
  session-level FIX messages (Logon/Heartbeat/etc).
- Responses are written directly from worker threads via `send()`, guarded
  by a per-connection mutex — not routed back through the epoll loop with
  proper `EPOLLOUT` backpressure handling. Documented in `EventLoop.hpp`
  as a deliberate, named simplification.
- The queue between the event loop and the worker pool is still Tier 1's
  mutex-based `TaskQueue` — Phase 11 (lock-free queue) is a separate,
  optional upgrade on top of this.
- No real matching engine — this is still risk-validation only, exactly
  like Tier 1. Phase 12 in the Tier 2 plan covers a real order book if you
  want to go further.

## Verification performed

This was built and run in a real sandbox, not just written and assumed
correct:
- Compiles clean with CMake + g++.
- Server actually starts and binds a real TCP listening socket.
- Test client actually connects over loopback TCP and exchanges real bytes.
- All four order-validation test cases produced the correct accept/reject
  outcome, matching Tier 1's `RiskValidator` logic exactly (same class,
  unchanged).
- The split-message framing test confirms `FixParser::tryExtractMessage`
  correctly buffers a partial message across multiple `recv()` calls
  instead of failing or misframing it.
