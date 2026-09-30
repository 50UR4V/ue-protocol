// Engine cross-check: value-encoding round-trips for the new output &
// timer fields, conditional-visibility row counts, and a ring test proving the
// paced generator delivers the bigger list/detail pages with zero loss.
#include "ultraedge/protocol.h"
#include <cstdio>
#include <vector>
#include <cstring>
using namespace ultraedge;

// ---- value math (mirrors companion_emit.cpp cfgGet/SetOutput + Timer) -------
static const int PPMC = 1500, MAXC = 32;
struct Lim { int min,max,ppm,off,sym,rev,curve; };
static int outGet(Lim&l,int sub){ switch(sub){
  case OUT_REVERSE:return l.rev; case OUT_SUBTRIM:return l.off/10;
  case OUT_MIN:return (l.min-1000)/10; case OUT_MAX:return (l.max+1000)/10;
  case OUT_CURVE:return l.curve; case OUT_PPMCENTER:return PPMC+l.ppm;
  case OUT_SUBTRIMMODE:return l.sym; } return -999; }
static int outSet(Lim&l,int sub,int p){ switch(sub){
  case OUT_REVERSE:l.rev=p?1:0;return 0;
  case OUT_SUBTRIM:if(p<-100||p>100)return NACK_OUT_OF_RANGE;l.off=p*10;return 0;
  case OUT_MIN:if(p<-150||p>0)return NACK_OUT_OF_RANGE;l.min=p*10+1000;return 0;
  case OUT_MAX:if(p<0||p>150)return NACK_OUT_OF_RANGE;l.max=p*10-1000;return 0;
  case OUT_CURVE:if(p<-MAXC||p>MAXC)return NACK_OUT_OF_RANGE;l.curve=p;return 0;
  case OUT_PPMCENTER:{int c=p-PPMC;if(c<-500||c>500)return NACK_OUT_OF_RANGE;l.ppm=c;return 0;}
  case OUT_SUBTRIMMODE:if(p<0||p>1)return NACK_OUT_OF_RANGE;l.sym=p?1:0;return 0;} return NACK_UNKNOWN_FIELD; }

// ---- ring model (same ring model as ringcheck) --------------------------------
static const int RING=512; static uint8_t rb[RING]; static int rH,rT,rC; static long ov;
static void rReset(){rH=rT=rC=0;ov=0;}
static void rPut(uint8_t b){rb[rH]=b;rH=(rH+1)%RING; if(rC<RING)rC++; else{rT=(rT+1)%RING;ov++;}}
static int rFree(){return RING-rC;}
template<class F> static void rDrain(int n,F s){while(n-->0&&rC>0){uint8_t b=rb[rT];rT=(rT+1)%RING;rC--;s(b);}}

static void putStr(uint8_t*p,size_t&o,const char*s){uint8_t n=0;while(s[n])n++;p[o++]=n;for(uint8_t i=0;i<n;++i)p[o++]=(uint8_t)s[i];}
static void putI32(uint8_t*p,size_t&o,int32_t v){p[o++]=v&0xFF;p[o++]=(v>>8)&0xFF;p[o++]=(v>>16)&0xFF;p[o++]=(v>>24)&0xFF;}
struct Row{uint16_t id;uint8_t kind,type,icon;int32_t mn,mx,st;const char*label;const char*unit;const char*const*opts;uint8_t nopt;uint16_t target;};
static size_t buildRow(uint16_t page,const Row&r,uint8_t*p){size_t o=0;
  p[o++]=r.id&0xFF;p[o++]=r.id>>8;p[o++]=page&0xFF;p[o++]=page>>8;p[o++]=r.kind;p[o++]=r.type;p[o++]=r.icon;
  putI32(p,o,r.mn);putI32(p,o,r.mx);putI32(p,o,r.st);putStr(p,o,r.label);putStr(p,o,r.unit?r.unit:"");
  p[o++]=r.nopt;for(uint8_t i=0;i<r.nopt;++i)putStr(p,o,r.opts[i]);
  if(r.kind==ROW_LINK){p[o++]=r.target&0xFF;p[o++]=r.target>>8;}return o;}

// Build one page's full frame list (FIELD_DESC + a value/summary per row + END).
static std::vector<std::vector<uint8_t>> buildPageFrames(uint16_t page, const std::vector<Row>& rows){
  std::vector<std::vector<uint8_t>> fr; uint8_t seq=0,f[MAX_FRAME_WIRE];
  for(auto&r:rows){ uint8_t dp[200]; size_t dn=buildRow(page,r,dp);
    size_t n=encodeFrame(MSG_FIELD_DESC,seq++,dp,(uint16_t)dn,f,sizeof(f)); fr.emplace_back(f,f+n);
    uint8_t vp[4+24]; vp[0]=r.id&0xFF;vp[1]=r.id>>8;
    if(r.kind==ROW_LINK){ const char*s="OFF"; vp[2]=T_STR;vp[3]=3;memcpy(vp+4,s,3); n=encodeFrame(MSG_CONFIG_VALUE,seq++,vp,7,f,sizeof(f)); }
    else { vp[2]=T_I16;vp[3]=2;vp[4]=0x10;vp[5]=0; n=encodeFrame(MSG_CONFIG_VALUE,seq++,vp,6,f,sizeof(f)); }
    fr.emplace_back(f,f+n);
  }
  uint8_t e[2]={(uint8_t)(page&0xFF),(uint8_t)(page>>8)}; size_t n=encodeFrame(MSG_PAGE_DESC_END,seq++,e,2,f,sizeof(f)); fr.emplace_back(f,f+n);
  return fr;
}
struct Cnt{int desc=0,val=0,end=0,crc=0;};
static Cnt runRing(const std::vector<std::vector<uint8_t>>&fr){
  rReset(); Cnt c; Decoder d;
  d.setCallback([](const Message&m,void*u){Cnt*c=(Cnt*)u; if(m.type==MSG_FIELD_DESC)c->desc++; else if(m.type==MSG_CONFIG_VALUE)c->val++; else if(m.type==MSG_PAGE_DESC_END)c->end++;},&c);
  auto sink=[&](uint8_t b){d.feed(&b,1);}; size_t fi=0;
  for(int t=0;t<600;++t){ rDrain(640,sink);
    for(int g=0;g<8&&fi<fr.size();++g){int len=(int)fr[fi].size(); if(rFree()>=len+64){for(uint8_t b:fr[fi])rPut(b);fi++;}else break;}
    if(fi>=fr.size()&&rC==0)break; }
  for(int t=0;t<200&&rC>0;++t)rDrain(640,sink);
  c.crc=(int)d.framesCrcErr(); return c;
}

int main(){
  bool ok=true;
  // ---- A. output value round-trips + range rejects ----
  Lim l{0,0,0,0,0,0,0};
  outSet(l,OUT_PPMCENTER,1600); outSet(l,OUT_CURVE,-5); outSet(l,OUT_SUBTRIMMODE,1); outSet(l,OUT_MAX,90);
  bool a = outGet(l,OUT_PPMCENTER)==1600 && outGet(l,OUT_CURVE)==-5 && outGet(l,OUT_SUBTRIMMODE)==1 && outGet(l,OUT_MAX)==90;
  int r1=outSet(l,OUT_PPMCENTER,2200), r2=outSet(l,OUT_CURVE,99), r3=outSet(l,OUT_SUBTRIMMODE,3);
  bool arej = r1==NACK_OUT_OF_RANGE && r2==NACK_OUT_OF_RANGE && r3==NACK_OUT_OF_RANGE;
  printf("A outputs: ppm=%d curve=%d submode=%d max=%d  rejects(%d,%d,%d) -> %s\n",
    outGet(l,OUT_PPMCENTER),outGet(l,OUT_CURVE),outGet(l,OUT_SUBTRIMMODE),outGet(l,OUT_MAX),r1,r2,r3,(a&&arej)?"OK":"FAIL");
  ok &= a && arej;

  // ---- B. conditional visibility: timer OFF shows 2 rows, ON shows 7 ----
  auto timerRows=[&](bool on){ std::vector<Row> v;
    v.push_back({(uint16_t)(CFG_TMR_BASE|TMR_NAME),ROW_FIELD,T_STR,IC_TIMERS,0,8,1,"Name","",nullptr,0,0});
    v.push_back({(uint16_t)(CFG_TMR_BASE|TMR_MODE),ROW_FIELD,T_ENUM,IC_NONE,0,5,1,"Mode","",nullptr,0,0});
    if(on){ const char*U="s";
      v.push_back({(uint16_t)(CFG_TMR_BASE|TMR_START),ROW_FIELD,T_U16,IC_NONE,0,3600,5,"Start",U,nullptr,0,0});
      v.push_back({(uint16_t)(CFG_TMR_BASE|TMR_MINBEEP),ROW_FIELD,T_BOOL,IC_NONE,0,1,1,"Minute beep","",nullptr,0,0});
      v.push_back({(uint16_t)(CFG_TMR_BASE|TMR_COUNTDOWN),ROW_FIELD,T_ENUM,IC_NONE,0,3,1,"Countdown","",nullptr,0,0});
      v.push_back({(uint16_t)(CFG_TMR_BASE|TMR_PERSIST),ROW_FIELD,T_ENUM,IC_NONE,0,2,1,"Persistent","",nullptr,0,0});
      v.push_back({(uint16_t)(CFG_TMR_BASE|TMR_DIR),ROW_FIELD,T_ENUM,IC_NONE,0,1,1,"Direction","",nullptr,0,0}); }
    return v; };
  { auto off=buildPageFrames(PAGE_TIMER_BASE,timerRows(false)); Cnt c=runRing(off);
    bool vok = c.desc==2 && c.val==2 && c.end==1 && c.crc==0;
    printf("B timer OFF: desc=%d val=%d end=%d crc=%d ov=%ld -> %s\n",c.desc,c.val,c.end,c.crc,ov,vok?"OK":"FAIL"); ok&=vok&&ov==0; }
  { auto on=buildPageFrames(PAGE_TIMER_BASE,timerRows(true)); Cnt c=runRing(on);
    bool vok = c.desc==7 && c.val==7 && c.end==1 && c.crc==0;
    printf("B timer ON : desc=%d val=%d end=%d crc=%d ov=%ld -> %s\n",c.desc,c.val,c.end,c.crc,ov,vok?"OK":"FAIL"); ok&=vok&&ov==0; }

  // ---- C. Outputs LIST page (32 channel LINK rows) through the ring ----
  { std::vector<Row> rows; char labels[32][8];
    for(int ch=0;ch<32;++ch){ int i=0;labels[ch][i++]='C';labels[ch][i++]='H'; if(ch+1>=10)labels[ch][i++]='0'+(ch+1)/10; labels[ch][i++]='0'+(ch+1)%10; labels[ch][i]=0;
      rows.push_back({(uint16_t)(CFG_OUTROW_BASE+ch),ROW_LINK,T_LINK,IC_CHANNEL,0,0,0,labels[ch],"",nullptr,0,(uint16_t)(PAGE_OUT_BASE+ch)}); }
    auto fr=buildPageFrames(PAGE_OUTPUTS,rows); Cnt c=runRing(fr);
    bool vok = c.desc==32 && c.val==32 && c.end==1 && c.crc==0 && ov==0;
    printf("C outputs list: desc=%d/32 val=%d/32 end=%d crc=%d ov=%ld (%zu frames) -> %s\n",c.desc,c.val,c.end,c.crc,ov,fr.size(),vok?"OK":"FAIL"); ok&=vok; }

  // ---- D. Output DETAIL page (8 fields) through the ring ----
  { const char*const SM[]={"Center","Symmetric"}; std::vector<Row> rows={
      {(uint16_t)(CFG_OUT_BASE|OUT_NAME),ROW_FIELD,T_STR,IC_CHANNEL,0,6,1,"Name","",nullptr,0,0},
      {(uint16_t)(CFG_OUT_BASE|OUT_SUBTRIM),ROW_FIELD,T_I16,IC_NONE,-100,100,1,"Subtrim","",nullptr,0,0},
      {(uint16_t)(CFG_OUT_BASE|OUT_MIN),ROW_FIELD,T_I16,IC_NONE,-150,0,1,"Min","%",nullptr,0,0},
      {(uint16_t)(CFG_OUT_BASE|OUT_MAX),ROW_FIELD,T_I16,IC_NONE,0,150,1,"Max","%",nullptr,0,0},
      {(uint16_t)(CFG_OUT_BASE|OUT_REVERSE),ROW_FIELD,T_BOOL,IC_NONE,0,1,1,"Inverted","",nullptr,0,0},
      {(uint16_t)(CFG_OUT_BASE|OUT_CURVE),ROW_FIELD,T_I16,IC_CURVES,-32,32,1,"Curve","",nullptr,0,0},
      {(uint16_t)(CFG_OUT_BASE|OUT_PPMCENTER),ROW_FIELD,T_I16,IC_NONE,1000,2000,1,"PPM center","us",nullptr,0,0},
      {(uint16_t)(CFG_OUT_BASE|OUT_SUBTRIMMODE),ROW_FIELD,T_ENUM,IC_NONE,0,1,1,"Subtrim mode","",SM,2,0}};
    auto fr=buildPageFrames(PAGE_OUT_BASE,rows); Cnt c=runRing(fr);
    bool vok = c.desc==8 && c.val==8 && c.end==1 && c.crc==0 && ov==0;
    printf("D output detail: desc=%d/8 val=%d/8 end=%d crc=%d ov=%ld -> %s\n",c.desc,c.val,c.end,c.crc,ov,vok?"OK":"FAIL"); ok&=vok; }

  printf("\n%s\n", ok?"NAV ENGINE CROSS-CHECK PASS":"NAV ENGINE CROSS-CHECK FAIL");
  return ok?0:1;
}
