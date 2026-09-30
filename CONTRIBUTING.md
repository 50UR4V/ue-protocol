# Contributing to UltraEdge Protocol

Thanks for your interest. UltraEdge Protocol is the open link contract between an EdgeTX radio and a
companion app over USB-CDC. It is deliberately small, embedded-safe (C++11, no allocation, no
exceptions, no RTTI, no dependencies), and implementable in any language.

## Ground rules

- **The wire format in `include/ultraedge/protocol.h` is authoritative.** Any change to framing, the
  CRC, the handshake, message IDs, or field layouts is a wire change: bump the protocol version and
  describe the compatibility impact in the PR.
- **Keep it embedded-safe.** No heap allocation, exceptions, RTTI, or external dependencies in the
  library — the same source compiles into Cortex-M firmware. CI builds it plain with `-Wall -Wextra`.
- **Tests are required for wire changes.** Add or update cases in `tests/test_protocol.cpp` (its own
  dependency-free assert harness). `ctest` must stay green; CI runs both the CMake and the direct
  `g++` build.
- **Safety invariants are not negotiable** (see the README): the codec never touches a flight-control
  path; companion input is gated on `LINK_ATTACHED`; corrupt/oversize/wrong-version frames are dropped,
  never half-applied.

## Building and testing

```bash
cmake -B build . && cmake --build build && ctest --test-dir build --output-on-failure
# or, no CMake:
g++ -std=c++11 -Wall -Wextra -Iinclude src/protocol.cpp tests/test_protocol.cpp -o t && ./t
```

## Implementing the protocol elsewhere

A conformance-oriented reimplementation (another language, another radio, another app) is exactly what
this repo is for. Open a PR adding your project to [`ADOPTERS.md`](ADOPTERS.md) — that list is how the
ecosystem finds each other. If you hit an ambiguity in the spec, file an issue; tightening the wire
contract so independent implementations interoperate is the whole point.

## Licensing

The reference codec is **Apache-2.0** and the protocol specification is **CC-BY-4.0** (see `LICENSE`
files added at publication). By contributing you agree your contribution is licensed under those terms.
