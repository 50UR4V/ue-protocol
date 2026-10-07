# UE Protocol — Specification

The radio↔phone link protocol for UltraEdge-style companions. This document is the implementer's
reference; the **authoritative** definitions (constants, message ids, field layouts) live in
[`include/ultraedge/protocol.h`](include/ultraedge/protocol.h), and the reference C++11 codec in
[`src/protocol.cpp`](src/protocol.cpp) with tests in [`tests/`](tests/). Where this prose and the header
disagree, the header wins.

**Version:** `PROTO_VERSION = 4` (negotiated in the handshake; see §3). **Transport:** any reliable,
ordered byte stream — in practice USB-CDC serial. The protocol is transport-agnostic: one endpoint feeds
received bytes into a decoder and writes encoded frames out.

**Safety invariant (normative):** the phone is a companion display/editor, **never** on the flight-control
path. Inbound control-ish messages (`KEY`, `TOUCH`, `CONFIG_WRITE`, and any actuator command) are accepted
by the radio **only** while the link is `ATTACHED` (§4). Loss of link must return the radio to standalone
behavior with no companion dependency.

---

## 1. Framing
Each frame on the wire:

```
SOF  <escaped( VER  TYPE  SEQ  LEN_L  LEN_H  PAYLOAD[LEN]  CRC_L  CRC_H )>
```

- `SOF = 0x7E` — start-of-frame flag (unescaped; marks a frame boundary).
- **Byte-stuffing:** within a frame, any byte equal to `SOF (0x7E)` or `ESC (0x7D)` is escaped as
  `ESC` followed by `(byte XOR 0x20)`. Decoders un-stuff before parsing.
- **Header (5 bytes):** `VER` (protocol version of the sender), `TYPE` (message id, §5), `SEQ`
  (per-sender sequence, wraps at 256), `LEN_L`/`LEN_H` (payload length, little-endian, u16).
- **Payload:** `LEN` bytes (≤ `MAX_PAYLOAD = 512`).
- **CRC:** `CRC16-CCITT` (poly `0x1021`, init `0xFFFF`, big-endian on the wire as `CRC_L,CRC_H` per the
  header) computed over the **unescaped** `VER..PAYLOAD` bytes. This is the *only* checksum in the system.
- A decoder resynchronizes on `SOF`; a corrupt, oversize, or CRC-failing frame is **dropped whole**,
  never half-applied.

All multi-byte integers in payloads are **little-endian** unless a message says otherwise.

## 2. Message header fields
`VER` lets a receiver detect a peer on a different protocol version. `SEQ` is echoed by `ACK`/`NACK`
(`ref_seq`) for request/reply correlation. `TYPE` selects the message (§5).

## 3. Handshake & versioning
- On transport-up, each side sends **`HELLO` (0x01)** — payload `{proto_ver(1), fw_ver(4), role(1),
  caps(4)}` — and replies with **`HELLO_ACK` (0x02)** of the same shape.
- `proto_ver` is `PROTO_VERSION`. Peers negotiate to the **lower** common version; a peer that only
  understands an older version ignores newer message types.
- `caps` is a bitfield of optional capabilities (e.g. `CAP_STRUCTURED = 1<<0`). A feature is used only if
  both peers advertise it. Unknown caps bits are ignored.
- `role` distinguishes radio vs companion.
- **`HEARTBEAT` (0x03)** — empty payload — keeps the link alive; absence past a timeout drops the link
  to `DETACHED` (§4).

## 4. Link-state machine (normative)
```
DETACHED ──transport up──▶ HANDSHAKING ──HELLO/ACK ok + heartbeat──▶ ATTACHED
   ▲                                                                     │
   └──────────────── transport down / heartbeat timeout ────────────────┘
```
- `LINK_DETACHED (0)` — no companion. The radio flies normally; companion inputs are **inert**.
- `LINK_HANDSHAKING (1)` — transport up, HELLO in progress; still inert for control/config.
- `LINK_ATTACHED (2)` — handshake complete and heartbeat alive; companion I/O active.
- The single gate `companionInputAllowed() == (state == ATTACHED)` MUST be checked by the radio before
  dispatching any `KEY`/`TOUCH`/`CONFIG_WRITE`/actuator message. This is the enforcement point for the
  safety invariant.

## 5. Message catalog
Direction R→P = radio to phone, P→R = phone to radio. Ids are stable; the header is authoritative.

| Id | Name | Dir | Payload | Notes |
|----|------|-----|---------|-------|
| 0x01 | HELLO | both | proto_ver, fw_ver(4), role, caps(4) | handshake |
| 0x02 | HELLO_ACK | both | same as HELLO | |
| 0x03 | HEARTBEAT | both | — | keepalive |
| 0x10 | CHANNELS | R→P | count(1), int16×count | live channel outputs |
| 0x11 | TELEMETRY | R→P | count(1), [id(2), value(4)]×count | live sensor values |
| 0x12 | LUA_DATA | R→P | (impl-defined superset) | for on-phone Lua |
| 0x20 | KEY | P→R | keycode(1), event(1) | **gated on ATTACHED** |
| 0x21 | TOUCH | P→R | touch state | **gated on ATTACHED** |
| 0x30 | CONFIG_READ | P→R | field_id(2) | request one config item |
| 0x31 | CONFIG_WRITE | P→R | field_id(2), type(1), len(1), bytes | set — **gated on ATTACHED** |
| 0x32 | CONFIG_VALUE | R→P | field_id(2), type(1), len(1), bytes | current value |
| 0x33 | CONFIG_PAGES | R→P | count(1), [page_id(2), icon(1), str title] | top-level page list |
| 0x34 | GET_PAGES | P→R | — | request the page list |
| 0x35 | DESCRIBE_PAGE | P→R | page_id(2) | request a page's rows |
| 0x36 | FIELD_DESC | R→P | row descriptor (§6) | one row |
| 0x37 | PAGE_DESC_END | R→P | page_id(2) | end-of-rows marker |
| 0x38 | CONFIG_CHANGED | R→P | field_id(2), type, len, bytes | value changed on the radio |
| 0x39 | GET_CAPS | P→R | — | request value-space anchors |
| 0x3A | CAPS | R→P | count(1), [key(1), value(2)] | anchors the app names from (§6) |
| 0x40 | AUDIO_EVENT | R→P | event_id(2), param(2) | phone owns the sound files |
| 0x50 | SUB_SET | P→R | stream_id(1), rate_hz(1) | subscribe a stream (QoS) |
| 0x51 | SUB_CLEAR | P→R | stream_id(1) | unsubscribe |
| 0x70 | ACK | both | ref_seq(1) | |
| 0x71 | NACK | both | ref_seq(1), reason(1) | |

*Higher id ranges (bulk file transfer, extended config, etc.) are product extensions layered on this
core; a conformant core implementation ignores message types it doesn't know.*

## 6. Config & describe model (summary)
Configuration is **self-describing**: the radio advertises pages (`CONFIG_PAGES`), the phone requests a
page's rows (`DESCRIBE_PAGE`), and the radio streams `FIELD_DESC` rows (label, `ConfigType`, range, step,
unit, enum options, or a link target) then `PAGE_DESC_END`. The phone renders the right widget per type
and reads/writes values by `field_id`. Types (`ConfigType`): `T_U8/I8/U16/I16/I32/STR/BOOL/ENUM`, `T_LINK`
(navigation row, not a value), and `T_REF` (value is a raw index into a name space — source/switch/curve).
For `T_REF`, the **app owns naming**: the radio sends only value-space anchors via `CAPS`, so a
name-less firmware needs no string tables. Field-id ranges (`CFG_*_BASE`) and the describe row byte-layout
are enumerated in the header.

## 7. Traffic classes / QoS
Streams (channels, telemetry, …) are subscribed via `SUB_SET {stream_id, rate_hz}` and stopped with
`SUB_CLEAR`. Config/handshake traffic is treated as higher priority than bulk streams so a busy stream
never starves an interactive write. Implementations should pace stream output to the transport's budget.

## 8. Conformance
A conformant implementation:
1. Frames/deframes per §1 (SOF, byte-stuffing, CRC16-CCITT over unescaped bytes) and drops bad frames.
2. Performs the §3 handshake and negotiates version/caps.
3. Enforces the §4 gate on all control/config-write inbound messages (radio side).
4. Round-trips the §5 core messages it advertises support for.

The host test suite (`tests/`) exercises framing, handshake, gating, and message round-trips; run it via
CMake+ctest or the direct `g++` line in the [README](README.md). New implementations are welcome — add
yourself to [`ADOPTERS.md`](ADOPTERS.md).
