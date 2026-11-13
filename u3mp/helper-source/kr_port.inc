/* ===== v5.16 kill-reward port: PS3 f_381ae4 (ScoreManager kill record, called first by the kill handler f_318d88) =====
 * PS4 dropped the whole function, so its kill history, K/D counters, ~40 stat events (which drive medals and missions
 * through 0x5ae4e0) and medal sub-checks never happened. This code restores it 1:1. It is hooked at the entry of the PS4
 * kill handler 0x7350c0 (rdi = NetGameManager 0x17cb060, rsi = kill info), as PS3 calls it first (kind 49).
 *
 * Map (PS3 -> PS4), checked against PS4 code (see NOTES-funcs16.md):
 *  score manager 0x12492a4 -> 0x17dd780 (f004: was 0x17dd778, 8 bytes off; same layout: +0 members, +0xe0 history count, +0xe4 records 0x28 x 512,
 *    +0x50e4 member table 0xa4 {id, .. team byte +7}, +0x574c team records, +0x60cc player records 0x130); PS4 still
 *    reads the history (0x7ad280 GetWeaponIdByStatIndex) and UpdatePlaces = 0x7aa560 (PS3 f_37d61c)
 *  net game manager 0x1223da0 -> 0x17cb060; net players +0x1f0 stride 0x8b0 -> +0x390 stride 0x9c0; local players
 *    +0x1e4/+0x1e8 -> +0x370/+0x378 (pointers); first-blood byte +0x19d -> +0x31d (net sync msg 0x60)
 *  net player: id fields +0x1ac/+0x1b0/+0x1b4 -> +0x1e4/+0x1e8/+0x1ec, team +0x1e4 -> +0x21c, active +0xa9/+0x51c ->
 *    +0xe1/+0x5e4, +0x344/+0x348 -> +0x39c/+0x3a0, +0x5a4 -> +0x674, +0x6a0 -> +0x770, last damager time/id
 *    +0x894/+0x898 -> +0x9a0/+0x9a4 (msg 0x79 handler 0x6cfeb0), character handle +0x10/+0x14 -> +0x10 (u64)/+0x18
 *  game clock *(0x121a5bc)+0xc -> *(0x17c3580)+0xc
 *  f_300a40 -> 0x721050, f_309bbc -> 0x722670, f_3082a4 -> 0x729b10, f_30b3a4 -> 0x7270f0, f_30f008 -> 0x7424a0,
 *  f_59c94 (stat event) -> 0x5ae4e0 (first arg unused), f_34c45c (missions) -> 0x773b50 on 0x17da200+user*0x1018,
 *  f_9a77a4 -> 0x11f2bf0, entity table f_2fefe4 -> FNV-1a hash at mgr+0x6520/+0x655c (node+0x18 = handle)
 *  stat index tables: f_2fc84c / f_2fc74c (0x14 PS3 entries) -> PS4 0x13a6170 (0x13 entries) + DC 0x9e354807,
 *  f_3555d4 (0x1c) -> PS4 0x13a66d0[1..0x1a]; "-killed" sid = PS3 f_709b14 crc32 (table 0x13a6ff0 on PS4)
 * Not in this increment (step 2/1b): points/cash/XP, boosters, kickback, +points popups, team score, f_37ae3c and
 * f_3814d8 (weapon defs), the co-op AI branch details (character flags), victim np+0x4e0 (unmapped).
 * Switches (state+0x3d79): bit1 U3MP_KR=0 whole port off, bit2 U3MP_KR_NOHIST, bit3 U3MP_KR_NOKD, bit4
 * U3MP_KR_NOSTAT, bit5 U3MP_KR_NOMEDAL. Counters X.kr[16] (see ExtState). */
#define KR_OFF 0x200u
#define KR_NOHIST 0x400u
#define KR_NOKD 0x800u
#define KR_NOSTAT 0x1000u
#define KR_NOMEDAL 0x2000u
typedef struct {
 uint8_t type,pad1; uint16_t killer,victim; int8_t kteam,vteam; int32_t time; uint8_t widx,pad2;
 int16_t pos[6]; uint8_t pad3[2]; int32_t f1c; uint8_t f20,f21,f22,f23,f24,f25,pad4[2];
} KRec;
_Static_assert(sizeof(KRec)==0x28&&offsetof(KRec,time)==8&&offsetof(KRec,widx)==0xc&&offsetof(KRec,pos)==0xe&&
 offsetof(KRec,f1c)==0x1c&&offsetof(KRec,f25)==0x25,"KRec");
/* float helpers (helper is built with -mno-sse; values are IEEE bit patterns) */
static uint32_t f_i2f(int32_t v){uint32_t r;__asm__("cvtsi2ss %1,%%xmm0\n\tmovd %%xmm0,%0":"=r"(r):"r"(v):"xmm0");return r;}
static uint32_t f_add(uint32_t a,uint32_t b){uint32_t r;__asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\taddss %%xmm1,%%xmm0\n\tmovd %%xmm0,%0":"=r"(r):"r"(a),"r"(b):"xmm0","xmm1");return r;}
static uint32_t f_sub(uint32_t a,uint32_t b){uint32_t r;__asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\tsubss %%xmm1,%%xmm0\n\tmovd %%xmm0,%0":"=r"(r):"r"(a),"r"(b):"xmm0","xmm1");return r;}
static uint32_t f_mul(uint32_t a,uint32_t b){uint32_t r;__asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\tmulss %%xmm1,%%xmm0\n\tmovd %%xmm0,%0":"=r"(r):"r"(a),"r"(b):"xmm0","xmm1");return r;}
static uint32_t f_div(uint32_t a,uint32_t b){uint32_t r;__asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\tdivss %%xmm1,%%xmm0\n\tmovd %%xmm0,%0":"=r"(r):"r"(a),"r"(b):"xmm0","xmm1");return r;}
static uint32_t f_sqrt(uint32_t a){uint32_t r;__asm__("movd %1,%%xmm0\n\tsqrtss %%xmm0,%%xmm0\n\tmovd %%xmm0,%0":"=r"(r):"r"(a):"xmm0");return r;}
static int32_t f_trunc(uint32_t a){int32_t r;__asm__("movd %1,%%xmm0\n\tcvttss2si %%xmm0,%0":"=r"(r):"r"(a):"xmm0");return r;}
static int f_lt(uint32_t a,uint32_t b){int r;__asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\txorl %0,%0\n\tucomiss %%xmm0,%%xmm1\n\tseta %b0":"=&r"(r):"r"(a),"r"(b):"xmm0","xmm1","cc");return r;}  /* a < b */
static int f_le(uint32_t a,uint32_t b){int r;__asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\txorl %0,%0\n\tucomiss %%xmm0,%%xmm1\n\tsetae %b0":"=&r"(r):"r"(a),"r"(b):"xmm0","xmm1","cc");return r;} /* a <= b */
#define F_1_300 0x3b5a740eu
#define F_ONE 0x3f800000u
#define F_16 0x41800000u
#define F_1_16 0x3d800000u

static uint32_t kr_bits(ProfileState *s){return *(uint32_t*)((uint8_t*)s+0x3d78);}
/* v5.43 PS3 f_9f3654(tags, sid) as PS4 inlines it (0x9f8d1c): tag table built flag 0x1b3efd8, the sid's bucket list at
   0x1b3efd8+bucket (circular, the head itself is the end), node +0x10 sid, +0x14 bit; bit set in tags+0x20[bit>>6]. */
static int kr_tag(uintptr_t base,uint8_t *tags,uint32_t sid,uint32_t bucket){
 uint8_t *t=(uint8_t*)(base+0x1b3efd8);if(!tags||!*t)return 0;
 return u3x_has_tag(t+bucket,tags,sid);}   /* v7.131 C++ */
static uint8_t *kr_ng(uintptr_t base){return (uint8_t*)(base+0x17cb060u);}
/* f004 fix: the PS3 score-manager fields sit 8 bytes further on PS4. Every PS4 reader passes 0x17dd778+8: GetWeaponIdByStatIndex
 * 0x7ad280 (callers 0xa19e09/0xbbcbf5: lea rdi,[0x17dd778]; add rdi,8; it reads the count at rdi+0xe0 and the victim at rdi+0xe8),
 * UpdatePlaces 0x7aa560 (0x77a8a6: lea rdi,[rbx+8]), and the spawner score 0x730a00 reads the history count at 0x17dd778+0xe8 and
 * the records at +0xec (victim +0xf0, time +0xf4, victim pos +0x100), member ids at +0x50ec, player records at +0x60d4. With the
 * object at 0x17dd778 the port wrote the history count 8 bytes low, so the spawner score read the victim id/team bytes of record
 * 0 as the count and ran off the table (fault 0x73227b after a death, City of Brass). PS3 0x12492a4 = PS4 0x17dd780. */
static uint8_t *kr_S(uintptr_t base){return (uint8_t*)(base+0x17dd780u);}
static KRec *kr_rec(uint8_t *S,uint32_t i){return (KRec*)(S+0xe4+(uint64_t)i*0x28);}
static uint32_t kr_n(uint8_t *S){return *(uint32_t*)(S+0xe0);}
/* PS3 f_358c3c: net player id */
static uint32_t kr_pid(uint8_t *np){return u3x_net_player_id(np);}   /* v7.131 C++ */
static int32_t kr_team(uint8_t *np){return *(int32_t*)(np+0x21c);}
static uint32_t kr_user(uint8_t *np){return *(uint32_t*)(np+0x1e8);}
/* PS3 f_30089c */
static uint8_t *kr_np_by_id(uintptr_t base,uint32_t id){
 if(!id)return 0;
 for(int i=0;i<10;i++){uint8_t *np=kr_ng(base)+0x390+(uint64_t)i*0x9c0;if(kr_pid(np)==id)return np;}
 return 0;}
/* PS3 f_359694 (compares with both local slots, even when np is 0, as PS3 does) */
static int kr_local(uintptr_t base,uint8_t *np){
 uint8_t *ng=kr_ng(base);return np==*(uint8_t**)(ng+0x370)||np==*(uint8_t**)(ng+0x378);}
/* PS3 f_359cc4(np,0) */
static int kr_active(uint8_t *np){return u3x_net_player_in_game(np);}   /* v7.131 C++ */
/* PS3 f_2f8af4 / f_2f9ea0 */
static uint8_t *kr_np_at(uintptr_t base,uint32_t i){
 if(!*(uint8_t*)(base+0x17d5c61u)||i>9)return 0;return kr_ng(base)+0x390+(uint64_t)i*0x9c0;}
/* current mode entry (PS3 f_300a40 = PS4 0x721050) and its type (+0xc) */
static uint8_t *kr_mode(uintptr_t base){return ((uint8_t*(*)(void*))(base+0x721050u))(kr_ng(base));}
static int32_t kr_mode_type(uintptr_t base){uint8_t *m=kr_mode(base);return m?*(int32_t*)(m+0xc):-1;}
/* PS3 f_3081a4: same player, or same team (never in mode type 0xe) */
static int kr_friendly(uintptr_t base,uint8_t *a,uint8_t *b){
 if(a==b)return 1;if(kr_mode_type(base)==0xe)return 0;
 if(a&&b)return kr_team(a)==kr_team(b);return 0;}
static int kr_online(uintptr_t base){return ((uint8_t(*)(void*))(base+0x7270f0u))(kr_ng(base))!=0;}
/* PS3 f_30feec */
static int kr_30feec(uintptr_t base){
 if(kr_mode_type(base)==0xd)return 1;
 return ((uint32_t(*)(void*))(base+0x7424a0u))(kr_ng(base))==0x4b720b62u;}
/* member table index / record (PS3 loops over S+0x50e4 comparing f_358c3c; default record 0x125ed48) */
static int kr_member(uint8_t *S,uint32_t id){return u3x_score_member(S,id);}   /* v7.131 C++ */
static uint8_t *kr_prec(uint8_t *S,uint8_t *np){
 if(np){int i=kr_member(S,kr_pid(np));if(i>=0)return S+0x60cc+(uint64_t)i*0x130;}
 return X.krdummy;}
/* stat index <-> weapon sid (PS3 f_2fc84c / f_2fc74c, PS4 0x7ad280 GetWeaponIdByStatIndex) */
static uint32_t *kr_wlist(uintptr_t base){uint64_t *p=((uint64_t*(*)(uint32_t))(base+0x11f2bf0u))(0x9e354807u);return p?(uint32_t*)*p:0;}
static uint32_t kr_statidx(uintptr_t base,uint32_t sid){
 return u3x_weapon_stat_number((const uint32_t*)(base+0x13a6170u),kr_wlist(base),sid);}   /* v7.131 C++ */
static uint32_t kr_statw(uintptr_t base,uint32_t idx){
 return u3x_weapon_for_stat((const uint32_t*)(base+0x13a6170u),idx<0x13?0:kr_wlist(base),idx);}   /* v7.131 C++ */
static int kr_group6(uint32_t w){return u3x_weapon_group(w)==6;}   /* v7.131 C++ */
static int kr_group4(uint32_t w){return u3x_weapon_group(w)==4;}
/* stat event (PS3 f_59c94(0x11f45b0,id,amount,np,0) = PS4 0x5ae4e0) */
static void kr_stat(uintptr_t base,ProfileState *s,uint32_t id,uint32_t amount,uint8_t *np){
 X.kr[2]++;X.krs[X.kr[3]++&15]=id;
 if(kr_bits(s)&KR_NOSTAT)return;
 ((void(*)(void*,uint32_t,uint32_t,void*,uint32_t))(base+0x5ae4e0u))(0,id,amount,np,0);}
/* PS3 f_709b14: sid continued with a string (crc32, msb first, table 0x13a6ff0) */
static uint32_t kr_sid_cat(uintptr_t base,uint32_t h,const char *str){
 const uint32_t *t=(const uint32_t*)(base+0x13a6ff0u);
 while(*str){h=(h&0xffffff)<<8^t[((h>>24)^(uint8_t)*str)&0xff];str++;}return h;}
/* PS3 f_37b428: profile counter data+8+n*4 (PS4 profile object *(0x17dd610)+user*0x7b00, data at +0x248) */
static void kr_prof_add(uintptr_t base,uint8_t *np,uint32_t n,uint32_t amt){
 if(!kr_online(base))return;
 uint8_t *obj=*(uint8_t**)(base+0x17dd610u);if(!obj)return;
 obj+=(uint64_t)kr_user(np)*0x7b00;*(uint32_t*)(obj+0x250+n*4)+=amt;X.kr[9]++;}
static uint32_t kr_now(uintptr_t base){uint8_t *c=*(uint8_t**)(base+0x17c3580u);return c?*(uint32_t*)(c+0xc):0;}

/* ---- medal sub-checks (PS3 0x379xxx..0x381xxx) ---- */
static const char kr_killed[]="-killed";
/* v7.204 C++: the medal sub-checks are u3x_kr_* in u3x_fixes.cpp */
static void kr_379c18(uintptr_t base,ProfileState *s,uint8_t *knp,uint8_t *info){u3x_kr_379c18(base,s,knp,info);}   /* from above / below */
static void kr_37952c(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp){u3x_kr_37952c(base,s,S,knp);}   /* deaths since the last kill */
static void kr_3817b8(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp,uint8_t *info,int team_mode){u3x_kr_3817b8(base,s,S,knp,info,team_mode);}   /* explosive multi-kill */
static void kr_37992c(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp,uint8_t *info){u3x_kr_37992c(base,s,S,knp,info);}   /* pistol kills */
static void kr_37ab00(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp,uint8_t *info){u3x_kr_37ab00(base,s,S,knp,info);}   /* chained kills */
static void kr_37a4bc(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp){u3x_kr_37a4bc(base,s,S,knp);}   /* long range */
static void kr_379ed8(uintptr_t base,ProfileState *s,uint8_t *S){u3x_kr_379ed8(base,s,S);}   /* objective chains */
static void kr_379ff0(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp){u3x_kr_379ff0(base,s,S,knp);}   /* fewest kills on an enemy */
static void kr_379430(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp){u3x_kr_379430(base,s,S,knp);}   /* betrayal chain */
static void kr_3791ec(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp,uint8_t *vnp,uint8_t *info){u3x_kr_3791ec(base,s,S,knp,vnp,info);}   /* trade kills */
static void kr_37a914(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp){u3x_kr_37a914(base,s,S,knp);}   /* killed every enemy alive */
static void kr_37a16c(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp){u3x_kr_37a16c(base,s,S,knp);}   /* ended a streak */
static void kr_37a32c(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp,uint8_t *info){u3x_kr_37a32c(base,s,S,knp,info);}   /* revenge */

/* ---- step 2: points, cash/XP awards ---- */
#define KR_NOPTS 0x4000u
/* "+points" popup (PS3 f_66c73c -> f_66b940 -> f_65b808), inline on PS4 (0x6820fc): HUD object 0x1805fa0 + user*0x270
   (PS3 0x1297bb0 + user*0x148), list *(obj+0x238)+0x2690 (PS3 *(obj+0x12c)+0x2420): 12 slots of 0x70 {BoundFrame 0x60,
   +0x60 points (-1 = free), +0x64 clock 0x17b4a90+0x114, +0x68 type, +0x69 double}. Position: the victim's frame
   (process +0xb0, PS3 +0x70; translation at +0x20) raised by 1.5 (PS3 0xd8a350); without a victim character (PS3 uses
   an identity frame there) the killer's frame moved to the kill position. Globals 0x1806480 / 0x1b24bd8 = obj. */
#define KR_NOPOPUP 0x8000u
static void kr_popup(uintptr_t base,ProfileState *s,uint8_t *np,uint8_t *at,uint8_t *info,int32_t pts,uint32_t type,uint32_t dbl){
 u3x_kill_popup(base,s,np,at,info,pts,type,dbl);}   /* v7.169 C++ */
/* PS3 f_38115c: score +2/+4 of the player's record, then UpdatePlaces */
static void kr_addscore(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *np,int32_t pts){
 u3x_add_score(base,s,S,np,pts);}   /* v7.169 C++ */
/* team record (S+0x574c, 0x130 each) +2/+4 */
static void kr_teamscore(uintptr_t base,ProfileState *s,uint8_t *S,uint32_t t,int32_t pts){
 u3x_team_score(base,s,S,t,pts);}   /* v7.169 C++ */
/* PS3 f_37ba38 -> PS4 0x7abb70 (= f_37b4ec: cash, XP, rank-up popup, missions 0x15b46981, profile counters) */
static void kr_award(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *np,int32_t pts,uint32_t type,uint32_t flag){
 u3x_award(base,s,S,np,pts,type,flag);}   /* v7.169 C++ */
/* PS3 f_34fc04: mode type 0xf with a mode entry */
static int kr_34fc04(uintptr_t base){return u3x_kr_34fc04(base);}   /* v7.169 C++ */
static int32_t kr_dc_int(uintptr_t base,uint32_t off){return u3x_kr_dc_int(base,off);}
static int32_t kr_37c984(uintptr_t base){return u3x_kr_37c984(base);}
static int32_t kr_37c914(uintptr_t base){return u3x_kr_37c914(base);}
static int32_t kr_37c9f4(uintptr_t base){return u3x_kr_37c9f4(base);}
/* kill point getters (PS3 f_5befc/f_5be80/f_5be04/f_5bd88/f_5bd0c/f_5bc90/f_5bc14/f_5bb98 = DC 0xc85e199d +8..+0x24 times
   the booster multiplier) = PS4 0x6621a0 + 0x80*k */
static int32_t kr_kpts(uintptr_t base,uint32_t fn,uint8_t *np){return u3x_kill_points(base,fn,np);}
#define KP_5BEFC 0x6621a0u
#define KP_5BE80 0x662220u
#define KP_5BE04 0x6622a0u
#define KP_5BD88 0x662320u
#define KP_5BD0C 0x6623a0u
#define KP_5BC14 0x6624a0u
#define KP_5BB98 0x662520u
/* PS3 f_365474(0x1248678) double-points event -> PS4 0x17dd620 (+4 active, +0x14 team) */
static int kr_dblpts(uintptr_t base,uint8_t *np){
 return u3x_double_points(base,np);}   /* v7.169 C++ */
/* PS3 f_30ffa8 */
static int kr_30ffa8(uintptr_t base,uint8_t *np,int b){return u3x_kr_30ffa8(base,np,b);}
/* PS3 f_5b62c: AI kill points: DC 0xa977dad3 {id,..,+8 points} (default 100) + DC 0x829f99c6 {weapon,+4 bonus}
   x f_30ffa8 x difficulty (mgr+0x5c28 -> +0x6a48: 3 = 1.5, 4 = 2.0) */
static int32_t kr_5b62c(uintptr_t base,uint32_t w,uint32_t id,uint8_t *knp,int b){
 return u3x_ai_kill_points(base,w,id,knp,b);}   /* v7.169 C++ */

/* ---- booster / kickback kill rewards (PS3 f_381ae4 0x382c..0x3833xx and f_37d1cc) ----
   PS4 shapes taken from 0x7acd30 (PS4 counterpart of PS3 f_3803e8, same booster pattern): booster value = float
   0x689d20(0x17c3b78, id, np, out) (PS3 f_29e7f4 on 0x121b968); inventory = character +0x520 (PS3 +0x3d8), PS3 vtable
   offset k -> PS4 2k: add ammo vt+0xe8 (inv, {u32 weapon, i32 amount, u8 0}, out), give pickup vt+0xf0 (inv, pickup,
   0, 0, out), current weapon vt+0x80 / vt+0x78, slot weapon vt+0x70 (inv, 2); weapon id = weapon +0x6ec (PS3 +0x568,
   f_6cd964); weapon def 0xd436b0 (PS3 f_6cf864, ammo per pickup +0x58, +4). Kickback meter = 0x17d79f0 + user*0x20 [0]
   (PS3 0x12306ac, f_32806c clamps at 0). */
#define KR_NOBOOST 0x10000u
/* PS3 0x382bd0..0x383300: rewards for the killer's linked local player (np +0x674) */
static void kr_booster_kill(uintptr_t base,ProfileState *s,uint8_t *knp,uint32_t fl,uint32_t w){
 u3x_booster_kill(base,s,knp,fl,w);}   /* v7.158 C++ */
/* PS3 f_37d1cc: boosters for a local killer's weapon */
static void kr_37d1cc(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp){
 u3x_weapon_boosters(base,s,S,knp);}   /* v7.158 C++ */

/* ---- weapon-definition medal checks (PS3 f_37ae3c, f_3814d8) ----
   weapon def f_6ce8b4 = DC 0x61d589d9 hash map {u32 n, u32 keys[] @8, def* values[] @0x10} (PS4 inline 0x437180),
   category at def+8; per-net-game weapon data f_6cf7e4 = PS4 0xd415e0 (binary search in DC 0x9e354807 / 0xb4ca46a6),
   PS3 fields +4k -> PS4 +8k (0xd41680/0xd41720/0xd417c0/0xd41860 vs PS3 f_6cf940/f_6cf90c/f_6cf8d8/f_6cf8a4; +0x10 ->
   +0x20 also at PS4 0x7ad208). The pointed-to data (*e) keeps PS3 offsets (+4 at PS4 0x7ad22f); its +0x17c "type" is
   assumed unchanged. */
static uint8_t *kr_wdef(uintptr_t base,uint32_t w){
 return u3x_weapon_definition(base,w);}   /* v7.158 C++ */
static void kr_37ae3c(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp,uint8_t *info,int team_mode){
 u3x_category_medals(base,s,S,knp,info,team_mode);}   /* v7.158 C++ */
/* f_3814d8: three of each (explosive, category 0, category 1) */
static void kr_3814d8(uintptr_t base,ProfileState *s,uint8_t *S,uint8_t *knp){
 u3x_three_of_each(base,s,S,knp);}   /* v7.158 C++ */

/* ---- the port ---- */
static void kr_port(uintptr_t base,uint8_t *info,ProfileState *s){
 uint32_t bits=kr_bits(s);X.kr[0]++;
 if(bits&KR_OFF){X.kr[15]++;return;}
 uint8_t *ng=kr_ng(base),*S=kr_S(base);
 uint8_t *mode=kr_mode(base);if(!mode)return;
 X.kr[1]++;
 int kd=!(bits&KR_NOKD),med=!(bits&KR_NOMEDAL);
 int team_mode=((uint8_t(*)(void*))(base+0x722670u))(ng)!=0;   /* local_cb, PS3 f_309bbc */
 int ffa=*(int32_t*)(mode+0xc)==0xe;                          /* cVar30, PS3 f_3052a8 */
 uint32_t fl=*(uint32_t*)(info+0x60),id30=*(uint32_t*)(info+0x30),id34=*(uint32_t*)(info+0x34),w=*(uint32_t*)(info+0x38);
 X.kr[7]=fl;X.kr[8]=id30;X.kr[10]=id34;
 /* history: shift, then record 0 = this kill (PS3 0x381b2c..0x381e6c) */
 uint32_t n=kr_n(S);KRec *r0=kr_rec(S,0);
 if(!(bits&KR_NOHIST)){
  for(int64_t i=(int64_t)n-1;i>=0;i--)if(i<0x1fe){uint8_t *a=(uint8_t*)kr_rec(S,(uint32_t)i),*b=(uint8_t*)kr_rec(S,(uint32_t)i+1);for(int j=0;j<0x26;j++)b[j]=a[j];}
  r0->type=0;r0->killer=(fl&1)?(uint16_t)id30:0;r0->victim=(fl&2)?(uint16_t)id34:0;r0->time=*(int32_t*)info;
  r0->widx=(uint8_t)kr_statidx(base,w);
  for(int i=0;i<3;i++){r0->pos[i]=(int16_t)f_trunc(f_mul(((uint32_t*)(info+0x10))[i],F_16));
                       r0->pos[3+i]=(int16_t)f_trunc(f_mul(((uint32_t*)(info+0x20))[i],F_16));}
  r0->kteam=-1;r0->vteam=-1;r0->f1c=*(int32_t*)(info+0x6c);
  r0->f23=(fl>>27)&1;r0->f24=(fl>>13)&1;
  if(fl&0x20)r0->type=6;else if(fl&4)r0->type=1;else if(fl&0x40)r0->type=2;
  r0->f25=0;
  if(n<0x200)*(uint32_t*)(S+0xe0)=++n;
  X.kr[4]=n;}
 uint8_t *knp=(fl&1)?kr_np_by_id(base,id30):0,*vnp=(fl&2)?kr_np_by_id(base,id34):0;
 uint8_t *vent=0;   /* entity victim (non-player): PS3 f_2fefe4 */
 if(!(fl&2)){
  uint8_t *ng2=ng;uint32_t h=0x811c9dc5u,v=id34;for(int i=0;i<4;i++){h=((v&0xff)^h)*0x1000193u;v>>=8;}
  uint32_t size=*(uint32_t*)(ng2+0x655c);uint64_t *tab=*(uint64_t**)(ng2+0x6520);
  if(size&&tab){uint64_t *node=(uint64_t*)tab[h%size];while(node&&*(uint32_t*)(node+2)!=id34)node=(uint64_t*)*node;
   if(node&&node!=(uint64_t*)tab[size])vent=(uint8_t*)(node+3);}
  /* PS3 f_381ae4 0x381f..: rec0 +0x25 (PS3 +0x109) = 1 when the entity victim's handle {obj, id} is valid (obj+0x50 == id) and
     the Npc has +0x578 bit 15. s005: ported with the v5.43 map (+0x578 bit 15 = PS4 +0x708 bit 48; handle check = handle_obj,
     obj+0x7c == id, as kr's co-op AI part). U3MP_KR_VICTIMFLAG=0 (RS2 0x10000000) leaves it out. */
  if(vent&&!(*(uint32_t*)((uint8_t*)s+0x3d78)&0x10000000u)){uint8_t *vc=handle_obj(vent);if(vc&&((*(uint64_t*)(vc+0x708)>>48)&1))r0->f25=1;}
 }
 if(kr_30feec(base)&&!(fl&0x200))r0->f25=1;
 uint8_t *krec=kr_prec(S,knp),*vrec=kr_prec(S,vnp);
 if(knp)r0->kteam=(int8_t)kr_team(knp);
 if(vnp)r0->vteam=(int8_t)kr_team(vnp);
 r0->f22=(fl>>20)&1;r0->f21=(fl>>19)&1;
 int local_cc=0,on=0;
 if(knp&&vnp&&knp==vnp){   /* suicide */
  local_cc=1;if(kd){(*(int16_t*)(vrec+0xc))++;(*(int16_t*)(krec+0x10))++;X.kr[5]++;}
  goto L20f0;}
 if(ffa){r0->kteam=0;r0->vteam=1;}
 if(vnp&&kd){(*(int16_t*)(vrec+0xc))++;X.kr[5]++;}
 if(!knp)goto L3b44;
 if(vnp){local_cc=0;goto L20f0;}
 on=kr_online(base)||(X.ec[245]&1u);   /* cs02: show-only co-op cash */local_cc=0;
 goto L298c;
L20f0:
 if(kr_friendly(base,knp,vnp)){
  if(kd)(*(int16_t*)(krec+0x12))++;
  if(r0->f21){   /* betrayal: credit the last attacker (PS3 f_358d0c) */
   uint32_t a=(*(int32_t*)(knp+0x9a0)-(int32_t)kr_now(base)>0)?*(uint32_t*)(knp+0x9a4):0;
   uint8_t *anp=kr_np_by_id(base,a);
   if(!anp){
    if(!(fl&0x4000))kr_teamscore(base,s,S,1-kr_team(knp),(fl&0x200000)?(kr_34fc04(base)?0:3):kr_37c9f4(base));
    else if(!kr_34fc04(base))kr_teamscore(base,s,S,1-kr_team(knp),kr_37c984(base));   /* + f_34fd9c: gone on PS4 */
   }else if(kr_team(anp)!=kr_team(knp)){
    uint8_t *arec=kr_prec(S,anp);if(kd)(*(int16_t*)(arec+8))++;
    if(fl&0x4000){arec[0x57]++;kr_addscore(base,s,S,anp,kr_37c984(base));kr_stat(base,s,0xb477093du,arec[0x57],anp);}
    else kr_addscore(base,s,S,anp,(fl&0x200000)?(kr_34fc04(base)?0:3):kr_37c9f4(base));
   }}}
 on=kr_online(base)||(X.ec[245]&1u);   /* cs02: show-only co-op cash */
 /* PS3 here sets victim np +0x4e0 + killer user*4 = 1 when the killer is local (unmapped on PS4) */
 if(!local_cc&&!kr_friendly(base,knp,vnp))goto L2528;
 if(!r0->f21){   /* suicide / team kill: the other team scores */
  int ti=kr_team(vnp)==0;
  if(!(fl&0x4000)){if(fl&0x200000)kr_teamscore(base,s,S,ti,kr_34fc04(base)?0:3);}
  else kr_teamscore(base,s,S,ti,kr_37c984(base));
 }else{if(!local_cc){if(vnp)goto L2528;goto L298c;}if(!vnp)goto L298c;}
 goto L296c;
L2528:
 if(!kr_friendly(base,knp,vnp)){
  if(!(fl&0x4000)){
   int32_t pts=0;int mt=*(int32_t*)(mode+0xc);
   if(!((uint8_t(*)(void*))(base+0x729b10u))(ng)){
    if(mt!=10||!on){
     if(!team_mode&&on){if(!ffa){pts=kr_kpts(base,KP_5BE80,knp);goto L27a0;}kr_addscore(base,s,S,knp,1);}
     else if(!ffa){goto L28d4;}else{kr_addscore(base,s,S,knp,1);if(!on)goto L28d4;}}
    /* c001 (agent C): PS3 1.19 f_381ae4 copy 0x390c08 changes this branch. 1.10 0x382704..0x382728 (mode type 10 with
     * f_30b3a4 set) took the plain kill points f_5befc; 1.19 0x391834..0x391864 doubles them: bl f_5c8a4 (= 1.10 f_5befc,
     * PS4 0x6621a0), slwi r26,r3,1, then joins at 0x3918e0 (the 16-bit test of r26). U3MP_KR119=0 (agent C switch word
     * ec[0] bit0) keeps the 1.10 behaviour. f_30b3a4 = PS4 0x7270f0 is false in LAN games, so on LAN this branch is not taken
     * on PS3 1.19 either. */
    else if(!(X.ec[0]&1)){pts=kr_kpts(base,KP_5BEFC,knp)<<1;X.ec[16]++;goto L27a0;}
    L2798:pts=kr_kpts(base,KP_5BEFC,knp);
   }else{
    kr_addscore(base,s,S,knp,(fl&0x200000)?(kr_34fc04(base)?0:3):kr_37c9f4(base));
    if(fl&0x40000){kr_addscore(base,s,S,knp,kr_37c914(base));krec[0x57]++;kr_stat(base,s,0xb477093du,krec[0x57],knp);}
    if(!on)goto L28d4;
    if(mt!=0xf)goto L2798;
    pts=((uint8_t(*)(void*,void*,void*))(base+0x77c880u))((uint8_t*)(base+0x17dc230u),knp,vnp)?kr_kpts(base,KP_5BE04,knp):kr_kpts(base,KP_5BD88,knp);
   }
  L27a0:
   if(pts){
    uint32_t dbl=0;if(kr_dblpts(base,knp)){pts<<=1;dbl=1;}
    if(r0->f21){pts<<=1;dbl=1;}
    if(kr_local(base,knp))kr_popup(base,s,knp,vnp?vnp:0,info,pts,0,dbl);
    kr_award(base,s,S,knp,pts,0,0);}
  }else{kr_addscore(base,s,S,knp,kr_37c984(base));krec[0x57]++;kr_stat(base,s,0xb477093du,krec[0x57],knp);}
 L28d4:
  if(!ffa){   /* avenged a teammate within 2.5 s */
   uint32_t c=n?n:1;
   for(uint32_t i=1;;i++){if(--c==0)break;KRec *r=kr_rec(S,i);
    if((int32_t)(*(int32_t*)info-r->time)>0x2ee)break;
    if(r->kteam!=r->vteam&&r->killer==(uint16_t)id34&&r->victim!=(uint16_t)id30&&r->vteam==kr_team(knp)){kr_stat(base,s,0x5258a989u,1,knp);break;}}}
 }
L296c:
 if(!kr_friendly(base,knp,vnp))goto L298c;
 goto L3698;
L298c:
 if(kd){(*(int16_t*)(krec+8))++;X.kr[6]++;
  if(*(uint8_t*)(base+0x17d5c61u)&&((uint8_t(*)(void*))(base+0x722670u))(ng)&&kr_team(knp)==1)(*(int16_t*)(krec+0x6c))++;   /* f_35a4f8 */
  int m=kr_member(S,id30);
  if(m>=0){uint32_t wi=0;const uint32_t *t=(const uint32_t*)(base+0x13a66d0u);for(uint32_t j=1;j<=0x1a;j++)if(t[j]==w){wi=j;break;}
   (*(int16_t*)(S+0x60cc+(uint64_t)m*0x130+0xee + wi*2))++;}}
 kr_stat(base,s,0x5c494554u,*(uint16_t*)(krec+8),knp);
 if(kr_local(base,knp))((void(*)(void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t))(base+0x773b50u))(
   (uint8_t*)(base+0x17da200u)+(uint64_t)kr_user(knp)*0x1018,w,1,0,w,(uint8_t)*(uint32_t*)(info+0x3c));
 if(!r0->f25){
  if(fl&0x2000000){krec[0x65]++;kr_stat(base,s,0x15fd6bcdu,krec[0x65],knp);}
  if(fl&0x40000000)kr_stat(base,s,0x1e8153a6u,1,knp);
  int c2c28=1;
  if(fl&0x8000000){
   uint32_t c=0;for(uint32_t i=0;i<n;i++)c+=kr_rec(S,i)->f23!=0;
   kr_stat(base,s,0xd4f91f9au,c,knp);
   if(kr_group6(w))kr_stat(base,s,0xbb000af1u,1,knp);
   else if(kr_group4(w))kr_stat(base,s,0x5398047bu,1,knp);
   else c2c28=0;}
  if(c2c28&&w==0x3d2f108bu)kr_stat(base,s,0xc2b00a95u,1,knp);
  if(fl&0x4000000)kr_stat(base,s,0x78573763u,1,knp);
  if(fl&0x10000000)kr_stat(base,s,0xd323c81du,1,knp);
  if(fl&0x20000000)kr_stat(base,s,0xdc380255u,1,knp);
  if(fl&0x100){krec[0x56]++;kr_stat(base,s,0x76f0c8c8u,krec[0x56],knp);}
  if(fl&0x200){(*(uint16_t*)(krec+0x58))++;kr_stat(base,s,0x77240247u,*(uint16_t*)(krec+0x58),knp);}
  if(fl&0x400)kr_stat(base,s,0xcff265edu,1,knp);
  if(fl&0x8000)kr_stat(base,s,0x87cbf9cbu,1,knp);
  if(fl&0x800)kr_stat(base,s,0x3838b544u,1,knp);
  if(fl&0x1000)kr_stat(base,s,0x68774d94u,1,knp);
  if(fl&0x20000)kr_stat(base,s,0xb4210460u,1,knp);
  if(fl&0x400000)kr_stat(base,s,0x6685348eu,1,knp);
  if(fl&0x800000)kr_stat(base,s,0x0b854093u,1,knp);
  if(fl&0x1000000)kr_stat(base,s,0xcd644ff7u,1,knp);
  kr_stat(base,s,kr_sid_cat(base,*(uint32_t*)(info+0x70),kr_killed),1,knp);
  if(fl&0x2000){
   uint32_t c=0;for(uint32_t i=0;i<n;i++){KRec *r=kr_rec(S,i);if(r->f24&&r->killer==(uint16_t)id30&&r->kteam!=r->vteam)c++;}
   kr_stat(base,s,0xa622ff01u,c,knp);}
  else if(fl&0x10000)kr_stat(base,s,0xe5ad0b24u,1,knp);
  if(kr_group4(w)){
   const uint32_t *k=(const uint32_t*)(info+0x10),*v=(const uint32_t*)(info+0x20);uint32_t d2=0;
   for(int i=0;i<3;i++){uint32_t d=f_sub(k[i],v[i]);d2=f_add(d2,f_mul(d,d));}
   if(f_le(0x44610000u,d2))kr_stat(base,s,0xd294ccd1u,1,knp);}
  if(!ng[0x31d]){kr_stat(base,s,0x1bdbd578u,1,knp);ng[0x31d]=1;}   /* f_2fa4bc first blood */
  if(med){kr_379c18(base,s,knp,info);kr_37952c(base,s,S,knp);X.kr[11]++;}
 }
 kr_booster_kill(base,s,knp,fl,w);
 if(vent&&kd)(*(int16_t*)(krec+0xa))++;
 if(team_mode){
  /* v5.43 step 1b: the co-op AI victim (PS3 0x3833f4..0x383448, 0x3835a4..0x383690, 0x383edc). PS3 resolves the entity
     victim's handle to its Npc (r25) and skips the whole co-op points block when Npc +0x578 bit 15 is set. After the points it
     emits: bit 14 -> 0x0c65fecd; a kickback object (+0x91c, +4 = kickback id set by npc-activate-random-kickback) -> 0x9c498eb3;
     otherwise, unless the Npc has tag 0xfd2c9c09, the +0x980 class: 2 with weapon 0xb934b2b2 -> 0x35cfda1f with
     ++krec[0x64], 4 -> 0x38771768. PS4 map: +0x578 -> +0x708 (bit b -> 63-b: 15 -> 48, 14 -> 49), +0x91c -> +0xbf8 (PS4
     npc-activate-random-kickback 0x41b4d0 writes +0xbf8 -> +4), +0x2c tags -> +0x38 with the tag test inlined as at PS4
     0x9f8d1c, +0x980 -> +0xc60. U3MP_KR_COOPAI=0 (RS2 0x8000000) leaves this out. Counts at rw2+0x31e0 X.cai: [0] Npc victims
     seen, [1] skipped by bit 48, [2] co-op AI stats emitted, [3] last stat id. */
  uint8_t *vch=(vent&&!(kr_bits(s)&0x8000000u))?handle_obj(vent):0;
  if(vch){X.cai[0]++;if((*(uint64_t*)(vch+0x708)>>48)&1){X.cai[1]=(X.cai[1]&0xffff0000u)|((X.cai[1]+1)&0xffffu);goto L3698;}}
  int32_t t0=*(int32_t*)info;
  if(!(kr_bits(s)&KR_NOPTS))((void(*)(void*,uint32_t,int32_t*))(base+0x661ca0u))((uint8_t*)(base+0x17c3404u),info[0x64],&t0);  /* f_5b2d8 */
  int32_t pts=kr_5b62c(base,w,*(uint32_t*)(info+0x68),knp,(fl>>9)&1);
  if(kr_local(base,knp)){if(on)kr_popup(base,s,knp,0,info,pts,0,0);kr_prof_add(base,knp,0x17b,1);}
  if(on)kr_award(base,s,S,knp,pts,0,0);
  if(vch){
   if((*(uint64_t*)(vch+0x708)>>49)&1){kr_stat(base,s,0x0c65fecdu,1,knp);X.cai[2]++;X.cai[3]=0x0c65fecdu;}
   uint8_t *kb=*(uint8_t**)(vch+0xbf8);
   if(kb&&*(uint32_t*)(kb+4)){kr_stat(base,s,0x9c498eb3u,1,knp);X.cai[2]++;X.cai[3]=0x9c498eb3u;}
   else if(!kr_tag(base,*(uint8_t**)(vch+0x38),0xfd2c9c09u,0x9e8)){
    int32_t cls=*(int32_t*)(vch+0xc60);
    if(cls==2){if(w==0xb934b2b2u&&krec){krec[0x64]++;kr_stat(base,s,0x35cfda1fu,krec[0x64],knp);X.cai[2]++;X.cai[3]=0x35cfda1fu;}}
    else if(cls==4){kr_stat(base,s,0x38771768u,1,knp);X.cai[2]++;X.cai[3]=0x38771768u;}}}
 }
L3698:
 if(!r0->f25){
  if(!local_cc){
   if(vnp&&!kr_friendly(base,knp,vnp)&&(fl&0x80)){(*(uint16_t*)(krec+0x54))++;kr_stat(base,s,0x744a942bu,*(uint16_t*)(krec+0x54),knp);}
   if(fl&4){(*(uint16_t*)(krec+0x48))++;kr_stat(base,s,0xb759f1c7u,*(uint16_t*)(krec+0x48),knp);}
   if(fl&0x40){(*(uint16_t*)(krec+0x4e))++;kr_stat(base,s,0xaa22db16u,*(uint16_t*)(krec+0x4e),knp);}}
  if(fl&0x10)kr_stat(base,s,0x5cff7436u,1,knp);
  if(med)kr_3817b8(base,s,S,knp,info,team_mode);
  if(kr_group6(w)){
   if(kr_local(base,knp)){kr_prof_add(base,knp,0x17c,1);if(w==0xc58ae515u)kr_prof_add(base,knp,0x17d,1);}
   if(med)kr_37992c(base,s,S,knp,info);}
  else if(med)kr_37ae3c(base,s,S,knp,info,team_mode);
  if(med){kr_37ab00(base,s,S,knp,info);kr_37a4bc(base,s,S,knp);kr_379ed8(base,s,S);kr_379ff0(base,s,S,knp);kr_379430(base,s,S,knp);X.kr[12]++;}
  if(r0->f22&&r0->kteam==r0->vteam&&!local_cc&&!r0->f21)kr_stat(base,s,0xffc1c7b5u,1,knp);
  if(vnp){
   if((fl&8)&&r0->kteam!=r0->vteam){kr_stat(base,s,0x89c96dedu,1,knp);krec[0x6a]++;}
   uint32_t c=0;
   for(uint32_t i=0;i<n;i++){KRec *r=kr_rec(S,i);if(r->f25)continue;
    if(r->victim==(uint16_t)id30)break;
    if(r->killer==(uint16_t)id30){if(r->vteam==r0->kteam)break;c++;}}
   kr_stat(base,s,0x58b0d53du,c,knp);
   if(med)kr_37a32c(base,s,S,knp,info);
   c=0;
   for(uint32_t i=0;i<n;i++){KRec *r=kr_rec(S,i);if(r->f25)continue;
    if(r->killer==(uint16_t)id34)break;
    if(r->victim==(uint16_t)id34){if(r->vteam==r0->kteam)break;c++;}}
   kr_stat(base,s,0x6f593a9bu,c,vnp);
   if(med){kr_3791ec(base,s,S,knp,vnp,info);kr_37a914(base,s,S,knp);kr_3814d8(base,s,S,knp);kr_37a16c(base,s,S,knp);X.kr[13]++;}
  }
 }
 if(kr_local(base,knp))kr_37d1cc(base,s,S,knp);
L3b44:
 if(!ffa){   /* assists */
  for(int i=0;i<8;i++){
   int32_t kt=r0->kteam;if(kr_mode_type(base)==0x12&&kt==-1)kt=1;
   uint32_t aid=*(uint32_t*)(info+0x40+i*4);uint8_t *anp=aid?kr_np_by_id(base,aid):0;
   if(!anp||kr_team(anp)==r0->vteam||kr_team(anp)!=kt)continue;
   uint8_t *arec=kr_prec(S,anp);if(kd){(*(uint16_t*)(arec+0xe))++;X.kr[14]++;}
   if(kr_online(base)||(X.ec[245]&1u)){   /* cs02 */
    int32_t pts;
    if(kr_mode_type(base)==0xf)pts=((uint8_t(*)(void*,void*,void*))(base+0x77c880u))((uint8_t*)(base+0x17dc230u),anp,vnp)?kr_kpts(base,KP_5BC14,anp):kr_kpts(base,KP_5BB98,anp);
    else pts=kr_kpts(base,KP_5BD0C,anp)*kr_30ffa8(base,knp,(fl>>9)&1);
    if(kr_dblpts(base,knp))pts<<=1;
    if(kr_local(base,anp))kr_popup(base,s,anp,vnp,info,pts,1,0);
    kr_award(base,s,S,anp,pts,0,0);}
   kr_stat(base,s,0x83daf02cu,*(uint16_t*)(arec+0xe),anp);
   /* PS3: victim np +0x4e0 + assister user*4 = 2 when the assister is local (unmapped) */
  }}
 ((void(*)(void*))(base+0x7aa560u))(S);   /* f_37d61c UpdatePlaces */
}

/* ---- v5.23 mission completion (PS3 f_34b91c, called by the mission tracker f_34c45c = PS4 0x773b50 when a mission's
   progress reaches its target; PS4 dropped the call, its text ids 0xc423e4f5 / 0x065ba916 and the cash reward).
   PS4 shapes: tracker user index +0x1004 (PS3 +0xa04); mission def same layout as PS3 (+0 id, +4 name text, +0x1c level,
   +0x24 cash reward, +0x34 booster id); profile data = *(0x17dd610) + user*0x7b00 + 0x248 with PS3 data offsets
   (+0x161c current mission, +0x1620, +0xd04, +0xc84 completed bits, +0x504 cash via 0x660fb0 = f_5af2c);
   text db 0x17026b8: lookup 0xf32ae0 (= f_799f8c), [TEXT] formatter 0xf32bd0 (= f_79a9a8), cash "$1,000" 0xf32080
   (= f_796d78); booster table 0x687d30(0x17c3b40, id) (= f_29d33c, record 0x38: +8 name, +0x28 icon; PS3 +0x20);
   HUD message queue (= f_262d48, inlined on PS4 at 0x774d89): menu 0x17bd080 / 0x17bfa10 (PS3 0x12151a0 / 0x1216a50),
   4 slots: text +0x26a8+i*0x80, params +0x28a8+i*0x30, first-slot clock +0x26a4; icon (= f_97202c(.,id,1)) from the
   tables *(0x1b24bc8)+0x28 / +0x20 like PS4 0x774c9c. */
static const char *ms_text(uintptr_t base,uint32_t id){
 return u3x_ms_text(base,id);}   /* v7.174 C++ */
static int ms_len(const char *a){return u3x_ms_len(a);}
/* replace the first tag in buf (size n) with rep, like PS3 f_799ff8 */
static void ms_rep(char *buf,int n,const char *tag,const char *rep){
 u3x_ms_replace(buf,n,tag,rep);}
static void ms_itoa(char *b,int32_t v){u3x_ms_itoa(b,v);}
static int32_t ms_icon_find(uint8_t *t,uint32_t id){
 return u3x_ms_icon_find(t,id);}
static void ms_icon(uintptr_t base,uint32_t id,uint32_t ic[3]){
 u3x_ms_icon(base,id,ic);}
static int ms_hud(uintptr_t base,int user,const char *txt,uint32_t type,int32_t level,const uint32_t ic[3]){
 return u3x_ms_hud(base,user,txt,type,level,ic);}
static void ms_34b91c(uintptr_t base,ProfileState *s,uint8_t *trk,int32_t *def){
 (void)s;u3x_mission_done(base,trk,def);}   /* v7.174 C++ */
