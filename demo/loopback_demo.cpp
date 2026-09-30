// UltraEdge protocol — host loopback demo.
// Simulates a radio and a phone exchanging frames over an in-memory "wire",
// demonstrating: HELLO handshake -> ATTACHED, CHANNELS streaming radio->phone,
// KEY events phone->radio, and the button-gating link state.
//
// Build: g++ -std=c++11 -I../include ../src/protocol.cpp loopback_demo.cpp -o demo
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ultraedge/protocol.h"
#include <cstdio>

using namespace ultraedge;

static int g_phone_channels_rx = 0;
static int g_radio_keys_rx = 0;

// An "endpoint" bundles a decoder + a link manager and routes decoded frames
// into both. This mirrors how the firmware and the app each wire the codec.
struct Endpoint {
  Decoder dec;
  LinkManager link;
  uint32_t now = 0;
  explicit Endpoint(uint8_t role) : link(role) {}
  void receive(const uint8_t* data, size_t n) { dec.feed(data, n); }
};

static void phoneOnMsg(const Message& m, void* user) {
  Endpoint* ep = (Endpoint*)user;
  ep->link.onMessage(m, ep->now);
  if (m.type == MSG_CHANNELS) {
    int16_t ch[16]; uint8_t c = parseChannels(m, ch, 16);
    g_phone_channels_rx++;
    std::printf("  [phone] CHANNELS seq=%u count=%u  ch0=%d ch1=%d\n", m.seq, c, ch[0], ch[1]);
  } else if (m.type == MSG_HELLO || m.type == MSG_HELLO_ACK) {
    std::printf("  [phone] handshake frame from radio -> state=%d\n", ep->link.state());
  }
}
static void radioOnMsg(const Message& m, void* user) {
  Endpoint* ep = (Endpoint*)user;
  ep->link.onMessage(m, ep->now);
  if (m.type == MSG_KEY) {
    // Firmware would gate here: only act if companion attached.
    bool allowed = ep->link.companionInputAllowed();
    if (allowed) { g_radio_keys_rx++;
      std::printf("  [radio] KEY keycode=%u event=%u (accepted: attached)\n", m.payload[0], m.payload[1]);
    } else {
      std::printf("  [radio] KEY dropped (not attached)\n");
    }
  } else if (m.type == MSG_HELLO || m.type == MSG_HELLO_ACK) {
    std::printf("  [radio] handshake frame from phone -> state=%d\n", ep->link.state());
  }
}

int main() {
  std::printf("UltraEdge loopback demo\n\n");

  Endpoint radio(ROLE_RADIO), phone(ROLE_PHONE);
  radio.dec.setCallback(radioOnMsg, &radio);
  phone.dec.setCallback(phoneOnMsg, &phone);
  uint8_t f[MAX_FRAME_WIRE]; size_t n;

  std::printf("1) USB connects -> both sides HANDSHAKING\n");
  radio.link.onTransportUp(); phone.link.onTransportUp();
  std::printf("   radio.companionInputAllowed=%d (expect 0: buttons inert)\n\n",
              radio.link.companionInputAllowed());

  std::printf("2) HELLO handshake\n");
  radio.now = phone.now = 10;
  Hello rh{PROTO_VERSION, 0x00020000, ROLE_RADIO, CAP_STRUCTURED|CAP_AUDIO|CAP_LUA_FEED};
  Hello ph{PROTO_VERSION, 0x00000100, ROLE_PHONE, CAP_STRUCTURED|CAP_AUDIO};
  n = encodeHello(0, rh, false, f, sizeof(f)); phone.receive(f, n); // radio's HELLO -> phone
  n = encodeHello(0, ph, false, f, sizeof(f)); radio.receive(f, n); // phone's HELLO -> radio
  std::printf("   radio.state=%d phone.state=%d (2==ATTACHED)\n", radio.link.state(), phone.link.state());
  std::printf("   radio.companionInputAllowed=%d (expect 1)\n\n", radio.link.companionInputAllowed());

  std::printf("3) Radio streams CHANNELS to phone\n");
  int16_t ch[8] = {0, 512, -512, 1024, 0, 0, 250, -250};
  for (int i = 0; i < 3; ++i) {
    ch[0] = (int16_t)(i * 100);
    n = encodeChannels((uint8_t)i, ch, 8, f, sizeof(f));
    phone.receive(f, n);
  }
  std::printf("\n");

  std::printf("4) Phone sends a KEY (nav) event; radio accepts it (attached)\n");
  n = encodeKey(0, /*keycode*/5, /*event*/1, f, sizeof(f));
  radio.receive(f, n);
  std::printf("\n");

  std::printf("5) USB drops -> radio DETACHED; a late KEY must be ignored\n");
  radio.link.onTransportDown();
  n = encodeKey(1, 5, 1, f, sizeof(f));
  radio.receive(f, n); // should be dropped by the gate
  std::printf("   radio.companionInputAllowed=%d (expect 0)\n\n", radio.link.companionInputAllowed());

  std::printf("Summary: phone got %d CHANNELS frames, radio accepted %d KEY frames.\n",
              g_phone_channels_rx, g_radio_keys_rx);
  bool ok = (g_phone_channels_rx == 3) && (g_radio_keys_rx == 1)
            && !radio.link.companionInputAllowed();
  std::printf("DEMO %s\n", ok ? "OK" : "FAILED");
  return ok ? 0 : 1;
}
