# UltraEdge Companion Protocol

Embedded-safe C++11 codec for the radio↔phone link (USB-CDC). Same source compiles
into EdgeTX firmware (Cortex-M) **and** builds standalone for host tests/CI. No dynamic
allocation, no exceptions, no RTTI, no external dependencies.

**The wire format is authoritative in [`include/ultraedge/protocol.h`](include/ultraedge/protocol.h)** —
that header is the spec any implementation must match. The implementer-facing prose spec (framing,
handshake, link-state gating, message catalog, conformance) is in **[`SPEC.md`](SPEC.md)**.

Implementing it elsewhere or embedding it? See [CONTRIBUTING](CONTRIBUTING.md) and add your project to
[ADOPTERS](ADOPTERS.md).

## Layout

```
protocol/
  include/ultraedge/protocol.h   public API + wire definitions
  src/protocol.cpp               codec implementation
  tests/test_protocol.cpp        dependency-free unit tests (own assert harness)
  demo/loopback_demo.cpp         mock radio<->phone exchange
  CMakeLists.txt                 standalone build (lib + tests + demo)
```

## Build & test (host)

```bash
# via CMake
cmake -B build . && cmake --build build && ctest --test-dir build --output-on-failure

# or directly, no CMake, no network
g++ -std=c++11 -Wall -Wextra -Iinclude src/protocol.cpp tests/test_protocol.cpp -o test && ./test
g++ -std=c++11 -Wall -Wextra -Iinclude src/protocol.cpp demo/loopback_demo.cpp  -o demo && ./demo
```

## What it provides

- **Framing:** HDLC-style SOF + byte-stuffing, CRC16-CCITT, streaming decoder with resync.
- **Handshake:** `HELLO`/`HELLO_ACK` with proto version + capability negotiation.
- **Messages:** CHANNELS, TELEMETRY, LUA_DATA, KEY, TOUCH, CONFIG_READ/WRITE, AUDIO_EVENT, HEARTBEAT, ACK/NACK.
- **Link state machine** (`LinkManager`): `DETACHED → HANDSHAKING → ATTACHED`, heartbeat timeout, and the central gating decision `companionInputAllowed()` — **the hard requirement that local nav buttons/scroll are inert unless a phone is attached** (no ghost-scroll).

## Safety invariants (enforced by design)

- The codec is pure logic; it never touches the flight-control path.
- Companion input (`KEY`/`TOUCH`/`CONFIG_WRITE`) is gated on `LINK_ATTACHED`.
- Corrupt / oversize / wrong-version frames are dropped, never half-applied.
- Loss of link → `DETACHED` → companion input disabled; control output unaffected.
