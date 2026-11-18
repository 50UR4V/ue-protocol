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
- **Retired, never reused:** caps bit 22 and config field 72. A 2026-11-14 draft used them for a wireless arm switch set
  from the app; it never shipped, and a development radio may still send or accept them.
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
| 0x4F | TRIM_OP | P→R | op(1), arg(1) | trim command, `ACK`/`NACK` — **gated on ATTACHED**; only with `CAP_TRIMS` (§6.3) |
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

### 6.1 Optional: the radio's AUX serial port (`CAP_AUX_MODE`)
A radio that advertises `CAP_AUX_MODE` (bit 19) has an AUX1 serial port that can carry this protocol to a wireless
bridge. Its mode is the config field `CFG_GEN_AUX1_MODE` (71, `T_ENUM`):

- **Read:** 0 Off, 1 companion link, 2 another use (trainer, telemetry, …).
- **Write:** only 0 or 1. The radio validates the write, applies it after its `ACK` and saves the setting itself.
- **Refused with `NACK`:**
  - `NACK_AUX_IN_USE` (16): the port holds another mode, which a peer must never overwrite;
  - reason 6: a model transfer is open;
  - reason 13: the radio has no such port.

A peer that doesn't know the bit, the field or the reason ignores them (§3). Nothing else in the protocol changes.

### 6.2 Optional: fast sticks (`CAP_FAST_STICKS`)
A radio that advertises `CAP_FAST_STICKS` (bit 20) answers a `STREAM_SOURCES` subscription above 30 Hz with extra
`MSG_SOURCES` frames that carry the sticks only, as often as every 10 ms. The full set (sticks, pots, switches) keeps
arriving at its normal pace, so a peer merges a sticks-only frame into the last full one. A radio without the bit
clamps the rate and sends full frames only.

### 6.3 Optional: trims (`CAP_TRIMS`)
A radio that advertises `CAP_TRIMS` (bit 21) does three things:

- **Real trim count.** The `CAPS` key `CAP_N_TRIM` (10) is the number of trims the radio has (4 on most radios, 6 on a
  radio with T5/T6), not the size of its trim array. The trim sources start at `CAP_SRC_TRIM` and the trim switches
  at `CAP_SW_TRIM`, two per trim (down, up), so a peer offers exactly `CAP_N_TRIM` trim sources and `2 × CAP_N_TRIM`
  trim switches. The base anchors keep their values. Without the bit, `CAP_N_TRIM` is the array size and may include
  trims the radio doesn't have.
- **Live values survive a model save.** Trim values and global-variable values (every flight mode) change on the radio,
  in flight too, and its RAM is their truth. When the phone saves the radio's *active* model, the radio keeps its live
  values across the reload: the file supplies everything else, including each trim's mode. A model written as a new
  file (a restore) is not loaded, so it keeps the values in its file.
- **`TRIM_OP` (0x4F)**, payload `{op(1), arg(1)}`, answered with `ACK {ref_seq}` or `NACK {ref_seq, reason}`:

  | op | Name | Effect (as the radio's own menu item) | arg |
  |---|---|---|---|
  | 0 | `TRIM_RESET` | every trim of every flight mode cleared | ignored |
  | 1 | `TRIMS_TO_SUBTRIMS` | the trims' effect on every output added to its subtrim, trims zeroed | ignored |
  | 2 | `TRIM_TO_SUBTRIM` | the trims' effect on output `arg` added to its subtrim | channel, 0-based |
  | 3 | `STICK_TO_SUBTRIM` | output `arg`'s current stick position made its subtrim | channel, 0-based |

  `NACK` reasons: `NACK_NOT_ATTACHED` (5), `NACK_BAD_LENGTH` (3), `NACK_OUT_OF_RANGE` (4, unknown op or channel),
  `NACK_PUSH_STATE` (6, a save of the active model is being received: retry after it commits). The ops change subtrims
  (and op 0 the trim modes), which the model file holds: after an `ACK` a peer that keeps a copy of the model file
  fetches it again before its next save.

`encodeTrimOp` / `parseTrimOp` in the reference codec build and check the message.

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

---
[![CC BY 4.0](https://licensebuttons.net/l/by/4.0/88x31.png)](https://creativecommons.org/licenses/by/4.0/)
This specification is © 2026 Sourav Karmakar (UltraEdge), licensed under
[CC BY 4.0](LICENSE-SPEC.md). The reference code is [MIT](LICENSE).
