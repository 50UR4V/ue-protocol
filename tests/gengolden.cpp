// Generate v2 golden frames for the Kotlin cross-check: exercises the
// FIELD_DESC v2 layout (u16 page, kind, icon, link target) + CONFIG_PAGES u16 ids.
// Mirrors companion_emit.cpp buildRowDesc / GET_PAGES byte-for-byte.
#include "ultraedge/protocol.h"
#include <cstdio>
#include <cstring>
using namespace ultraedge;

static void putStr(uint8_t* p, size_t& o, const char* s){ uint8_t n=0; while(s[n])n++; p[o++]=n; for(uint8_t i=0;i<n;++i)p[o++]=(uint8_t)s[i]; }
static void putI32(uint8_t* p, size_t& o, int32_t v){ p[o++]=v&0xFF;p[o++]=(v>>8)&0xFF;p[o++]=(v>>16)&0xFF;p[o++]=(v>>24)&0xFF; }

struct RowDef { uint16_t id; uint8_t kind,type,icon; int32_t mn,mx,st; const char* label; const char* unit; const char* const* opts; uint8_t nopt; uint16_t target; uint8_t domain; };
static size_t buildRowDesc(uint16_t page, const RowDef& r, uint8_t* p){
  size_t o=0;
  p[o++]=r.id&0xFF; p[o++]=r.id>>8; p[o++]=page&0xFF; p[o++]=page>>8;
  p[o++]=r.kind; p[o++]=r.type; p[o++]=r.icon;
  putI32(p,o,r.mn); putI32(p,o,r.mx); putI32(p,o,r.st);
  putStr(p,o,r.label); putStr(p,o,r.unit?r.unit:"");
  p[o++]=r.nopt; for(uint8_t i=0;i<r.nopt;++i) putStr(p,o,r.opts[i]);
  if(r.kind==ROW_LINK){ p[o++]=r.target&0xFF; p[o++]=r.target>>8; }
  if(r.type==T_REF) p[o++]=r.domain;
  return o;
}
static void emit(const char* name, const uint8_t* f, size_t n){
  printf("// %s\nintArrayOf(", name);
  for(size_t i=0;i<n;++i) printf("%d%s", f[i], i+1<n?",":"");
  printf(")\n");
}
int main(){
  uint8_t f[MAX_FRAME_WIRE]; size_t n; uint8_t seq=0;
  const char* const OPTS_TRIMINC[]={"Expo","ExFine","Fine","Medium","Coarse"};

  // CONFIG_PAGES (u16 ids + icon + title) — matches GET_PAGES handler
  { uint8_t p[96]; size_t o=0; p[o++]=2;
    p[o++]=PAGE_MODEL_SETUP&0xFF;p[o++]=PAGE_MODEL_SETUP>>8;p[o++]=IC_SETUP; putStr(p,o,"Model Setup");
    p[o++]=PAGE_OUTPUTS&0xFF;p[o++]=PAGE_OUTPUTS>>8;p[o++]=IC_OUTPUTS; putStr(p,o,"Outputs");
    n=encodeFrame(MSG_CONFIG_PAGES,seq++,p,(uint16_t)o,f,sizeof(f)); emit("CONFIG_PAGES",f,n); }

  // Model name FIELD (str)
  { RowDef r{CFG_MODEL_NAME,ROW_FIELD,T_STR,IC_GENERAL,0,15,1,"Model name","",nullptr,0,0};
    uint8_t dp[200]; size_t dn=buildRowDesc(PAGE_MODEL_SETUP,r,dp);
    n=encodeFrame(MSG_FIELD_DESC,seq++,dp,(uint16_t)dn,f,sizeof(f)); emit("FIELD_DESC model name",f,n); }

  // Timer 1 LINK row (target=PAGE_TIMER_BASE+0, icon TIMERS)
  { RowDef r{(uint16_t)(CFG_TMRROW_BASE+0),ROW_LINK,T_LINK,IC_TIMERS,0,0,0,"Timer 1","",nullptr,0,PAGE_TIMER_BASE+0};
    uint8_t dp[200]; size_t dn=buildRowDesc(PAGE_MODEL_SETUP,r,dp);
    n=encodeFrame(MSG_FIELD_DESC,seq++,dp,(uint16_t)dn,f,sizeof(f)); emit("FIELD_DESC timer1 link",f,n); }

  // Trim increment ENUM (on the Trims sub-page)
  { RowDef r{CFG_TRIM_INC,ROW_FIELD,T_ENUM,IC_NONE,0,4,1,"Trim increment","",OPTS_TRIMINC,5,0,0};
    uint8_t dp[200]; size_t dn=buildRowDesc(PAGE_TRIMS,r,dp);
    n=encodeFrame(MSG_FIELD_DESC,seq++,dp,(uint16_t)dn,f,sizeof(f)); emit("FIELD_DESC trim inc",f,n); }

  // Source T_REF field (domain = DOM_SOURCE) on a mix detail page
  { RowDef r{(uint16_t)(CFG_MIX_BASE|MX_SRC),ROW_FIELD,T_REF,IC_NONE,0,2047,1,"Source","",nullptr,0,0,DOM_SOURCE};
    uint8_t dp[200]; size_t dn=buildRowDesc(PAGE_MIX_BASE,r,dp);
    n=encodeFrame(MSG_FIELD_DESC,seq++,dp,(uint16_t)dn,f,sizeof(f)); emit("FIELD_DESC source ref",f,n); }

  // PAGE_DESC_END (u16 page)
  { uint8_t e[2]={PAGE_MODEL_SETUP&0xFF,PAGE_MODEL_SETUP>>8};
    n=encodeFrame(MSG_PAGE_DESC_END,seq++,e,2,f,sizeof(f)); emit("PAGE_DESC_END",f,n); }

  // ---- unchanged message goldens, re-emitted at PROTO_VERSION 2 ----
  seq=0;
  { Hello h; h.proto_ver=PROTO_VERSION; h.fw_ver=0x00040000; h.role=ROLE_RADIO; h.caps=CAP_STRUCTURED;
    n=encodeHello(seq++,h,true,f,sizeof(f)); emit("HELLO_ACK",f,n); }
  { int16_t ch[16]={0,256,-256,510,-510,1024,-1024,50,0,0,0,0,0,0,0,0};
    n=encodeChannels(seq++,ch,16,f,sizeof(f)); emit("CHANNELS16",f,n); }
  { uint8_t vp[4+8]; vp[0]=CFG_MODEL_NAME&0xFF; vp[1]=CFG_MODEL_NAME>>8; vp[2]=T_STR; vp[3]=8;
    memcpy(vp+4,"AIRFIELD",8); n=encodeFrame(MSG_CONFIG_VALUE,seq++,vp,12,f,sizeof(f)); emit("CV model name",f,n); }
  { uint8_t vp[6]; vp[0]=CFG_TIMER1_START&0xFF; vp[1]=CFG_TIMER1_START>>8; vp[2]=T_U16; vp[3]=2; vp[4]=(300&0xFF); vp[5]=(300>>8);
    n=encodeFrame(MSG_CONFIG_VALUE,seq++,vp,6,f,sizeof(f)); emit("CV timer1",f,n); }
  // CAPS golden (a few anchors): CH base 100 count 32, LS base 200 count 64, curve count 32
  { uint8_t p[32]; size_t o=1; uint8_t cnt=0;
    auto kv=[&](uint8_t k,int16_t v){ p[o++]=k; p[o++]=v&0xFF; p[o++]=(v>>8)&0xFF; cnt++; };
    kv(CAP_SRC_NONE,0); kv(CAP_SRC_CH,100); kv(CAP_N_CH,32); kv(CAP_SRC_LS,200); kv(CAP_N_LS,64);
    kv(CAP_SW_SW,1); kv(CAP_N_SW,8); kv(CAP_N_CURVE,32); p[0]=cnt;
    n=encodeFrame(MSG_CAPS,seq++,p,(uint16_t)o,f,sizeof(f)); emit("CAPS",f,n); }
  return 0;
}
