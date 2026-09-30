// UltraEdge Companion Protocol — core codec (embedded-safe C++11)
//
// Design constraints (so this same code compiles into EdgeTX firmware AND builds
// standalone on a desktop/CI):
//   * No dynamic allocation, no STL containers, no exceptions, no RTTI.
//   * Caller owns all buffers; the codec never allocates.
//   * Pure logic — no USB/OS/target dependencies. The transport (USB-CDC on the
//     radio, Android USB host on the phone) feeds bytes in and takes bytes out.
//
// See SPEC.md for the design rationale. This header is the
// authoritative wire definition for Phase 2 (structured path).
//
// SPDX-License-Identifier: GPL-3.0-or-later  (matches EdgeTX for upstream mergeability)

#ifndef ULTRAEDGE_PROTOCOL_H
#define ULTRAEDGE_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

namespace ultraedge {

// ---- Protocol version -------------------------------------------------------
// Bump PROTO_VERSION on any wire-breaking change. Handshake negotiates to the
// lowest common version; mismatched frames are dropped (never half-applied).
//   v1: flat self-describing pages (Model Setup, 8-ch Outputs)
//   v2: generalized UI engine — 16-bit page ids, LINK rows for
//       list->detail navigation, server-side conditional visibility, and an
//       ACK "restructure" hint so the app re-describes when a write changes
//       which rows are visible. Mirrors EdgeTX colorlcd model menus.
//   v3: named-value catalogs — T_REF fields carry a catalog id; the
//       radio emits Source/Switch/Curve name catalogs (its own getSourceString
//       etc.) so the phone shows real EdgeTX names in a picker, not raw indices.
static const uint8_t PROTO_VERSION = 3;

// ---- Framing constants (HDLC-style) ----------------------------------------
// Frame on the wire:  SOF  <escaped: VER TYPE SEQ LEN_L LEN_H PAYLOAD... CRC_L CRC_H>
// CRC16-CCITT is computed over the UNescaped header+payload (VER..PAYLOAD).
static const uint8_t SOF        = 0x7E; // start-of-frame flag
static const uint8_t ESC        = 0x7D; // escape byte
static const uint8_t ESC_XOR    = 0x20; // escaped byte = original ^ 0x20

static const size_t  MAX_PAYLOAD = 512;  // fits F205 USB-CDC comfortably (see rate assessment)
static const size_t  HEADER_LEN  = 5;    // VER TYPE SEQ LEN_L LEN_H
static const size_t  CRC_LEN     = 2;
// Worst case on the wire: SOF + fully-escaped (header+payload+crc)
static const size_t  MAX_FRAME_WIRE = 1 + 2 * (HEADER_LEN + MAX_PAYLOAD + CRC_LEN);

// ---- Message types ----------------------------------------------------------
enum MsgType : uint8_t {
  MSG_HELLO       = 0x01, // both directions: {proto_ver(1), fw_ver(4), role(1), caps(4)}
  MSG_HELLO_ACK   = 0x02, // reply to HELLO, same payload shape
  MSG_HEARTBEAT   = 0x03, // keepalive, empty payload

  MSG_CHANNELS    = 0x10, // radio->phone: {count(1), int16 value * count}
  MSG_TELEMETRY   = 0x11, // radio->phone: {count(1), [id(2), value(4)] * count}
  MSG_LUA_DATA    = 0x12, // radio->phone: LUA-required superset (Phase 2+: fields TBD)

  MSG_KEY         = 0x20, // phone->radio: {keycode(1), event(1)}  -- GATED on ATTACHED
  MSG_TOUCH       = 0x21, // phone->radio: TouchState mirror       -- GATED on ATTACHED

  MSG_CONFIG_READ = 0x30, // phone->radio: {u16 field_id}  request a config item
  MSG_CONFIG_WRITE= 0x31, // phone->radio: {u16 field_id, u8 type, u8 len, bytes} set -- GATED on ATTACHED
  MSG_CONFIG_VALUE= 0x32, // radio->phone: {u16 field_id, u8 type, u8 len, bytes} current value

  MSG_SUB_SET     = 0x50, // phone->radio: {u8 stream_id, u8 rate_hz} subscribe to a stream
  MSG_SUB_CLEAR   = 0x51, // phone->radio: {u8 stream_id} unsubscribe

  MSG_CONFIG_PAGES    = 0x33, // radio->phone: top-level page list {u8 count, [u16 page_id, u8 icon, str title]}  (v2: page_id u16)
  MSG_GET_PAGES       = 0x34, // phone->radio: request the top-level page list
  MSG_DESCRIBE_PAGE   = 0x35, // phone->radio: {u16 page_id} request rows for a page (v2: u16; any page incl. detail/sub-pages)
  MSG_FIELD_DESC      = 0x36, // radio->phone: one row descriptor (see FieldDesc layout below; v2 adds kind + link target)
  MSG_PAGE_DESC_END   = 0x37, // radio->phone: {u16 page_id} end-of-rows marker for a page  (v2: page_id u16)
  MSG_CONFIG_CHANGED  = 0x38, // radio->phone: {u16 field_id, u8 type, u8 len, bytes} value changed on radio (diff push)

  MSG_GET_CAPS    = 0x39, // phone->radio: request the model/hardware structure
  MSG_CAPS        = 0x3A, // radio->phone: {u8 count, [u8 key, i16 value]} value-space anchors

  MSG_AUDIO_EVENT = 0x40, // radio->phone: {event_id(2), param(2)} -- phone owns sound files

  MSG_ACK         = 0x70, // {ref_seq(1)}
  MSG_NACK        = 0x71, // {ref_seq(1), reason(1)}
};

// ---- Traffic classes (QoS over the single CDC pipe) ------------
// Class derived from message type. Firmware scheduler services 0 > 1 > 2, with a
// byte-budget cap on class 2 so streaming never starves control/config.
enum TrafficClass : uint8_t {
  CLASS_CONTROL = 0, // HELLO/ACK, HEARTBEAT, SUB_*   — tiny, highest priority
  CLASS_CONFIG  = 1, // CONFIG_*                       — reliable, ACKed, rare
  CLASS_STREAM  = 2, // CHANNELS/TELEMETRY/LUA_DATA    — best-effort, subscribed, capped
};

inline TrafficClass trafficClassOf(uint8_t type) {
  switch (type) {
    case MSG_CHANNELS: case MSG_TELEMETRY: case MSG_LUA_DATA: return CLASS_STREAM;
    case MSG_CONFIG_READ: case MSG_CONFIG_WRITE: case MSG_CONFIG_VALUE: return CLASS_CONFIG;
    default: return CLASS_CONTROL;
  }
}

// ---- Stream ids (class-2 subscriptions) ------------------------------------
enum StreamId : uint8_t {
  STREAM_CHANNELS  = 1, // live channel outputs
  STREAM_TELEMETRY = 2, // (later) telemetry sensors
  STREAM_LUA       = 3, // (later) LUA script data
};

// ---- Config field registry (typed-field access) -----
// Field ids the phone can read/write. The firmware exposes a getter + validating
// setter + a self-describing descriptor per id — this list IS the safety
// boundary (only settings, never the control path). Grows field-by-field; the
// app renders whatever the radio describes (no per-field app code).
enum ConfigField : uint16_t {
  CFG_MODEL_NAME    = 1,  // T_STR   g_model.header.name
  CFG_TIMER1_START  = 2,  // T_U16   g_model.timers[0].start (seconds)
  CFG_THR_REVERSED  = 3,  // T_BOOL  g_model.throttleReversed
  CFG_THR_TRIM      = 4,  // T_BOOL  g_model.thrTrim
  CFG_TRIM_INC      = 5,  // T_ENUM  g_model.trimInc (Expo/ExFine/Fine/Medium/Coarse)
  CFG_EXTENDED_TRIMS= 6,  // T_BOOL  g_model.extendedTrims
  CFG_DISPLAY_TRIMS = 7,  // T_ENUM  g_model.displayTrims (No/Change/Yes)
  CFG_TIMER1_MODE   = 8,  // T_ENUM  timers[0].mode (OFF/ON/Strt/THs/TH%/THt)
  CFG_TIMER1_PERSIST= 9,  // T_ENUM  timers[0].persistent (OFF/Flight/Manual)
  CFG_TIMER1_MINBEEP=10,  // T_BOOL  timers[0].minuteBeep
  CFG_TIMER2_MODE   =11,  // T_ENUM  timers[1].mode
  CFG_TIMER2_START  =12,  // T_U16   timers[1].start
  CFG_TIMER3_MODE   =13,  // T_ENUM  timers[2].mode
  CFG_TIMER3_START  =14,  // T_U16   timers[2].start
  CFG_THR_WARNING   =15,  // T_BOOL  !disableThrottleWarning
  CFG_JITTER_FILTER =16,  // T_ENUM  jitterFilter (Global/Off/On)

  // ---- Outputs --------------------------------------------------
  // Per-channel detail field id = CFG_OUT_BASE | (channel<<4) | OutSub.
  // Channel list-row (summary/link) id  = CFG_OUTROW_BASE | channel.
  CFG_OUT_BASE      = 0x1000,
  CFG_OUTROW_BASE   = 0x1F00,

  // ---- Timers ---------------------------------------------------
  // Per-timer detail field id = CFG_TMR_BASE | (timer<<4) | TmrSub.
  // Timer list-row (summary/link) id = CFG_TMRROW_BASE | timer.
  CFG_TMR_BASE      = 0x2000,
  CFG_TMRROW_BASE   = 0x2F00,

  // Model Setup section LINK rows (Trims / Throttle / Other) id = base | section.
  CFG_SECROW_BASE   = 0x3000,

  // ---- Per-item subsystems --------------------------------------
  // Each item occupies ITEM_STRIDE ids: detail field id = <BASE> + item*STRIDE + sub.
  // The list-page LINK/summary row for item i uses sub = SUB_SUMMARY.
  CFG_INPUT_BASE    = 0x4000,   // Inputs   (MAX_EXPOS)
  CFG_MIX_BASE      = 0x4800,   // Mixes    (MAX_MIXERS)
  CFG_CURVE_BASE    = 0x5000,   // Curves   (MAX_CURVES)
  CFG_LS_BASE       = 0x5800,   // Logical switches (MAX_LOGICAL_SWITCHES)
  CFG_SF_BASE       = 0x6000,   // Special functions (MAX_SPECIAL_FUNCTIONS)
  CFG_GVAR_BASE     = 0x6800,   // Global variables (MAX_GVARS)
  CFG_FM_BASE       = 0x7000,   // Flight modes (MAX_FLIGHT_MODES)
  CFG_TELEM_BASE    = 0x8000,   // Telemetry sensors (MAX_TELEMETRY_SENSORS; wider span)
};
// Model Setup section rows (each opens a sub-page of existing model fields).
enum SecRow : uint8_t { SEC_TRIMS = 0, SEC_THROTTLE = 1, SEC_OTHER = 2 };

// Per-item id layout.
static const uint16_t ITEM_STRIDE = 0x20;   // ids reserved per list item
static const uint8_t  SUB_SUMMARY = 0x1F;   // list-row summary/link sub-id

// Subfields within an item's detail page (per subsystem).
enum InputSub : uint8_t { IN_NAME=0, IN_WEIGHT=1, IN_OFFSET=2, IN_SWITCH=3, IN_CURVE=4, IN_SRC=5, IN_LINE_CH=6 };
enum MixSub   : uint8_t { MX_NAME=0, MX_WEIGHT=1, MX_OFFSET=2, MX_SWITCH=3, MX_MULT=4, MX_SRC=5, MX_DESTCH=6, MX_CURVE=7 };
enum CurveSub : uint8_t { CV_NAME=0, CV_TYPE=1, CV_SMOOTH=2, CV_POINTS=3 };
enum LsSub    : uint8_t { LS_FUNC=0, LS_V1=1, LS_V2=2, LS_V3=3, LS_AND=4, LS_DELAY=5, LS_DURATION=6 };
enum SfSub    : uint8_t { SF_SWITCH=0, SF_FUNC=1, SF_PARAM=2, SF_REPEAT=3, SF_ENABLE=4 };
enum GvSub    : uint8_t { GV_NAME=0, GV_MIN=1, GV_MAX=2, GV_POPUP=3, GV_PREC=4, GV_UNIT=5 };
enum FmSub    : uint8_t { FM_NAME=0, FM_SWITCH=1, FM_FADEIN=2, FM_FADEOUT=3 };
enum TeSub    : uint8_t { TE_LABEL=0, TE_UNIT=1, TE_PREC=2 };
// Output detail subfields (mirror EdgeTX output_edit.cpp).
enum OutSub : uint8_t {
  OUT_REVERSE = 0, OUT_SUBTRIM = 1, OUT_MIN = 2, OUT_MAX = 3,
  OUT_NAME = 4, OUT_CURVE = 5, OUT_PPMCENTER = 6, OUT_SUBTRIMMODE = 7,
};
// Timer detail subfields (mirror EdgeTX timer_setup.cpp).
enum TmrSub : uint8_t {
  TMR_NAME = 0, TMR_MODE = 1, TMR_START = 2, TMR_MINBEEP = 3,
  TMR_COUNTDOWN = 4, TMR_PERSIST = 5, TMR_DIR = 6,
};
static const uint8_t OUT_CHANNELS = 32;   // real output channels (MAX_OUTPUT_CHANNELS)
static const uint8_t NUM_TIMERS   = 3;

// Config value types. T_LINK (v2) is a navigation row, not an editable value.
enum ConfigType : uint8_t {
  T_U8 = 1, T_I8 = 2, T_U16 = 3, T_I16 = 4, T_I32 = 5, T_STR = 6,
  T_BOOL = 7, T_ENUM = 8, T_LINK = 9,
  T_REF = 10,   // reference to a source/switch/curve — value is the raw index;
                // the APP names it from CAPS anchors, so a future
                // stringless MCU needs no name tables. Descriptor carries a domain.
};

// T_REF domains: which name space the value indexes. The app owns the naming.
enum RefDomain : uint8_t { DOM_SOURCE = 1, DOM_SWITCH = 2, DOM_CURVE = 3 };

// CAPS keys (MSG_CAPS): value-space anchors the app names from. Each is
// a base index or a count; the app generates "CH3" / "SA↑" / "L2" / "CV3" etc.
// from these. A future MCU with its own value space sends its own anchors — no
// strings ever cross the wire.
enum CapKey : uint8_t {
  CAP_SRC_NONE=1, CAP_SRC_INPUT=2, CAP_N_INPUT=3, CAP_SRC_STICK=4, CAP_N_STICK=5,
  CAP_SRC_POT=6, CAP_N_POT=7, CAP_SRC_MAX=8, CAP_SRC_TRIM=9, CAP_N_TRIM=10,
  CAP_SRC_SW=11, CAP_N_SW=12, CAP_SRC_LS=13, CAP_N_LS=14, CAP_SRC_CH=15,
  CAP_N_CH=16, CAP_SRC_GV=17, CAP_N_GV=18, CAP_SRC_TELE=19, CAP_N_TELE=20,
  CAP_SRC_LAST=21,
  CAP_SW_SW=30, CAP_SW_TRIM=31, CAP_SW_LS=32, CAP_SW_ON=33, CAP_SW_LAST=34,
  CAP_N_CURVE=40,
};

// Row kind (v2 FieldDesc). FIELD = editable widget; LINK = opens target page.
enum RowKind : uint8_t { ROW_FIELD = 0, ROW_LINK = 1 };

// Icon ids (v2). Mirror EdgeTX colorlcd icons (radio/src/bitmaps/800x480 masks);
// the app carries the matching tinted glyphs. 0 = none.
enum IconId : uint8_t {
  IC_NONE = 0, IC_SETUP, IC_OUTPUTS, IC_TIMERS, IC_INPUTS, IC_MIXER,
  IC_CURVES, IC_LS, IC_SF, IC_TELEM, IC_GVARS, IC_FLIGHTMODES,
  IC_GENERAL, IC_THROTTLE, IC_TRIMS, IC_MONITOR, IC_CHANNEL,
};

// ---- Config pages (16-bit; mirrors EdgeTX colorlcd model menus) ----
// Top-level pages appear in GET_PAGES and become tabs. Detail/sub pages are
// reached by tapping a LINK row and are addressed by base+index.
enum ConfigPage : uint16_t {
  PAGE_MODEL_SETUP = 0x0001,  // form: fields + timer/section LINK rows
  PAGE_OUTPUTS     = 0x0002,  // list: 32 channel LINK rows -> output detail
  PAGE_INPUTS      = 0x0003,  // list of input lines -> input detail
  PAGE_MIXES       = 0x0004,  // list of mix lines -> mix detail
  PAGE_CURVES      = 0x0005,  // list of curves -> curve detail
  PAGE_LS          = 0x0006,  // list of logical switches -> LS detail
  PAGE_SF          = 0x0007,  // list of special functions -> SF detail
  PAGE_GVARS       = 0x0008,  // list of global variables -> GVar detail
  PAGE_FMODES      = 0x0009,  // list of flight modes -> FM detail
  PAGE_TELEM       = 0x000A,  // list of telemetry sensors -> sensor detail

  PAGE_TIMER_BASE  = 0x0100,  // + timer(0..2): timer detail form
  PAGE_OUT_BASE    = 0x0200,  // + channel(0..31): output detail form
  PAGE_TRIMS       = 0x0300,  // sub-page: trim increment / display / extended
  PAGE_THROTTLE    = 0x0400,  // sub-page: reversed / trim / warning
  PAGE_OTHER       = 0x0500,  // sub-page: ADC jitter filter (+ future)
  PAGE_INPUT_BASE  = 0x0600,  // + input line: input detail form
  PAGE_MIX_BASE    = 0x0700,  // + mix line:   mix detail form
  PAGE_CURVE_BASE  = 0x0800,  // + curve:      curve detail form
  PAGE_LS_BASE     = 0x0900,  // + logical switch: LS detail form
  PAGE_SF_BASE     = 0x0A00,  // + special function: SF detail form
  PAGE_GVAR_BASE   = 0x0B00,  // + gvar:       GVar detail form
  PAGE_FM_BASE     = 0x0C00,  // + flight mode: FM detail form
  PAGE_TELEM_BASE  = 0x0D00,  // + sensor:     telemetry sensor detail form
};

// FieldDesc wire layout v2 (MSG_FIELD_DESC payload):
//   u16 field_id, u16 page_id, u8 kind, u8 type, u8 icon,
//   i32 min, i32 max, i32 step,
//   u8 label_len, char label[],
//   u8 unit_len,  char unit[],
//   u8 opt_count, [u8 opt_len, char opt[]] * opt_count            (T_ENUM)
//   if kind==ROW_LINK: u16 target_page_id                          (navigation)
//   if type==T_REF:    u8 domain (RefDomain)                       (named picker)
// The app renders by kind then type: LINK=tappable summary row (value carries a
// T_STR summary); FIELD: STR=text, U*/I*=stepper, BOOL=toggle, ENUM=dropdown.
// Conditional visibility is server-side: the radio only emits rows currently
// visible (EdgeTX isVisible() semantics). After a write that changes structure
// the ACK sets ACK_RESTRUCTURE and the app re-describes the current page.
static const uint8_t ACK_RESTRUCTURE = 0x01;  // ACK payload byte 2, bit0

// NACK reason codes (payload byte 2 of MSG_NACK).
enum NackReason : uint8_t {
  NACK_UNKNOWN_FIELD = 1, NACK_BAD_TYPE = 2, NACK_BAD_LENGTH = 3,
  NACK_OUT_OF_RANGE = 4,  NACK_NOT_ATTACHED = 5,
};

// ---- Roles & capability bits ------------------------------------------------
enum Role : uint8_t { ROLE_RADIO = 0, ROLE_PHONE = 1 };

enum Caps : uint32_t {
  CAP_STRUCTURED   = 1u << 0, // S1/S3 structured-state channel (MVP baseline)
  CAP_DISPLAY_SYNC = 1u << 1, // S2 raw display/touch mirror (future, no protocol break)
  CAP_LUA_FEED     = 1u << 2, // can produce/consume LUA_DATA
  CAP_AUDIO        = 1u << 3, // phone renders AUDIO_EVENT from its own sound files
};

// ---- Link state machine -----------------------------------------------------
// The hard requirement: when no phone is attached, the radio's physical nav
// buttons / scroll-wheel must NOT drive the companion config surface. Gating is
// centralised in companionInputAllowed(): callers check it before dispatching
// local KEY/TOUCH events to the companion GUI layer. This never touches the
// flight-control path — control runs regardless of link state.
enum LinkState : uint8_t {
  LINK_DETACHED    = 0, // no companion; buttons inert for config; radio flies normally
  LINK_HANDSHAKING = 1, // USB up, HELLO in progress; still inert for config
  LINK_ATTACHED    = 2, // handshake done + heartbeat alive; companion I/O active
};

// ---- Decoded message view (points into caller's buffer; no copy) ------------
struct Message {
  uint8_t  type;      // MsgType
  uint8_t  seq;
  const uint8_t* payload;
  uint16_t len;
};

// ---- HELLO payload helper ---------------------------------------------------
struct Hello {
  uint8_t  proto_ver;
  uint32_t fw_ver;
  uint8_t  role;
  uint32_t caps;
};

// ---- CRC16-CCITT (poly 0x1021, init 0xFFFF) --------------------------------
uint16_t crc16(const uint8_t* data, size_t len);

// ---- Encoding ---------------------------------------------------------------
// Encodes one frame (type+seq+payload) into `out` (size MAX_FRAME_WIRE).
// Returns the number of bytes written, or 0 on error (payload too big / null).
size_t encodeFrame(uint8_t type, uint8_t seq,
                   const uint8_t* payload, uint16_t payload_len,
                   uint8_t* out, size_t out_cap);

// Convenience encoders for common messages. Return frame length in `out`, or 0.
size_t encodeHello(uint8_t seq, const Hello& h, bool ack, uint8_t* out, size_t out_cap);
size_t encodeHeartbeat(uint8_t seq, uint8_t* out, size_t out_cap);
size_t encodeChannels(uint8_t seq, const int16_t* ch, uint8_t count,
                      uint8_t* out, size_t out_cap);
size_t encodeKey(uint8_t seq, uint8_t keycode, uint8_t event,
                 uint8_t* out, size_t out_cap);

// ---- Streaming decoder ------------------------------------------------------
// Feed received bytes; whenever a complete, CRC-valid, version-accepted frame is
// assembled, `onMessage` is invoked. Corrupt/oversize/out-of-version frames are
// dropped and the decoder resyncs on the next SOF. Zero allocation.
class Decoder {
 public:
  typedef void (*Callback)(const Message& msg, void* user);

  Decoder() { reset(); }
  void reset();

  void setCallback(Callback cb, void* user) { cb_ = cb; user_ = user; }

  // Push a block of received bytes through the decoder.
  void feed(const uint8_t* data, size_t len);

  // Stats (useful for tests + field diagnostics).
  uint32_t framesOk()      const { return frames_ok_; }
  uint32_t framesCrcErr()  const { return frames_crc_err_; }
  uint32_t framesVerErr()  const { return frames_ver_err_; }
  uint32_t framesOversize()const { return frames_oversize_; }

 private:
  void pushByte(uint8_t b);
  void finishFrame();

  bool     in_frame_;
  bool     escaping_;
  uint8_t  buf_[HEADER_LEN + MAX_PAYLOAD + CRC_LEN];
  size_t   buf_len_;
  Callback cb_;
  void*    user_;
  uint32_t frames_ok_, frames_crc_err_, frames_ver_err_, frames_oversize_;
};

// ---- Helpers to parse known payloads ---------------------------------------
bool parseHello(const Message& m, Hello& out);
// Parses CHANNELS payload into caller array; returns channel count (0 on error).
uint8_t parseChannels(const Message& m, int16_t* out, uint8_t max_count);

// ---- Link manager -----------------------------------------------------------
// Tracks link state from handshake + heartbeat, and exposes the gating decision.
// Time is supplied by the caller (monotonic ms) so this stays OS-independent.
class LinkManager {
 public:
  explicit LinkManager(uint8_t self_role)
    : role_(self_role), state_(LINK_DETACHED),
      last_rx_ms_(0), heartbeat_timeout_ms_(1000) {}

  LinkState state() const { return state_; }

  // Central gating decision for the button-inert requirement.
  // True only when a companion is actually ATTACHED.
  bool companionInputAllowed() const { return state_ == LINK_ATTACHED; }

  // Call when the USB transport connects / disconnects.
  void onTransportUp()   { if (state_ == LINK_DETACHED) state_ = LINK_HANDSHAKING; }
  void onTransportDown() { state_ = LINK_DETACHED; }

  // Call for every successfully decoded inbound message, with current time.
  void onMessage(const Message& m, uint32_t now_ms);

  // Call periodically; drops to DETACHED if heartbeat lapses.
  void tick(uint32_t now_ms);

  void setHeartbeatTimeout(uint32_t ms) { heartbeat_timeout_ms_ = ms; }
  const Hello& peerHello() const { return peer_; }

 private:
  uint8_t   role_;
  LinkState state_;
  uint32_t  last_rx_ms_;
  uint32_t  heartbeat_timeout_ms_;
  Hello     peer_ = {0,0,0,0};
};

} // namespace ultraedge

#endif // ULTRAEDGE_PROTOCOL_H
