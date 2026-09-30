// Ring-overflow cross-check: proves the paced DESCRIBE_PAGE generator
// survives the real USB-CDC TX ring, and that the old synchronous blast does not.
//
// It models usbSerialPutc EXACTLY: a fixed ring whose write pointer advances
// unconditionally (no free-space check), so writing faster than the USB IN
// endpoint drains overwrites unsent bytes. Frames are built with the REAL
// protocol encoder (real framing + CRC), fed through the ring, drained at USB
// Full-Speed, and decoded. We count how many whole frames survive.
#include "ultraedge/protocol.h"
#include <cstdio>
#include <vector>
#include <cstring>
using namespace ultraedge;

// ---- Faithful model of the CDC TX ring (UserTxBufferFS) ----
static const int RING = 512;          // APP_TX_DATA_SIZE (bootloader size = worst case)
static uint8_t  ringBuf[RING];
static int      rHead = 0, rTail = 0, rCount = 0;
static long     overwritten = 0;      // bytes destroyed by overflow (the bug's fingerprint)

static void ringReset() { rHead = rTail = rCount = 0; overwritten = 0; }

// usbSerialPutc semantics: advance head unconditionally. If the ring is full the
// oldest unsent byte is clobbered (faithful to head lapping tail).
static void ringPutc(uint8_t b) {
  ringBuf[rHead] = b; rHead = (rHead + 1) % RING;
  if (rCount < RING) rCount++;
  else { rTail = (rTail + 1) % RING; overwritten++; }   // overflow: lose oldest
}
static int ringFree() { return RING - rCount; }

// USB IN drain: move up to n buffered bytes out to the host decoder, in order.
template <class F>
static void ringDrain(int n, F sink) {
  while (n-- > 0 && rCount > 0) {
    uint8_t b = ringBuf[rTail]; rTail = (rTail + 1) % RING; rCount--;
    sink(b);
  }
}

// ---- Build the describe frame sequence with the REAL encoder ----
// Representative payload sizes: enum FIELD_DESC ~ big (options), value ~ small.
static std::vector<std::vector<uint8_t>> buildPage(int nFields, bool outputs) {
  std::vector<std::vector<uint8_t>> frames;
  uint8_t seq = 0;
  uint8_t f[MAX_FRAME_WIRE];
  for (int k = 0; k < nFields; ++k) {
    // FIELD_DESC payload (sizes mirror buildFieldDesc: id,page,type,3×i32,label,unit,opts)
    uint8_t p[160]; size_t o = 0;
    uint16_t id = outputs ? (uint16_t)(CFG_OUT_BASE | ((k/4)<<4) | (k%4)) : (uint16_t)(k+1);
    p[o++]=id&0xFF; p[o++]=id>>8; p[o++]= outputs?PAGE_OUTPUTS:PAGE_MODEL_SETUP; p[o++]=T_ENUM;
    for(int i=0;i<12;i++) p[o++]=0;                 // 3× int32 min/max/step
    const char* lbl = "Timer 1 minute beep"; uint8_t ln=(uint8_t)strlen(lbl);
    p[o++]=ln; memcpy(p+o,lbl,ln); o+=ln;
    p[o++]=0;                                        // unit ""
    if (!outputs) { p[o++]=6; const char* op="THs"; for(int i=0;i<6;i++){p[o++]=3;memcpy(p+o,op,3);o+=3;} }
    else p[o++]=0;                                   // outputs: no options
    size_t n = encodeFrame(MSG_FIELD_DESC, seq++, p, (uint16_t)o, f, sizeof(f));
    frames.emplace_back(f, f+n);
    // CONFIG_VALUE payload: id,type,len,bytes
    uint8_t v[8]; v[0]=id&0xFF; v[1]=id>>8; v[2]=T_I16; v[3]=2; v[4]=0x10; v[5]=0x00;
    n = encodeFrame(MSG_CONFIG_VALUE, seq++, v, 6, f, sizeof(f));
    frames.emplace_back(f, f+n);
  }
  uint8_t e[1] = { outputs?PAGE_OUTPUTS:PAGE_MODEL_SETUP };
  size_t n = encodeFrame(MSG_PAGE_DESC_END, seq++, e, 1, f, sizeof(f));
  frames.emplace_back(f, f+n);
  return frames;
}

struct Counts { int desc=0, val=0, end=0, crcErr=0; int total() const { return desc+val+end; } };

static Counts runThroughRing(const std::vector<std::vector<uint8_t>>& frames, bool paced) {
  ringReset();
  Counts c;
  Decoder dec;
  dec.setCallback([](const Message& m, void* u){
    Counts* cc=(Counts*)u;
    if(m.type==MSG_FIELD_DESC) cc->desc++;
    else if(m.type==MSG_CONFIG_VALUE) cc->val++;
    else if(m.type==MSG_PAGE_DESC_END) cc->end++;
  }, &c);
  auto sink = [&](uint8_t b){ dec.feed(&b,1); };

  const int DRAIN = 640;   // USB FS: ~64 B/ms × 10 ms tick
  size_t fi = 0;
  size_t bi = 0;

  if (!paced) {
    // OLD: blast every frame's bytes in one tick BEFORE any USB drain.
    for (auto& fr : frames) for (uint8_t b : fr) ringPutc(b);
    // then drain over many ticks
    for (int t=0; t<200 && rCount>0; ++t) ringDrain(DRAIN, sink);
  } else {
    // NEW: paced pump — each tick drain first, then emit whole frames only while
    // the ring has room (freeSpace >= framelen + 64), max 8 frames/tick.
    for (int t=0; t<400; ++t) {
      ringDrain(DRAIN, sink);
      for (int g=0; g<8 && fi<frames.size(); ++g) {
        int len = (int)frames[fi].size();
        if (ringFree() >= len + 64) { for (uint8_t b: frames[fi]) ringPutc(b); fi++; }
        else break;
      }
      if (fi>=frames.size() && rCount==0) { ringDrain(DRAIN, sink); break; }
    }
    // final flush
    for (int t=0;t<200 && rCount>0;++t) ringDrain(DRAIN, sink);
  }
  c.crcErr = (int)dec.framesCrcErr();
  return c;
}

int main() {
  struct P { const char* name; int fields; bool out; } pages[] = {
    { "Model Setup", 16, false }, { "Outputs", 32, true } };
  bool allOk = true;
  for (auto& pg : pages) {
    auto frames = buildPage(pg.fields, pg.out);
    int wantDesc = pg.fields, wantVal = pg.fields, wantEnd = 1;
    Counts oldc = runThroughRing(frames, /*paced=*/false);
    Counts newc = runThroughRing(frames, /*paced=*/true);
    printf("\n== %s (%d fields, %zu frames) ==\n", pg.name, pg.fields, frames.size());
    printf("  OLD (blast) : desc=%d/%d val=%d/%d end=%d/%d  crcErr=%d  overwritten=%ld B  -> %s\n",
      oldc.desc,wantDesc, oldc.val,wantVal, oldc.end,wantEnd, oldc.crcErr, overwritten,
      (oldc.total() < wantDesc+wantVal+wantEnd) ? "LOSES FRAMES (reproduces the bug)" : "no loss");
    // re-run new to refresh overwritten for its report
    Counts newc2 = runThroughRing(frames, true);
    printf("  NEW (paced) : desc=%d/%d val=%d/%d end=%d/%d  crcErr=%d  overwritten=%ld B  -> %s\n",
      newc2.desc,wantDesc, newc2.val,wantVal, newc2.end,wantEnd, newc2.crcErr, overwritten,
      (newc2.desc==wantDesc && newc2.val==wantVal && newc2.end==wantEnd && newc2.crcErr==0) ? "ALL FRAMES INTACT" : "STILL LOSSY");
    bool pageOk = (newc2.desc==wantDesc && newc2.val==wantVal && newc2.end==wantEnd && newc2.crcErr==0 && overwritten==0);
    bool oldBug = (oldc.total() < wantDesc+wantVal+wantEnd);   // confirm the diagnosis
    allOk = allOk && pageOk && oldBug;
  }
  printf("\n%s\n", allOk ? "RING CROSS-CHECK PASS — paced generator loses nothing; blast reproduces the bug"
                         : "RING CROSS-CHECK FAIL");
  return allOk?0:1;
}
