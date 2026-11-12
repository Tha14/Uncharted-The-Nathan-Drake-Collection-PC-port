/* plat_ps4.inc - PS4 backend of plat.h. Included from u3_lan_profiles.c (same spot as plat_pc.inc) when
 * U3_PLAT_PS4 is defined. Part B (2026-11-05): every body is written; what is left is the first run on a
 * console (nothing here has executed on real hardware: see PLAN.md section 6 for the checklist).
 *
 * Bootstrap (image2elf.py writes the table at PLAT_TABLE_VA, its own rw segment):
 *   +0x00 "U3PLAT1\0"   +0x08 u64 count   +0x20 sceKernelLoadStartModule   +0x28 sceKernelDlsym
 *   +0x30 sceSysmoduleLoadModule   +0x60 count x {u64 slot, char nid[12], char lib[20]}
 * The three boot pointers are filled by the PS4 loader (R_X86_64_64 against big3's own import symbols:
 * NIDs wzvqT4UqKX8 / LwG8g3niqwA / g8cM39EUZ6o, verified against ps4libdoc known_names.txt). plat_init walks
 * the rows (the builder's IMPORTS / NET_IMPORTS / KIMPORTS slots that big3 does not import itself) and the
 * helper's own needs below, resolves each NID to its C name (sceKernelDlsym takes the name) and stores the
 * pointer in the slot. Game logic sees the same ProfileState / ExtState slots as on PC.
 *
 * Module handles for sceKernelDlsym: libkernel = 0x2001 and libc / libSceLibcInternal = 0x2 are the fixed
 * handles the PS4 loader assigns to the first modules of every process (the convention of the payload SDKs);
 * each is probed with a known symbol first and, if it does not answer, the library is opened by name with
 * sceKernelLoadStartModule("<lib>.sprx"), which also returns the handle of an already loaded module. System
 * modules (libSceImeDialog = 0x0096 SCE_SYSMODULE_IME_DIALOG) get sceSysmoduleLoadModule(id) first.
 * libSceNet / libSceNetCtl are NEEDED by big3 already.
 *
 * NIDs: standard PS4 scheme, base64(first 8 bytes of SHA-1(name + 518D64A635DED8C1E6B039B1C3E55230), reversed)
 * with the +- alphabet; every name here is in third_party/ps4libdoc/known_names.txt (checked 2026-11-03). */
#ifndef U3_PLAT_PS4_INC
#define U3_PLAT_PS4_INC
#define PLAT_TABLE_VA 0x2400000u          /* image2elf.py PLAT_TABLE_VA: fixed, above every helper segment */
#define PLAT_H_LIBKERNEL 0x2001
#define PLAT_H_LIBC 0x2
typedef struct { uint64_t slot; char nid[12]; char lib[20]; } PlatRow;
typedef struct { char magic[8]; uint64_t count,r0,r1; uintptr_t load_start_module,dlsym,sysmodule_load; uintptr_t pad[5]; PlatRow row[1]; } PlatTable;

/* The helper's own slots (not the builder's): resolved by plat_init into plat_ps4.fn[]. */
enum{PF_ERRNO,PF_RESOLVER_CREATE,PF_RESOLVER_NTOA,PF_RESOLVER_DESTROY,PF_MAP_FLEX,PF_PTHREAD_CREATE,PF_PTHREAD_ATTR_INIT,PF_PTHREAD_ATTR_STACK,
 PF_PTHREAD_ATTR_DETACH,PF_PTHREAD_ATTR_DESTROY,PF_USLEEP,PF_GETTIMEOFDAY,PF_MKDIR,PF_NET_RANDOM,PF_COUNT};
typedef struct { char nid[12],name[28],lib[20]; } PlatName;   /* inline strings: no pointer (absolute) relocations in .rodata */
static const PlatName plat_ps4_need[PF_COUNT]={
 {"HQOwnfMGipQ","sceNetErrnoLoc","libSceNet"},
 {"C4UgDHHPvdw","sceNetResolverCreate","libSceNet"},
 {"Nd91WaWmG2w","sceNetResolverStartNtoa","libSceNet"},
 {"kJlYH5uMAWI","sceNetResolverDestroy","libSceNet"},
 {"IWIBBdTHit4","sceKernelMapFlexibleMemory","libkernel"},
 {"6UgtwV+0zb4","scePthreadCreate","libkernel"},               /* PLAN.md once had 4J2sUJmuHZQ: that is sceKernelGetProcessTime */
 {"nsYoNRywwNg","scePthreadAttrInit","libkernel"},
 {"UTXzJbWhhTE","scePthreadAttrSetstacksize","libkernel"},
 {"-Wreprtu0Qs","scePthreadAttrSetdetachstate","libkernel"},
 {"62KCwEMmzcM","scePthreadAttrDestroy","libkernel"},
 {"1jfXLRVzisc","sceKernelUsleep","libkernel"},
 {"ejekcaNQNq0","sceKernelGettimeofday","libkernel"},
 {"1-LFLmRFxxM","sceKernelMkdir","libkernel"},
 {"G3O2j9f5z00","sceNetGetRandom","libSceNet"}};
/* NID -> C name for the builder's rows (IMPORTS, NET_IMPORTS, KIMPORTS of build_u3_profile_probe.py v7.53+) */
static const PlatName plat_ps4_known[]={
 {"NUeBrN7hzf0","sceImeDialogInit","libSceImeDialog"},{"IADmD4tScBY","sceImeDialogGetStatus","libSceImeDialog"},{"x01jxu+vxlc","sceImeDialogGetResult","libSceImeDialog"},{"gyTyVn+bXMw","sceImeDialogTerm","libSceImeDialog"},
 {"1G3lF1Gg1k8","sceKernelOpen","libkernel"},{"Cg4srZ6TKbU","sceKernelRead","libkernel"},{"4wSze92BhLI","sceKernelWrite","libkernel"},{"UK2Tl2DWUns","sceKernelClose","libkernel"},{"52NcYU9+lEo","sceKernelRename","libkernel"},
 {"Q4qBuN-c0ZM","sceNetSocket","libSceNet"},{"45ggEzakPJQ","sceNetSocketClose","libSceNet"},{"bErx49PgxyY","sceNetBind","libSceNet"},{"gvD1greCu0A","sceNetSendto","libSceNet"},{"304ooNZxWDY","sceNetRecvfrom","libSceNet"},
 {"2mKX2Spso7I","sceNetSetsockopt","libSceNet"},{"obuxdTiwkF8","sceNetCtlGetInfo","libSceNetCtl"},{"OXXX4mUk3uk","sceNetConnect","libSceNet"},{"kOj1HiAGE54","sceNetListen","libSceNet"},{"PIWqhn9oSxc","sceNetAccept","libSceNet"},
 {"beRjXBn-z+o","sceNetSend","libSceNet"},{"9wO9XrMsNhc","sceNetRecv","libSceNet"},
 {"taRWhTJFTgE","sceKernelGetdirentries","libkernel"},{"AUXVxWeJU-A","sceKernelUnlink","libkernel"},{"eV9wAD2riIA","sceKernelStat","libkernel"},
 {"wLlFkwG9UcQ","time","libc"},{"efhK-YSUYYQ","localtime","libSceLibcInternal"},{"1mecP7RgI2A","gmtime","libc"},{"1-LFLmRFxxM","sceKernelMkdir","libkernel"},{"1jfXLRVzisc","sceKernelUsleep","libkernel"}};
/* diagnostics (peek): base, ready, missing count, rows, mkdir result, handles, slots, last errno, first missing NIDs */
static struct { uintptr_t base; int ready,missing,rows,mkdir_r; int handles[8]; char libs[8][20]; uintptr_t fn[PF_COUNT]; int err; char miss[8][12]; } plat_ps4 EXT_BSS;
static int plat_streq(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return (int)(*a-*b);}
static int plat_sysmodule_id(const char *lib){
 if(!plat_streq(lib,"libSceImeDialog"))return 0x0096;          /* SCE_SYSMODULE_IME_DIALOG */
 return 0;}                                                     /* libSceNet / libSceNetCtl / libkernel / libc: already in the process */
static const char *plat_prx_name(const char *lib){
 if(!plat_streq(lib,"libSceNet"))return "libSceNet.sprx";
 if(!plat_streq(lib,"libSceNetCtl"))return "libSceNetCtl.sprx";
 if(!plat_streq(lib,"libSceImeDialog"))return "libSceImeDialog.sprx";
 if(!plat_streq(lib,"libkernel"))return "libkernel.sprx";
 if(!plat_streq(lib,"libc"))return "libc.sprx";
 if(!plat_streq(lib,"libSceLibcInternal"))return "libSceLibcInternal.sprx";
 return 0;}
static int plat_module(PlatTable *t,const char *lib){
 for(int i=0;i<8;i++)if(plat_ps4.handles[i]&&!plat_streq(plat_ps4.libs[i],lib))return plat_ps4.handles[i];
 int h=0,id=plat_sysmodule_id(lib);
 if(id)((int(*)(int))t->sysmodule_load)(id);
 if(!plat_streq(lib,"libkernel"))h=PLAT_H_LIBKERNEL;
 else if(!plat_streq(lib,"libc")||!plat_streq(lib,"libSceLibcInternal"))h=PLAT_H_LIBC;
 if(h){void *probe=0;const char *sym=h==PLAT_H_LIBKERNEL?"sceKernelUsleep":"memcpy";   /* does the fixed handle answer? else open by name */
  if(((int(*)(int,const char*,void**))t->dlsym)(h,sym,&probe)<0||!probe)h=0;}
 if(!h){const char *prx=plat_prx_name(lib);if(prx)h=((int(*)(const char*,uint64_t,const void*,uint32_t,const void*,int*))t->load_start_module)(prx,0,0,0,0,0);}
 if(h<=0)return 0;
 for(int i=0;i<8;i++)if(!plat_ps4.handles[i]){plat_ps4.handles[i]=h;int k=0;for(;k<19&&lib[k];k++)plat_ps4.libs[i][k]=lib[k];plat_ps4.libs[i][k]=0;break;}
 return h;}
static const char *plat_ps4_name(const char *nid){
 for(unsigned i=0;i<sizeof(plat_ps4_known)/sizeof(plat_ps4_known[0]);i++)if(!plat_streq(plat_ps4_known[i].nid,nid))return plat_ps4_known[i].name;
 for(unsigned i=0;i<PF_COUNT;i++)if(!plat_streq(plat_ps4_need[i].nid,nid))return plat_ps4_need[i].name;
 return 0;}
static void *plat_ps4_sym(PlatTable *t,const char *nid,const char *lib){
 int h=plat_module(t,lib);const char *name=plat_ps4_name(nid);void *fn=0;
 if(h&&name)((int(*)(int,const char*,void**))t->dlsym)(h,name,&fn);
 if(!fn&&plat_ps4.missing<8){int k=0;for(;k<11&&nid[k];k++)plat_ps4.miss[plat_ps4.missing][k]=nid[k];plat_ps4.miss[plat_ps4.missing][k]=0;}
 return fn;}
static void plat_ps4_path(char *out,const char *name){const char *p=PLAT_DATA_DIR;int i=0;while(*p)out[i++]=*p++;if(name){out[i++]='/';while(*name)out[i++]=*name++;}out[i]=0;}
static int plat_init(uintptr_t base){
 u3_fix_apply();
 PlatTable *t=(PlatTable*)(base+PLAT_TABLE_VA);
 if(t->magic[0]!='U'||t->magic[1]!='3'||t->magic[2]!='P'||!t->dlsym)return PLAT_E_NOSYS;
 plat_ps4.base=base;plat_ps4.missing=0;plat_ps4.rows=(int)t->count;
 for(uint64_t i=0;i<t->count;i++){PlatRow *r=&t->row[i];void *fn=plat_ps4_sym(t,r->nid,r->lib);if(fn)*(void**)(base+r->slot)=fn;else plat_ps4.missing++;}
 for(int i=0;i<PF_COUNT;i++){void *fn=plat_ps4_sym(t,plat_ps4_need[i].nid,plat_ps4_need[i].lib);plat_ps4.fn[i]=(uintptr_t)fn;if(!fn)plat_ps4.missing++;}
 if(plat_ps4.fn[PF_MKDIR]){char p[64];plat_ps4_path(p,0);plat_ps4.mkdir_r=((int(*)(const char*,int))plat_ps4.fn[PF_MKDIR])(p,0777);}   /* /download0/u3mp; EEXIST is fine */
 plat_ps4.ready=!plat_ps4.missing;return plat_ps4.missing?PLAT_E_NOSYS:0;}
static void plat_net_pick(void){if(X.room[0]||!X.net[0])return;X.room[0]=1;}   /* native sceNet: nothing to probe */
static int plat_sock_ready(void){return X.room[0]==1;}
static int plat_sys_bind(void){return plat_ps4.fn[PF_PTHREAD_CREATE]&&plat_ps4.fn[PF_MAP_FLEX]&&plat_ps4.fn[PF_RESOLVER_CREATE]&&plat_ps4.fn[PF_USLEEP];}
static int plat_thread_ready(void){return plat_ps4.fn[PF_PTHREAD_CREATE]!=0;}

/* ---- raw sockets on the X.net[NETF_*] slots (sceNet: int descriptors, negative SCE errors) ---- */
typedef struct { uint8_t len,family; uint16_t port; uint32_t addr; uint8_t zero[8]; } PlatSaIn;
static int plat_ps4_fail(int r){if(r<0&&plat_ps4.fn[PF_ERRNO])plat_ps4.err=*((int*(*)(void))plat_ps4.fn[PF_ERRNO])();return r;}
static plat_sock plat_sock_open(int family,int type,int proto){int r=((int32_t(*)(const char*,int,int,int))X.net[NETF_SOCKET])("u3mp",family,type,proto);return plat_ps4_fail(r)<0?PLAT_BADSOCK:(plat_sock)r;}
static int plat_sock_close(plat_sock f){return plat_ps4_fail(((int32_t(*)(int))X.net[NETF_CLOSE])((int)f));}
static int plat_sock_connect(plat_sock f,uint32_t addr,uint16_t port_be){PlatSaIn a;clear(&a,sizeof(a));a.len=16;a.family=2;a.port=port_be;a.addr=addr;return plat_ps4_fail(((int32_t(*)(int,const void*,uint32_t))X.net[NETF_CONNECT])((int)f,&a,16));}
static int plat_sock_send(plat_sock f,const void *b,int n,int flags){return plat_ps4_fail(((int32_t(*)(int,const void*,uint32_t,int))X.net[NETF_SEND])((int)f,b,(uint32_t)n,flags));}
static int plat_sock_recv(plat_sock f,void *b,int n,int flags){return plat_ps4_fail(((int32_t(*)(int,void*,uint32_t,int))X.net[NETF_RECV])((int)f,b,(uint32_t)n,flags));}
static int plat_sock_sendto(plat_sock f,const void *b,int n,uint32_t addr,uint16_t port_be){PlatSaIn a;clear(&a,sizeof(a));a.len=16;a.family=2;a.port=port_be;a.addr=addr;return plat_ps4_fail(((int32_t(*)(int,const void*,uint32_t,int,const void*,uint32_t))X.net[NETF_SENDTO])((int)f,b,(uint32_t)n,0,&a,16));}
static int plat_sock_recvfrom(plat_sock f,void *b,int n){return plat_ps4_fail(((int32_t(*)(int,void*,uint32_t,int,void*,uint32_t*))X.net[NETF_RECVFROM])((int)f,b,(uint32_t)n,0,0,0));}
static int plat_sock_timeout(plat_sock f,int opt,int32_t ms){int32_t us=ms*1000;return plat_ps4_fail(((int32_t(*)(int,int,int,const void*,uint32_t))X.net[NETF_SETSOCKOPT])((int)f,0xffff,opt,&us,4));}   /* SO_*TIMEO in microseconds */
static int plat_sock_nonblock(plat_sock f,uint32_t on){int32_t v=(int32_t)on;return plat_ps4_fail(((int32_t(*)(int,int,int,const void*,uint32_t))X.net[NETF_SETSOCKOPT])((int)f,0xffff,0x1200,&v,4));}   /* SCE_NET_SO_NBIO */
static int plat_sock_err(void){return plat_ps4.err;}
#define PLAT_EWOULDBLOCK 35   /* SCE_NET_EWOULDBLOCK / EAGAIN */
#define PLAT_ETIMEDOUT 60     /* SCE_NET_ETIMEDOUT */
static int plat_resolve(const char *host,uint32_t *addr){   /* sceNetResolverCreate / StartNtoa / Destroy */
 if(!plat_ps4.fn[PF_RESOLVER_CREATE])return -1;
 int rid=((int(*)(const char*,int,int))plat_ps4.fn[PF_RESOLVER_CREATE])("u3mp",0,0);if(rid<0){plat_ps4_fail(rid);return -1;}
 uint32_t a=0;int r=((int(*)(int,const char*,uint32_t*,int,int,int))plat_ps4.fn[PF_RESOLVER_NTOA])(rid,host,&a,0,0,0);
 ((int(*)(int))plat_ps4.fn[PF_RESOLVER_DESTROY])(rid);
 if(r<0||!a){plat_ps4_fail(r);return -1;}*addr=a;return 0;}
static int plat_random(void *b,uint32_t n){if(!plat_ps4.fn[PF_NET_RANDOM])return -1;return ((int(*)(void*,uint64_t))plat_ps4.fn[PF_NET_RANDOM])(b,n)<0?-1:0;}

/* ---- memory, threads, time ---- */
static void *plat_alloc(uint64_t bytes){void *p=0;if(!plat_ps4.fn[PF_MAP_FLEX])return 0;int r=((int(*)(void**,uint64_t,int,int))plat_ps4.fn[PF_MAP_FLEX])(&p,(bytes+0x3fffu)&~(uint64_t)0x3fffu,3,0);return r<0?0:p;}   /* flexible memory is zeroed, as VirtualAlloc */
static int plat_thread(plat_thread_fn fn,void *arg,uint64_t stack){
 if(!plat_ps4.fn[PF_PTHREAD_CREATE])return -1;
 uintptr_t th=0,attr=0;const void *ap=0;int have_attr=0;
 if(plat_ps4.fn[PF_PTHREAD_ATTR_INIT]&&((int(*)(uintptr_t*))plat_ps4.fn[PF_PTHREAD_ATTR_INIT])(&attr)==0){
  have_attr=1;ap=&attr;
  if(plat_ps4.fn[PF_PTHREAD_ATTR_STACK])((int(*)(uintptr_t*,uint64_t))plat_ps4.fn[PF_PTHREAD_ATTR_STACK])(&attr,stack<0x10000?0x10000:stack);
  if(plat_ps4.fn[PF_PTHREAD_ATTR_DETACH])((int(*)(uintptr_t*,int))plat_ps4.fn[PF_PTHREAD_ATTR_DETACH])(&attr,1);}   /* SCE_PTHREAD_CREATE_DETACHED: the helper never joins */
 int r=((int(*)(uintptr_t*,const void*,void*(*)(void*),void*,const char*))plat_ps4.fn[PF_PTHREAD_CREATE])(&th,ap,fn,arg,"u3mp");
 if(have_attr&&plat_ps4.fn[PF_PTHREAD_ATTR_DESTROY])((int(*)(uintptr_t*))plat_ps4.fn[PF_PTHREAD_ATTR_DESTROY])(&attr);
 return r==0?0:-1;}
static void plat_sleep_ms(uint32_t ms){if(plat_ps4.fn[PF_USLEEP])((int(*)(uint32_t))plat_ps4.fn[PF_USLEEP])(ms*1000u);}
static void plat_civil(int64_t sec,uint16_t out[8]){   /* days since 1970 -> y/m/d (H. Hinnant's civil_from_days), weekday 0 = Sunday */
 int64_t days=sec/86400;int64_t rem=sec-days*86400;if(rem<0){rem+=86400;days--;}
 int64_t z=days+719468;int64_t era=(z>=0?z:z-146096)/146097;uint64_t doe=(uint64_t)(z-era*146097);
 uint64_t yoe=(doe-doe/1460+doe/36524-doe/146096)/365;int64_t y=(int64_t)yoe+era*400;uint64_t doy=doe-(365*yoe+yoe/4-yoe/100);
 uint64_t mp=(5*doy+2)/153;uint64_t d=doy-(153*mp+2)/5+1;uint64_t m=mp<10?mp+3:mp-9;if(m<=2)y++;
 out[0]=(uint16_t)y;out[1]=(uint16_t)m;out[2]=(uint16_t)(((days%7)+11)%7);out[3]=(uint16_t)d;out[4]=(uint16_t)(rem/3600);out[5]=(uint16_t)(rem/60%60);out[6]=(uint16_t)(rem%60);}
static int plat_utc(uint16_t out[8]){struct{int64_t sec;int64_t usec;}tv;if(!plat_ps4.fn[PF_GETTIMEOFDAY])return -1;int r=((int(*)(void*))plat_ps4.fn[PF_GETTIMEOFDAY])(&tv);if(r<0)return r;plat_civil(tv.sec,out);out[7]=(uint16_t)(tv.usec/1000);return 0;}
static int plat_textbox_open(const char *title,const char *label,const char *initial,int password,char *out,uint32_t cap,volatile uint8_t *res,volatile int32_t *busy){
 (void)title;(void)label;(void)initial;(void)password;(void)out;(void)cap;(void)res;(void)busy;return 0;}   /* no native box on a console: the game's own IME page is the input */
static int plat_handoff(char *out,uint32_t cap){if(out&&cap)out[0]=0;return 0;}
static int plat_relay(char *out,uint32_t cap){if(out&&cap)out[0]=0;return 0;}
static int plat_diag_append(const char *path,const char *text,uint32_t n){(void)path;(void)text;(void)n;return 0;}   /* v7.90 rf24 */   /* v7.89 rf23: no launcher on a console */   /* v7.59 lh01: no launcher environment on a console */
#include "plat_tls.inc"
#pragma clang section text=".text.ext" rodata=".rodata.ext"
#endif
