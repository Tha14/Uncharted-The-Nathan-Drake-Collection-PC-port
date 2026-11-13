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
EXT static void kr_port(uintptr_t base,uint8_t *info,ProfileState *s){u3x_kr_port(base,info,(void*)s);}   /* v7.212 C++ */

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
