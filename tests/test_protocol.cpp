// UltraEdge protocol — dependency-free unit tests (no gtest, builds anywhere).
// Build:  g++ -std=c++11 -I../include ../src/protocol.cpp test_protocol.cpp -o test
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ultraedge/protocol.h"
#include <cstdio>
#include <cstring>

using namespace ultraedge;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { \
  if (cond) { g_pass++; } \
  else { g_fail++; std::printf("  FAIL: %s  (line %d)\n", #cond, __LINE__); } \
} while (0)

// Capture decoded messages into a small ring for assertions.
struct Capture {
  int count = 0;
  uint8_t types[16];
  uint8_t seqs[16];
  uint8_t last_payload[512];
  uint16_t last_len = 0;
};
static void onMsg(const Message& m, void* user) {
  Capture* c = (Capture*)user;
  if (c->count < 16) { c->types[c->count] = m.type; c->seqs[c->count] = m.seq; }
  c->count++;
  c->last_len = m.len;
  if (m.len && m.payload) std::memcpy(c->last_payload, m.payload, m.len);
}

static void test_crc_known_vector() {
  // CRC16-CCITT (0xFFFF init) of "123456789" is 0x29B1.
  const uint8_t s[] = "123456789";
  CHECK(crc16(s, 9) == 0x29B1);
}

static void test_roundtrip_channels() {
  int16_t ch[8] = {0, 1024, -1024, 32767, -32768, 100, -100, 512};
  uint8_t frame[MAX_FRAME_WIRE];
  size_t n = encodeChannels(7, ch, 8, frame, sizeof(frame));
  CHECK(n > 0);

  Capture cap;
  Decoder d; d.setCallback(onMsg, &cap);
  d.feed(frame, n);

  CHECK(cap.count == 1);
  CHECK(cap.types[0] == MSG_CHANNELS);
  CHECK(cap.seqs[0] == 7);
  CHECK(d.framesOk() == 1);

  // Rebuild a Message view to parse channels back
  Message m; m.type = MSG_CHANNELS; m.payload = cap.last_payload; m.len = cap.last_len;
  int16_t out[8]; uint8_t got = parseChannels(m, out, 8);
  CHECK(got == 8);
  bool same = true; for (int i=0;i<8;i++) if (out[i]!=ch[i]) same=false;
  CHECK(same);
}

static void test_escaping() {
  // Payload full of bytes that must be escaped (SOF, ESC) must survive round-trip.
  uint8_t payload[6] = {SOF, ESC, SOF, ESC, 0x00, SOF};
  uint8_t frame[MAX_FRAME_WIRE];
  size_t n = encodeFrame(MSG_TELEMETRY, 3, payload, 6, frame, sizeof(frame));
  CHECK(n > 0);
  // Ensure no raw SOF/ESC leaked into the body (only the leading SOF is raw).
  int rawSOF = 0; for (size_t i=1;i<n;i++) if (frame[i]==SOF) rawSOF++;
  CHECK(rawSOF == 0);

  Capture cap; Decoder d; d.setCallback(onMsg,&cap); d.feed(frame,n);
  CHECK(cap.count == 1);
  CHECK(cap.last_len == 6);
  CHECK(std::memcmp(cap.last_payload, payload, 6) == 0);
}

static void test_crc_rejects_corruption() {
  int16_t ch[2] = {10, 20};
  uint8_t frame[MAX_FRAME_WIRE];
  size_t n = encodeChannels(1, ch, 2, frame, sizeof(frame));
  // Flip a bit in the middle of the frame body.
  frame[3] ^= 0x01;
  Capture cap; Decoder d; d.setCallback(onMsg,&cap); d.feed(frame,n);
  CHECK(cap.count == 0);
  CHECK(d.framesCrcErr() >= 1);
}

static void test_resync_after_garbage() {
  int16_t ch[1] = {42};
  uint8_t frame[MAX_FRAME_WIRE];
  size_t n = encodeChannels(9, ch, 1, frame, sizeof(frame));

  Capture cap; Decoder d; d.setCallback(onMsg,&cap);
  // Junk, a truncated frame, then a good frame — decoder must recover.
  uint8_t junk[5] = {0x11, 0x22, SOF, 0xAB, 0xCD}; // SOF starts a bogus partial
  d.feed(junk, sizeof(junk));
  d.feed(frame, n);
  CHECK(cap.count == 1);
  CHECK(cap.types[0] == MSG_CHANNELS);
  CHECK(cap.seqs[0] == 9);
}

static void test_handshake_and_attach() {
  LinkManager radio(ROLE_RADIO);
  CHECK(radio.state() == LINK_DETACHED);
  CHECK(radio.companionInputAllowed() == false);

  radio.onTransportUp();
  CHECK(radio.state() == LINK_HANDSHAKING);
  CHECK(radio.companionInputAllowed() == false); // still inert during handshake

  // Phone sends HELLO with matching proto version.
  Hello ph; ph.proto_ver = PROTO_VERSION; ph.fw_ver = 0x00010000; ph.role = ROLE_PHONE;
  ph.caps = CAP_STRUCTURED | CAP_AUDIO;
  uint8_t frame[MAX_FRAME_WIRE];
  size_t n = encodeHello(0, ph, false, frame, sizeof(frame));

  Capture cap; Decoder d; d.setCallback(onMsg,&cap); d.feed(frame,n);
  CHECK(cap.count == 1 && cap.types[0] == MSG_HELLO);
  Message m; m.type=cap.types[0]; m.seq=cap.seqs[0]; m.payload=cap.last_payload; m.len=cap.last_len;
  radio.onMessage(m, /*now_ms=*/100);
  CHECK(radio.state() == LINK_ATTACHED);
  CHECK(radio.companionInputAllowed() == true);   // NOW buttons may drive companion GUI
  CHECK(radio.peerHello().role == ROLE_PHONE);
}

static void test_version_mismatch_refused() {
  LinkManager radio(ROLE_RADIO);
  radio.onTransportUp();
  Hello ph; ph.proto_ver = PROTO_VERSION + 7; ph.fw_ver = 1; ph.role = ROLE_PHONE; ph.caps = 0;
  uint8_t frame[MAX_FRAME_WIRE]; size_t n = encodeHello(0, ph, false, frame, sizeof(frame));
  Capture cap; Decoder d; d.setCallback(onMsg,&cap); d.feed(frame,n);
  Message m; m.type=cap.types[0]; m.seq=cap.seqs[0]; m.payload=cap.last_payload; m.len=cap.last_len;
  radio.onMessage(m, 100);
  CHECK(radio.state() != LINK_ATTACHED);        // must NOT attach on version mismatch
  CHECK(radio.companionInputAllowed() == false);
}

static void test_heartbeat_timeout_detaches() {
  LinkManager radio(ROLE_RADIO);
  radio.setHeartbeatTimeout(1000);
  radio.onTransportUp();
  Hello ph; ph.proto_ver = PROTO_VERSION; ph.fw_ver=1; ph.role=ROLE_PHONE; ph.caps=0;
  uint8_t frame[MAX_FRAME_WIRE]; size_t n=encodeHello(0,ph,false,frame,sizeof(frame));
  Capture cap; Decoder d; d.setCallback(onMsg,&cap); d.feed(frame,n);
  Message m; m.type=cap.types[0]; m.payload=cap.last_payload; m.len=cap.last_len;
  radio.onMessage(m, 1000);
  CHECK(radio.state() == LINK_ATTACHED);
  radio.tick(1500);   // within timeout
  CHECK(radio.state() == LINK_ATTACHED);
  radio.tick(2100);   // > 1000ms since last rx -> detach
  CHECK(radio.state() == LINK_DETACHED);
  CHECK(radio.companionInputAllowed() == false); // buttons inert again
}

// THE key requirement: local buttons must be inert whenever not ATTACHED.
static void test_buttons_inert_when_detached() {
  LinkManager radio(ROLE_RADIO);
  // Simulate the firmware's dispatch decision for a local nav-button/scroll event.
  auto dispatchLocalKeyToCompanion = [&](uint8_t /*key*/) -> bool {
    return radio.companionInputAllowed(); // firmware gates on this
  };
  // DETACHED: no phone
  CHECK(dispatchLocalKeyToCompanion(1) == false);
  radio.onTransportUp();               // HANDSHAKING
  CHECK(dispatchLocalKeyToCompanion(1) == false);
  // Attach
  Hello ph; ph.proto_ver=PROTO_VERSION; ph.fw_ver=1; ph.role=ROLE_PHONE; ph.caps=0;
  uint8_t f[MAX_FRAME_WIRE]; size_t n=encodeHello(0,ph,false,f,sizeof(f));
  Capture cap; Decoder d; d.setCallback(onMsg,&cap); d.feed(f,n);
  Message m; m.type=cap.types[0]; m.payload=cap.last_payload; m.len=cap.last_len;
  radio.onMessage(m, 10);
  CHECK(dispatchLocalKeyToCompanion(1) == true);   // only now
  // Unplug
  radio.onTransportDown();
  CHECK(dispatchLocalKeyToCompanion(1) == false);  // inert again, no ghost-scroll
}

int main() {
  std::printf("UltraEdge protocol tests\n");
  test_crc_known_vector();
  test_roundtrip_channels();
  test_escaping();
  test_crc_rejects_corruption();
  test_resync_after_garbage();
  test_handshake_and_attach();
  test_version_mismatch_refused();
  test_heartbeat_timeout_detaches();
  test_buttons_inert_when_detached();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
