// UltraEdge Companion Protocol — core codec implementation (embedded-safe C++11)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ultraedge/protocol.h"

namespace ultraedge {

// ---- CRC16-CCITT (0x1021, init 0xFFFF) -------------------------------------
uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; ++b) {
      if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
      else              crc = (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// ---- byte-stuffing writer ---------------------------------------------------
static inline bool putEscaped(uint8_t b, uint8_t* out, size_t out_cap, size_t& n) {
  if (b == SOF || b == ESC) {
    if (n + 2 > out_cap) return false;
    out[n++] = ESC;
    out[n++] = (uint8_t)(b ^ ESC_XOR);
  } else {
    if (n + 1 > out_cap) return false;
    out[n++] = b;
  }
  return true;
}

// ---- Encoding ---------------------------------------------------------------
size_t encodeFrame(uint8_t type, uint8_t seq,
                   const uint8_t* payload, uint16_t payload_len,
                   uint8_t* out, size_t out_cap) {
  if (!out || out_cap < 1) return 0;
  if (payload_len > MAX_PAYLOAD) return 0;
  if (payload_len > 0 && !payload) return 0;

  // Build the unescaped header+payload to CRC over.
  uint8_t hdr[HEADER_LEN];
  hdr[0] = PROTO_VERSION;
  hdr[1] = type;
  hdr[2] = seq;
  hdr[3] = (uint8_t)(payload_len & 0xFF);
  hdr[4] = (uint8_t)(payload_len >> 8);

  // CRC16-CCITT over header + payload, one running pass (no temp buffer).
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < HEADER_LEN; ++i) {
    crc ^= (uint16_t)hdr[i] << 8;
    for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? (uint16_t)((crc<<1)^0x1021) : (uint16_t)(crc<<1);
  }
  for (uint16_t i = 0; i < payload_len; ++i) {
    crc ^= (uint16_t)payload[i] << 8;
    for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? (uint16_t)((crc<<1)^0x1021) : (uint16_t)(crc<<1);
  }

  size_t n = 0;
  out[n++] = SOF;
  for (size_t i = 0; i < HEADER_LEN; ++i) if (!putEscaped(hdr[i], out, out_cap, n)) return 0;
  for (uint16_t i = 0; i < payload_len; ++i) if (!putEscaped(payload[i], out, out_cap, n)) return 0;
  if (!putEscaped((uint8_t)(crc & 0xFF), out, out_cap, n)) return 0;
  if (!putEscaped((uint8_t)(crc >> 8),  out, out_cap, n)) return 0;
  return n;
}

size_t encodeHeartbeat(uint8_t seq, uint8_t* out, size_t out_cap) {
  return encodeFrame(MSG_HEARTBEAT, seq, nullptr, 0, out, out_cap);
}

size_t encodeHello(uint8_t seq, const Hello& h, bool ack, uint8_t* out, size_t out_cap) {
  uint8_t q[10];
  q[0]=h.proto_ver;
  q[1]=(uint8_t)(h.fw_ver); q[2]=(uint8_t)(h.fw_ver>>8); q[3]=(uint8_t)(h.fw_ver>>16); q[4]=(uint8_t)(h.fw_ver>>24);
  q[5]=h.role;
  q[6]=(uint8_t)(h.caps); q[7]=(uint8_t)(h.caps>>8); q[8]=(uint8_t)(h.caps>>16); q[9]=(uint8_t)(h.caps>>24);
  return encodeFrame(ack ? MSG_HELLO_ACK : MSG_HELLO, seq, q, sizeof(q), out, out_cap);
}

size_t encodeChannels(uint8_t seq, const int16_t* ch, uint8_t count,
                      uint8_t* out, size_t out_cap) {
  if (!ch && count) return 0;
  uint8_t p[1 + 2*255];
  p[0] = count;
  for (uint8_t i = 0; i < count; ++i) {
    p[1 + 2*i]     = (uint8_t)(ch[i] & 0xFF);
    p[1 + 2*i + 1] = (uint8_t)((uint16_t)ch[i] >> 8);
  }
  return encodeFrame(MSG_CHANNELS, seq, p, (uint16_t)(1 + 2*count), out, out_cap);
}

size_t encodeKey(uint8_t seq, uint8_t keycode, uint8_t event,
                 uint8_t* out, size_t out_cap) {
  uint8_t p[2] = { keycode, event };
  return encodeFrame(MSG_KEY, seq, p, 2, out, out_cap);
}

// ---- Decoder ----------------------------------------------------------------
void Decoder::reset() {
  in_frame_ = false; escaping_ = false; buf_len_ = 0;
  cb_ = nullptr; user_ = nullptr;
  frames_ok_ = frames_crc_err_ = frames_ver_err_ = frames_oversize_ = 0;
}

void Decoder::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) pushByte(data[i]);
}

void Decoder::pushByte(uint8_t b) {
  if (b == SOF) {
    // Start (or restart) a frame. Any partial frame is abandoned (resync).
    in_frame_ = true; escaping_ = false; buf_len_ = 0;
    return;
  }
  if (!in_frame_) return; // junk before first SOF

  if (escaping_) {
    b = (uint8_t)(b ^ ESC_XOR);
    escaping_ = false;
  } else if (b == ESC) {
    escaping_ = true;
    return;
  }

  if (buf_len_ >= sizeof(buf_)) {
    // Oversize — drop and wait for next SOF.
    frames_oversize_++;
    in_frame_ = false; buf_len_ = 0;
    return;
  }
  buf_[buf_len_++] = b;

  // Do we have a full frame yet? We only know once we see the next SOF, so
  // instead we validate opportunistically: a frame is complete when we have at
  // least HEADER_LEN and buf_len_ == HEADER_LEN + declared_len + CRC_LEN.
  if (buf_len_ >= HEADER_LEN) {
    uint16_t declared = (uint16_t)buf_[3] | ((uint16_t)buf_[4] << 8);
    if (declared <= MAX_PAYLOAD) {
      size_t need = HEADER_LEN + declared + CRC_LEN;
      if (buf_len_ == need) finishFrame();
    } else {
      // impossible length — resync
      in_frame_ = false; buf_len_ = 0;
    }
  }
}

void Decoder::finishFrame() {
  // Layout in buf_: VER TYPE SEQ LEN_L LEN_H PAYLOAD... CRC_L CRC_H
  uint16_t declared = (uint16_t)buf_[3] | ((uint16_t)buf_[4] << 8);
  size_t crc_off = HEADER_LEN + declared;
  uint16_t rx_crc = (uint16_t)buf_[crc_off] | ((uint16_t)buf_[crc_off+1] << 8);
  uint16_t calc = crc16(buf_, HEADER_LEN + declared);

  in_frame_ = false; // frame consumed either way

  if (rx_crc != calc) { frames_crc_err_++; buf_len_ = 0; return; }
  if (buf_[0] != PROTO_VERSION) { frames_ver_err_++; buf_len_ = 0; return; }

  frames_ok_++;
  if (cb_) {
    Message m;
    m.type = buf_[1];
    m.seq  = buf_[2];
    m.payload = declared ? &buf_[HEADER_LEN] : nullptr;
    m.len  = declared;
    cb_(m, user_);
  }
  buf_len_ = 0;
}

// ---- Payload parsers --------------------------------------------------------
bool parseHello(const Message& m, Hello& out) {
  if ((m.type != MSG_HELLO && m.type != MSG_HELLO_ACK) || m.len < 10) return false;
  const uint8_t* p = m.payload;
  out.proto_ver = p[0];
  out.fw_ver = (uint32_t)p[1] | ((uint32_t)p[2]<<8) | ((uint32_t)p[3]<<16) | ((uint32_t)p[4]<<24);
  out.role = p[5];
  out.caps = (uint32_t)p[6] | ((uint32_t)p[7]<<8) | ((uint32_t)p[8]<<16) | ((uint32_t)p[9]<<24);
  return true;
}

uint8_t parseChannels(const Message& m, int16_t* out, uint8_t max_count) {
  if (m.type != MSG_CHANNELS || m.len < 1) return 0;
  uint8_t count = m.payload[0];
  if ((size_t)(1 + 2*count) > m.len) return 0;
  if (count > max_count) count = max_count;
  for (uint8_t i = 0; i < count; ++i) {
    out[i] = (int16_t)((uint16_t)m.payload[1+2*i] | ((uint16_t)m.payload[1+2*i+1] << 8));
  }
  return count;
}

// ---- LinkManager ------------------------------------------------------------
void LinkManager::onMessage(const Message& m, uint32_t now_ms) {
  last_rx_ms_ = now_ms;

  if (m.type == MSG_HELLO || m.type == MSG_HELLO_ACK) {
    Hello h;
    if (parseHello(m, h)) {
      // Accept only a matching major protocol version.
      if (h.proto_ver == PROTO_VERSION) {
        peer_ = h;
        state_ = LINK_ATTACHED;
      } else {
        // Version mismatch: refuse companion features; stay pre-ATTACHED.
        if (state_ == LINK_ATTACHED) state_ = LINK_HANDSHAKING;
      }
    }
    return;
  }

  // Any valid inbound traffic while handshook keeps us attached; but we only
  // ENTER attached via a successful HELLO exchange above.
  if (state_ == LINK_HANDSHAKING) {
    // stay handshaking until HELLO seen
  }
}

void LinkManager::tick(uint32_t now_ms) {
  if (state_ == LINK_ATTACHED) {
    if (now_ms - last_rx_ms_ > heartbeat_timeout_ms_) {
      state_ = LINK_DETACHED; // heartbeat lapsed -> disable companion I/O
    }
  }
}

} // namespace ultraedge
