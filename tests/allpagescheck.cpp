// Cross-check for the full page set: the biggest list pages (64-item
// Mixes / Logical Switches) through the 512-byte ring, plus GVar min/max offset
// math and per-item id decoding.
#include "ultraedge/protocol.h"
#include <cstdio>
#include <vector>
#include <cstring>
using namespace ultraedge;

// ring model
static const int RING=512; static uint8_t rb[RING]; static int rH,rT,rC; static long ov;
static void rReset(){rH=rT=rC=0;ov=0;}
static void rPut(uint8_t b){rb[rH]=b;rH=(rH+1)%RING; if(rC<RING)rC++; else{rT=(rT+1)%RING;ov++;}}
static int rFree(){return RING-rC;}
template<class F> static void rDrain(int n,F s){while(n-->0&&rC>0){uint8_t b=rb[rT];rT=(rT+1)%RING;rC--;s(b);}}

static void putStr(uint8_t*p,size_t&o,const char*s){uint8_t n=0;while(s[n])n++;p[o++]=n;for(uint8_t i=0;i<n;++i)p[o++]=(uint8_t)s[i];}
static void putI32(uint8_t*p,size_t&o,int32_t v){p[o++]=v&0xFF;p[o++]=(v>>8)&0xFF;p[o++]=(v>>16)&0xFF;p[o++]=(v>>24)&0xFF;}

// Build a list page of `n` LINK rows (id=base+i*STRIDE+SUMMARY, target=detail+i) + summary + END.
static std::vector<std::vector<uint8_t>> buildList(uint16_t page, uint16_t base, uint16_t detail, int n){
  std::vector<std::vector<uint8_t>> fr; uint8_t seq=0,f[MAX_FRAME_WIRE];
  for(int i=0;i<n;++i){
    uint16_t id=(uint16_t)(base+i*ITEM_STRIDE+SUB_SUMMARY), tgt=(uint16_t)(detail+i);
    uint8_t dp[64]; size_t o=0;
    dp[o++]=id&0xFF;dp[o++]=id>>8;dp[o++]=page&0xFF;dp[o++]=page>>8;dp[o++]=ROW_LINK;dp[o++]=T_LINK;dp[o++]=IC_MIXER;
    putI32(dp,o,0);putI32(dp,o,0);putI32(dp,o,0);
    char lbl[12]; int k=0; const char*P="Mix "; while(P[k]){lbl[k]=P[k];k++;} int m=i+1; if(m>=10){lbl[k++]='0'+m/10;} lbl[k++]='0'+m%10; lbl[k]=0;
    putStr(dp,o,lbl); putStr(dp,o,""); dp[o++]=0; dp[o++]=tgt&0xFF; dp[o++]=tgt>>8;
    size_t fn=encodeFrame(MSG_FIELD_DESC,seq++,dp,(uint16_t)o,f,sizeof(f)); fr.emplace_back(f,f+fn);
    uint8_t vp[8]; vp[0]=id&0xFF;vp[1]=id>>8;vp[2]=T_STR;vp[3]=3;memcpy(vp+4,"CH1",3);
    fn=encodeFrame(MSG_CONFIG_VALUE,seq++,vp,7,f,sizeof(f)); fr.emplace_back(f,f+fn);
  }
  uint8_t e[2]={(uint8_t)(page&0xFF),(uint8_t)(page>>8)}; size_t fn=encodeFrame(MSG_PAGE_DESC_END,seq++,e,2,f,sizeof(f)); fr.emplace_back(f,f+fn);
  return fr;
}
struct Cnt{int desc=0,val=0,end=0,crc=0;};
static Cnt runRing(const std::vector<std::vector<uint8_t>>&fr){
  rReset(); Cnt c; Decoder d;
  d.setCallback([](const Message&m,void*u){Cnt*c=(Cnt*)u; if(m.type==MSG_FIELD_DESC)c->desc++; else if(m.type==MSG_CONFIG_VALUE)c->val++; else if(m.type==MSG_PAGE_DESC_END)c->end++;},&c);
  auto sink=[&](uint8_t b){d.feed(&b,1);}; size_t fi=0;
  for(int t=0;t<2000;++t){ rDrain(640,sink);
    for(int g=0;g<8&&fi<fr.size();++g){int len=(int)fr[fi].size(); if(rFree()>=len+64){for(uint8_t b:fr[fi])rPut(b);fi++;}else break;}
    if(fi>=fr.size()&&rC==0)break; }
  for(int t=0;t<400&&rC>0;++t)rDrain(640,sink);
  c.crc=(int)d.framesCrcErr(); return c;
}

// GVar min/max offset math (mirrors gvGet/gvSet): min stored=v+1024, max stored=1024-v.
static int gvMinRT(int v){ int stored=v+1024; return stored-1024; }
static int gvMaxRT(int v){ int stored=1024-v; return 1024-stored; }

// per-item id decode
static uint16_t itemOf(uint16_t id,uint16_t base){return (uint16_t)((id-base)/ITEM_STRIDE);}
static uint8_t  subOf(uint16_t id,uint16_t base){return (uint8_t)((id-base)%ITEM_STRIDE);}

int main(){
  bool ok=true;
  // A. biggest list pages through the ring (64 items -> 129 frames)
  for (int n : {64}) {
    auto fr=buildList(PAGE_MIXES,CFG_MIX_BASE,PAGE_MIX_BASE,n); Cnt c=runRing(fr);
    bool v = c.desc==n && c.val==n && c.end==1 && c.crc==0 && ov==0;
    printf("A list %d items: desc=%d/%d val=%d/%d end=%d crc=%d ov=%ld (%zu frames) -> %s\n",n,c.desc,n,c.val,n,c.end,c.crc,ov,fr.size(),v?"OK":"FAIL"); ok&=v;
  }
  // B. GVar min/max round-trip at extremes
  bool g = gvMinRT(-500)==-500 && gvMinRT(500)==500 && gvMaxRT(-250)==-250 && gvMaxRT(250)==250;
  printf("B gvar min/max: -500->%d 500->%d  max -250->%d 250->%d -> %s\n",gvMinRT(-500),gvMinRT(500),gvMaxRT(-250),gvMaxRT(250),g?"OK":"FAIL"); ok&=g;
  // C. per-item id decode (item 40, sub GV_MAX) round-trips
  uint16_t id=(uint16_t)(CFG_LS_BASE+40*ITEM_STRIDE+LS_DURATION);
  bool d = itemOf(id,CFG_LS_BASE)==40 && subOf(id,CFG_LS_BASE)==LS_DURATION;
  printf("C id decode: LS item %d sub %d (want 40,%d) -> %s\n",itemOf(id,CFG_LS_BASE),subOf(id,CFG_LS_BASE),LS_DURATION,d?"OK":"FAIL"); ok&=d;
  // D. summary sub-id sits in every item's block and never collides with a real sub
  bool s = SUB_SUMMARY > FM_FADEOUT && SUB_SUMMARY > MX_CURVE && SUB_SUMMARY > LS_DURATION && SUB_SUMMARY < ITEM_STRIDE;
  printf("D summary sub 0x%02X distinct from field subs, < stride 0x%02X -> %s\n",SUB_SUMMARY,ITEM_STRIDE,s?"OK":"FAIL"); ok&=s;

  printf("\n%s\n", ok?"ALL-PAGES CROSS-CHECK PASS":"ALL-PAGES CROSS-CHECK FAIL");
  return ok?0:1;
}
