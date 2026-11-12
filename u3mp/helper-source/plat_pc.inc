/* plat_pc.inc - PC backend of plat.h (BilalReloaded's Windows runtime). Included from u3_lan_profiles.c
 * at the "LAN x001" spot (inside the .text.ext section pragma) when U3_PLAT_PS4 is NOT defined.
 * Every body is v7.53's code moved here with its name changed (part A of PKG Converter/eboot/PLAN.md):
 *
 *   plat_init             nothing: the builder's IMPORTS/NET_IMPORTS/KIMPORTS are relocated by the runtime
 *   plat_net_pick         x001 probe: when sceNetSocket answers 0x80410133 (ENETUNREACH), X.net[] is
 *                         pointed at the Winsock shims below (ws_find: PEB -> ws2_32.dll export table, v7.58 ws02;
 *                         Uncharted.exe's import table only as a fallback)
 *   plat_sock_*           the ws2_32 function pointers the shims use (raw handle, WSAGetLastError codes)
 *   plat_resolve          gethostbyname          plat_tls_*     BearSSL, shared with PS4 (plat_tls.inc, part C)
 *   plat_random           advapi32 SystemFunction036 (RtlGenRandom)
 *   plat_alloc            VirtualAlloc           plat_thread    CreateThread
 *   plat_sleep_ms         kernel32 Sleep         plat_utc       kernel32 GetSystemTime
 *   plat_textbox_open     user32/gdi32 box on its own thread (x035)
 *   plat_handoff          kernelbase GetEnvironmentVariableA / SetEnvironmentVariableA, ucrtbase _putenv_s (v7.59 lh01)
 *
 * All the Windows-only machinery (the PEB + PE export walk, the ms_abi thunks, ws_find) lives here, so a
 * PS4 build cannot reference it by accident. v7.55: the SChannel TLS path is gone (plat_tls.inc). Diagnostics:
 *   X.room[0] 1 native / 2 Winsock / 3 not available, [1] probe result, [2] WSAStartup result,
 *   [3] local IPv4, [4] sends, [5] receives, [6] last Winsock error, [7] last failing call;
 *   X.room[14] TLS: 0 ok, 0x1000|BearSSL error, or the socket error. */
#ifndef U3_PLAT_PC_INC
#define U3_PLAT_PC_INC
#define WSF __attribute__((ms_abi))
#define WSA __attribute__((force_align_arg_pointer))
typedef struct { uint16_t family,port; uint32_t addr; uint8_t zero[8]; } WinSa;
static struct {
 int (WSF *startup)(uint16_t,void*); int (WSF *lasterr)(void); uint64_t (WSF *socket_)(int,int,int); int (WSF *bind_)(uint64_t,const void*,int);
 int (WSF *close_)(uint64_t); int (WSF *connect_)(uint64_t,const void*,int); uint64_t (WSF *accept_)(uint64_t,void*,int*); int (WSF *listen_)(uint64_t,int);
 int (WSF *send_)(uint64_t,const void*,int,int); int (WSF *recv_)(uint64_t,void*,int,int); int (WSF *sendto_)(uint64_t,const void*,int,int,const void*,int);
 int (WSF *recvfrom_)(uint64_t,void*,int,int,void*,int*); int (WSF *setopt_)(uint64_t,int,int,const void*,int); int (WSF *ioctl_)(uint64_t,int32_t,uint32_t*);
 int (WSF *getname_)(uint64_t,void*,int*);
} ws EXT_BSS;
static uintptr_t ws_ctl_native EXT_BSS;
static struct { int32_t fd; int32_t nbio; } ws_nb[64] EXT_BSS;
static int32_t ws_err(uint32_t where){int e=ws.lasterr();X.room[6]=(uint32_t)e;X.room[7]=where;
 int b=e>=10000?e-10000:e;return (int32_t)(0x80410100u|(uint32_t)(b&0xff));}
static void ws_nbset(int32_t fd,int32_t v){int fr=-1;for(int i=0;i<64;i++){if(ws_nb[i].fd==fd){ws_nb[i].nbio=v;return;}if(!ws_nb[i].fd&&fr<0)fr=i;}if(fr>=0){ws_nb[fr].fd=fd;ws_nb[fr].nbio=v;}}
static int32_t ws_nbget(int32_t fd){for(int i=0;i<64;i++)if(ws_nb[i].fd==fd)return ws_nb[i].nbio;return 0;}
static void ws_nbdrop(int32_t fd){for(int i=0;i<64;i++)if(ws_nb[i].fd==fd){ws_nb[i].fd=0;ws_nb[i].nbio=0;}}
static void sa_in(WinSa *w,const void *a){const SceSockaddrIn *s=(const SceSockaddrIn*)a;clear(w,sizeof(*w));w->family=2;w->port=s->port;w->addr=s->addr;}
static void sa_out(void *a,uint32_t *al,const WinSa *w){if(!a)return;SceSockaddrIn *s=(SceSockaddrIn*)a;clear(s,sizeof(*s));s->len=16;s->family=2;s->port=w->port;s->addr=w->addr;if(al)*al=16;}
/* x003 test builds U3MP_LAN_LOOP=1 / 2 (eb[2] bits 30 / 31): two copies on one PC, on 127.0.0.2 and 127.0.0.3 (127.0.0.1 is treated as "self" by the LAN code). */
static uint32_t ws_loop_self(void){return X.eb[2]&0x40000000u?0x0200007fu:X.eb[2]&0x80000000u?0x0300007fu:0;}
static uint32_t ws_loop_peer(void){return X.eb[2]&0x40000000u?0x0300007fu:0x0200007fu;}
static void ws_loop_dest(WinSa *w){if(!ws_loop_self())return;if(w->addr==0xffffffffu)w->addr=ws_loop_peer();else if(w->addr==0x0100007fu)w->addr=ws_loop_self();}   /* localhost = this copy: its room server listens on its own address */
WSA static int32_t ws_socket(const char *n,int fam,int type,int proto){(void)n;uint64_t f=ws.socket_(fam,type,proto);if(f==~(uint64_t)0)return ws_err(1);return (int32_t)f;}
WSA static int32_t ws_close(int fd){ws_nbdrop(fd);return ws.close_((uint64_t)(int64_t)fd)<0?ws_err(2):0;}
WSA static int32_t ws_bind(int fd,const void *a,uint32_t l){(void)l;WinSa w;sa_in(&w,a);if(ws_loop_self())w.addr=ws_loop_self();return ws.bind_((uint64_t)(int64_t)fd,&w,16)<0?ws_err(3):0;}
WSA static int32_t ws_connect(int fd,const void *a,uint32_t l){(void)l;WinSa w;sa_in(&w,a);ws_loop_dest(&w);if(ws_loop_self()){WinSa me;clear(&me,sizeof(me));me.family=2;me.addr=ws_loop_self();ws.bind_((uint64_t)(int64_t)fd,&me,16);}   /* x012: an unbound loopback client appears as 127.0.0.1, so the room told the host the wrong guest address; a repeat bind just fails */
 if(ws.connect_((uint64_t)(int64_t)fd,&w,16)>=0)return 0;
 int32_t r=ws_err(4);return ((uint32_t)r&0xff)==35?(int32_t)0x80410124u:r;}                /* WSAEWOULDBLOCK on connect = EINPROGRESS */
WSA static int32_t ws_listen(int fd,int n){return ws.listen_((uint64_t)(int64_t)fd,n)<0?ws_err(5):0;}
WSA static int32_t ws_accept(int fd,void *a,uint32_t *al){WinSa w;int l=16;clear(&w,sizeof(w));uint64_t f=ws.accept_((uint64_t)(int64_t)fd,&w,&l);if(f==~(uint64_t)0)return ws_err(6);sa_out(a,al,&w);return (int32_t)f;}
WSA static int32_t ws_send(int fd,const void *b,uint32_t n,int fl){int dw=(fl&0x80)&&!ws_nbget(fd);uint32_t v=1;if(dw)ws.ioctl_((uint64_t)(int64_t)fd,(int32_t)0x8004667eu,&v);
 int r=ws.send_((uint64_t)(int64_t)fd,b,(int)n,fl&3);int32_t o=r<0?ws_err(7):r;if(dw){v=0;ws.ioctl_((uint64_t)(int64_t)fd,(int32_t)0x8004667eu,&v);}if(r>0)X.room[4]++;return o;}
WSA static int32_t ws_recv(int fd,void *b,uint32_t n,int fl){int dw=(fl&0x80)&&!ws_nbget(fd);uint32_t v=1;if(dw)ws.ioctl_((uint64_t)(int64_t)fd,(int32_t)0x8004667eu,&v);
 int r=ws.recv_((uint64_t)(int64_t)fd,b,(int)n,fl&3);int32_t o=r<0?ws_err(8):r;if(dw){v=0;ws.ioctl_((uint64_t)(int64_t)fd,(int32_t)0x8004667eu,&v);}if(r>0)X.room[5]++;return o;}
WSA static int32_t ws_sendto(int fd,const void *b,uint32_t n,int fl,const void *a,uint32_t l){(void)l;(void)fl;WinSa w;sa_in(&w,a);ws_loop_dest(&w);int r=ws.sendto_((uint64_t)(int64_t)fd,b,(int)n,0,&w,16);if(r<0)return ws_err(9);X.room[4]++;return r;}
WSA static int32_t ws_recvfrom(int fd,void *b,uint32_t n,int fl,void *a,uint32_t *al){(void)fl;WinSa w;int l=16;clear(&w,sizeof(w));int r=ws.recvfrom_((uint64_t)(int64_t)fd,b,(int)n,0,&w,&l);
 if(r<0){int32_t e=ws_err(10);return ((uint32_t)e&0xff)==54?(int32_t)0x80410123u:e;}       /* WSAECONNRESET after an ICMP unreachable: nothing to read */
 sa_out(a,al,&w);X.room[5]++;return r;}
WSA static int32_t ws_setopt(int fd,int lv,int opt,const void *v,uint32_t l){
 if(lv==0xffff&&opt==0x1200){uint32_t on=*(const int32_t*)v?1:0;ws_nbset(fd,(int32_t)on);return ws.ioctl_((uint64_t)(int64_t)fd,(int32_t)0x8004667eu,&on)<0?ws_err(11):0;}
 if(lv==0xffff&&(opt==0x1005||opt==0x1006)){int32_t ms=*(const int32_t*)v/1000;return ws.setopt_((uint64_t)(int64_t)fd,lv,opt,&ms,4)<0?ws_err(12):0;}   /* us -> ms */
 return ws.setopt_((uint64_t)(int64_t)fd,lv,opt,v,(int)l)<0?ws_err(12):0;}
static const char ws_dig[]="0123456789";
WSA static int32_t ws_ctl_info(int code,void *out){
 if(code!=14)return ws_ctl_native?((int32_t(*)(int,void*))ws_ctl_native)(code,out):-1;
 uint64_t f=ws_loop_self()?0:ws.socket_(2,2,0);if(f==~(uint64_t)0)return ws_err(13);
 WinSa w;clear(&w,sizeof(w));w.family=2;w.port=be16(9);w.addr=0x010a0a0au;                  /* 10.10.10.1: no packet is sent, this only picks the route */
 int l=16,ok=1;if(ws_loop_self())w.addr=ws_loop_self();else{ok=ws.connect_(f,&w,16)>=0&&ws.getname_(f,&w,&l)>=0;ws.close_(f);}if(!ok||!w.addr)return ws_err(13);
 X.room[3]=w.addr;char *o=(char*)out;for(int i=0;i<4;i++){uint32_t v=(w.addr>>(i*8))&0xff;if(v>=100)*o++=ws_dig[v/100];if(v>=10)*o++=ws_dig[v/10%10];*o++=ws_dig[v%10];if(i<3)*o++='.';}*o=0;return 0;}
static int ws_eq(const char *a,const char *b){for(;;a++,b++){char c=*a,d=*b;if(c>='A'&&c<='Z')c+=32;if(d>='A'&&d<='Z')d+=32;if(c!=d)return 0;if(!c)return 1;}}
static uint64_t *ws_slot(int k){return (uint64_t*)&ws+k;}
/* ws02 begin (v7.58): the ws2_32 functions from ws2_32.dll's own export table, so no runtime import is needed.
 * ws2_32.dll: the PEB loader list (pc_mod); not loaded -> LoadLibraryA from kernel32's export table (kernelbase's as a second
 * source). Forwarded exports ("module.function") are followed (an api-ms-/ext-ms- set goes to kernelbase). Uncharted.exe's
 * import table (the v7.57 walk) only fills a name the export walk did not give. ws02_src: 1 exports, 2 exports + imports,
 * 3 imports only (diagnostic). */
static void *pc_mod(const char *name);
static uint8_t ws02_src EXT_BSS;
static void *ws_exp(uint8_t *b,const char *name,int depth){
 if(!b||depth>4||b[0]!='M'||b[1]!='Z')return 0;
 uint8_t *nt=b+*(uint32_t*)(b+0x3c);if(*(uint32_t*)nt!=0x4550)return 0;
 uint32_t rva=*(uint32_t*)(nt+24+112),sz=*(uint32_t*)(nt+24+116);if(!rva||!sz)return 0;
 uint8_t *d=b+rva;uint32_t nf=*(uint32_t*)(d+20),n=*(uint32_t*)(d+24);
 const uint32_t *fn=(const uint32_t*)(b+*(uint32_t*)(d+28)),*nm=(const uint32_t*)(b+*(uint32_t*)(d+32));const uint16_t *ord=(const uint16_t*)(b+*(uint32_t*)(d+36));
 for(uint32_t i=0;i<n;i++){
  const char *a=(const char*)b+nm[i],*c=name;while(*c&&*a==*c){a++;c++;}if(*c||*a)continue;
  uint32_t o=ord[i];if(o>=nf)return 0;uint32_t f=fn[o];if(!f)return 0;
  if(f<rva||f>=rva+sz)return b+f;
  const char *s=(const char*)b+f;char mod[72];int k=0;                       /* forwarder "module.function" */
  while(s[k]&&s[k]!='.'&&k<60){char ch=s[k];mod[k]=(ch>='A'&&ch<='Z')?(char)(ch+32):ch;k++;}
  if(s[k]!='.'||s[k+1]=='#'||!s[k+1])return 0;
  const char *fname=s+k+1;mod[k]=0;uint8_t *m;
  if((mod[0]=='a'&&mod[1]=='p'&&mod[2]=='i'&&mod[3]=='-')||(mod[0]=='e'&&mod[1]=='x'&&mod[2]=='t'&&mod[3]=='-'))m=(uint8_t*)pc_mod("kernelbase.dll");
  else{const char *x=".dll";for(int j=0;x[j];j++)mod[k++]=x[j];mod[k]=0;m=(uint8_t*)pc_mod(mod);}
  return m?ws_exp(m,fname,depth+1):0;}
 return 0;}
static void ws_find_imports(const char *const *names){   /* v7.57's walk of Uncharted.exe's imports: fills only empty slots */
 uint8_t *peb;__asm__ volatile("mov %%gs:0x60,%0":"=r"(peb));if(!peb)return;
 uint8_t *b=*(uint8_t**)(peb+0x10);if(!b||b[0]!='M'||b[1]!='Z')return;
 uint8_t *nt=b+*(uint32_t*)(b+0x3c);if(*(uint32_t*)nt!=0x4550)return;
 uint32_t rva=*(uint32_t*)(nt+24+120);if(!rva)return;
 for(uint8_t *d=b+rva;*(uint32_t*)(d+12);d+=20){
  if(!ws_eq((char*)b+*(uint32_t*)(d+12),"ws2_32.dll"))continue;
  uint32_t ot=*(uint32_t*)d,ft=*(uint32_t*)(d+16);if(!ot||!ft)return;
  uint64_t *on=(uint64_t*)(b+ot),*fn=(uint64_t*)(b+ft);
  for(;*on;on++,fn++){if(*on>>63)continue;const char *nm=(char*)b+(uint32_t)*on+2;
   for(int k=0;k<15;k++)if(!*ws_slot(k)&&ws_eq(nm,names[k]))*ws_slot(k)=*fn;}
  return;}}
static int ws_find(void){
 const char *names[15];
 names[0]="WSAStartup";names[1]="WSAGetLastError";names[2]="socket";names[3]="bind";names[4]="closesocket";names[5]="connect";names[6]="accept";names[7]="listen";
 names[8]="send";names[9]="recv";names[10]="sendto";names[11]="recvfrom";names[12]="setsockopt";names[13]="ioctlsocket";names[14]="getsockname";
 for(int k=0;k<15;k++)*ws_slot(k)=0;
 uint8_t *w=(uint8_t*)pc_mod("ws2_32.dll");
 if(!w){void *ll=ws_exp((uint8_t*)pc_mod("kernel32.dll"),"LoadLibraryA",0);if(!ll)ll=ws_exp((uint8_t*)pc_mod("kernelbase.dll"),"LoadLibraryA",0);
  if(ll)w=(uint8_t*)((void*(WSF*)(const char*))ll)("ws2_32.dll");}
 int got=0;
 if(w)for(int k=0;k<15;k++){void *p=ws_exp(w,names[k],0);if(p){*ws_slot(k)=(uint64_t)(uintptr_t)p;got++;}}
 if(got<15)ws_find_imports(names);
 for(int k=0;k<15;k++)if(!*ws_slot(k))return 0;
 ws02_src=got==15?1:got?2:3;return 1;}
/* ws02 end */
static int plat_init(uintptr_t base){(void)base;return 0;}
/* Run before the first socket of each module; decides once. */
EXT static void plat_net_pick(void){
 if(X.room[0]||!X.net[0])return;
 if(X.eb[2]&0x20000000u){X.room[0]=1;return;}                                             /* U3MP_LAN_WINSOCK=0 */
 int32_t fd=((int32_t(*)(const char*,int,int,int))X.net[0])("u3mp-probe",2,2,0);X.room[1]=(uint32_t)fd;
 if(fd>=0){((int32_t(*)(int))X.net[1])(fd);X.room[0]=1;return;}
 if((uint32_t)fd!=0x80410133u||!ws_find()){X.room[0]=3;return;}
 uint8_t wd[0x200];clear(wd,sizeof(wd));int s=ws.startup(0x0202,wd);X.room[2]=(uint32_t)s;if(s){X.room[0]=3;return;}
 ws_ctl_native=X.net[6];
 X.net[0]=(uintptr_t)&ws_socket;X.net[1]=(uintptr_t)&ws_close;X.net[2]=(uintptr_t)&ws_bind;X.net[3]=(uintptr_t)&ws_sendto;X.net[4]=(uintptr_t)&ws_recvfrom;
 X.net[5]=(uintptr_t)&ws_setopt;X.net[6]=(uintptr_t)&ws_ctl_info;X.net[7]=(uintptr_t)&ws_connect;X.net[8]=(uintptr_t)&ws_listen;X.net[9]=(uintptr_t)&ws_accept;
 X.net[10]=(uintptr_t)&ws_send;X.net[11]=(uintptr_t)&ws_recv;X.room[0]=2;}
static int plat_sock_ready(void){return X.room[0]==2;}   /* the thread code below runs on the raw ws2_32 functions */

/* ---- raw sockets: the ws2_32 pointers ws_find filled, exactly as the thread code called them ---- */
static plat_sock plat_sock_open(int family,int type,int proto){return ws.socket_(family,type,proto);}
static int plat_sock_close(plat_sock f){return ws.close_(f);}
static int plat_sock_connect(plat_sock f,uint32_t addr,uint16_t port_be){WinSa a;clear(&a,sizeof(a));a.family=2;a.port=port_be;a.addr=addr;return ws.connect_(f,&a,16);}
static int plat_sock_send(plat_sock f,const void *b,int n,int flags){return ws.send_(f,b,n,flags);}
static int plat_sock_recv(plat_sock f,void *b,int n,int flags){return ws.recv_(f,b,n,flags);}
static int plat_sock_sendto(plat_sock f,const void *b,int n,uint32_t addr,uint16_t port_be){WinSa a;clear(&a,sizeof(a));a.family=2;a.port=port_be;a.addr=addr;return ws.sendto_(f,b,n,0,&a,16);}
static int plat_sock_recvfrom(plat_sock f,void *b,int n){return ws.recvfrom_(f,b,n,0,0,0);}
static int plat_sock_timeout(plat_sock f,int opt,int32_t ms){return ws.setopt_(f,0xffff,opt,&ms,4);}
static int plat_sock_nonblock(plat_sock f,uint32_t on){return ws.ioctl_(f,(int32_t)0x8004667eu,&on);}   /* FIONBIO */
static int plat_sock_err(void){return ws.lasterr();}
#define PLAT_EWOULDBLOCK 10035   /* WSAEWOULDBLOCK */
#define PLAT_ETIMEDOUT 10060     /* WSAETIMEDOUT */

/* ---- kernelbase/kernel32/ws2_32/advapi32 through the PEB module list + PE export table (x033) ---- */
static struct {
 void *(WSF *loadlib)(const char*);void *(WSF *getproc)(void*,const char*);
 void *(WSF *valloc)(void*,uint64_t,uint32_t,uint32_t);void *(WSF *cthread)(void*,uint64_t,void*,void*,uint32_t,uint32_t*);
 void *(WSF *ghbn)(const char*);
 void (WSF *sleep_)(uint32_t);void (WSF *gettime)(void*);
 uint8_t (WSF *rand_)(void*,uint32_t);
} pc_f EXT_BSS;
static int pc_weq(const uint16_t *w,uint32_t bytes,const char *n){uint32_t c=bytes/2,i=0;for(;i<c&&n[i];i++){uint16_t x=w[i];if(x>='A'&&x<='Z')x+=32;if(x!=(uint8_t)n[i])return 0;}return i==c&&!n[i];}
static void *pc_mod(const char *name){
 uint8_t *peb;__asm__ volatile("mov %%gs:0x60,%0":"=r"(peb));if(!peb)return 0;
 uint8_t *ldr=*(uint8_t**)(peb+0x18);if(!ldr)return 0;uint8_t *head=ldr+0x20,*l=*(uint8_t**)head;
 for(int g=0;l&&l!=head&&g<512;g++,l=*(uint8_t**)l){uint16_t bytes=*(uint16_t*)(l+0x48);const uint16_t *w=*(const uint16_t**)(l+0x50);if(w&&pc_weq(w,bytes,name))return *(void**)(l+0x20);}
 return 0;}
static void *pc_exp(uint8_t *b,const char *name){
 if(!b)return 0;uint8_t *nt=b+*(uint32_t*)(b+0x3c);uint32_t rva=*(uint32_t*)(nt+24+112);if(!rva)return 0;uint8_t *d=b+rva;
 uint32_t n=*(uint32_t*)(d+24);uint32_t *names=(uint32_t*)(b+*(uint32_t*)(d+32)),*fn=(uint32_t*)(b+*(uint32_t*)(d+28));uint16_t *ord=(uint16_t*)(b+*(uint32_t*)(d+36));
 for(uint32_t i=0;i<n;i++){const char *a=(const char*)b+names[i],*c=name;while(*c&&*a==*c){a++;c++;}if(!*c&&!*a)return b+fn[ord[i]];}
 return 0;}
static int plat_sys_bind(void){
 if(pc_f.loadlib)return 1;
 uint8_t *kb=(uint8_t*)pc_mod("kernelbase.dll");void *ll=pc_exp(kb,"LoadLibraryA"),*gp=pc_exp(kb,"GetProcAddress");if(!ll||!gp)return 0;
 void *(WSF *L)(const char*)=(void*(WSF*)(const char*))ll;void *(WSF *G)(void*,const char*)=(void*(WSF*)(void*,const char*))gp;
 void *k=L("kernel32.dll"),*w=L("ws2_32.dll"),*r=L("advapi32.dll");if(!k||!w)return 0;
 *(void**)&pc_f.valloc=G(k,"VirtualAlloc");*(void**)&pc_f.cthread=G(k,"CreateThread");*(void**)&pc_f.ghbn=G(w,"gethostbyname");
 if(!pc_f.valloc||!pc_f.cthread||!pc_f.ghbn)return 0;
 *(void**)&pc_f.sleep_=G(k,"Sleep");*(void**)&pc_f.gettime=G(k,"GetSystemTime");   /* v7.53 looked these up at each use; kernel32 always has them */
 if(r)*(void**)&pc_f.rand_=G(r,"SystemFunction036");                                /* RtlGenRandom; without it plat_tls falls back to rdtsc mixing */
 *(void**)&pc_f.getproc=gp;*(void**)&pc_f.loadlib=ll;return 1;}
static int plat_thread_ready(void){return pc_f.cthread!=0;}
static int plat_resolve(const char *host,uint32_t *addr){if(!plat_sys_bind())return -1;uint8_t *he=(uint8_t*)pc_f.ghbn(host);uint8_t **al=he?*(uint8_t***)(he+0x18):0;if(!al||!al[0])return -1;copy(addr,al[0],4);return 0;}
static void *plat_alloc(uint64_t bytes){if(!plat_sys_bind())return 0;return pc_f.valloc(0,bytes,0x3000,4);}   /* v7.102: bind on first use (LAN had none) */   /* MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE */
static int plat_thread(plat_thread_fn fn,void *arg,uint64_t stack){uint32_t tid;if(!plat_sys_bind())return -1;return pc_f.cthread(0,stack,(void*)fn,arg,0,&tid)?0:-1;}
static void plat_sleep_ms(uint32_t ms){if(pc_f.sleep_)pc_f.sleep_(ms);}
static int plat_utc(uint16_t st[8]){if(!pc_f.gettime)return -1;pc_f.gettime(st);return 0;}

static int plat_random(void *b,uint32_t n){if(!pc_f.rand_)return -1;return pc_f.rand_(b,n)?0:-1;}
#include "plat_tls.inc"
#pragma clang section text=".text.ext" rodata=".rodata.ext"

/* ---- v7.59 lh01: plat_handoff (plat.h). GetEnvironmentVariableA / SetEnvironmentVariableA from kernelbase's export table
 * (kernel32's as a second source; ws_exp follows forwarders), found as ws_find finds the ws2_32 functions; ucrtbase's _putenv_s
 * drops the C runtime's copy as well. Called once, on the game thread (rp_tick). Nothing here logs the value. ---- */
static int plat_env_take(const char *var,char *out,uint32_t cap);
/* v7.90 rf24: append n bytes to path (relative to the working folder) with kernel32 directly: usable from any thread, also while
 * the game thread is stuck. 0 when it could not be written. */
static int plat_diag_append(const char *path,const char *text,uint32_t n){
 uint8_t *kb=(uint8_t*)pc_mod("kernelbase.dll"),*k32=(uint8_t*)pc_mod("kernel32.dll");
 void *cf=ws_exp(kb,"CreateFileA",0),*wf=ws_exp(kb,"WriteFile",0),*ch=ws_exp(kb,"CloseHandle",0);
 if(!cf)cf=ws_exp(k32,"CreateFileA",0);if(!wf)wf=ws_exp(k32,"WriteFile",0);if(!ch)ch=ws_exp(k32,"CloseHandle",0);
 if(!cf||!wf||!ch)return 0;
 void *h=((void*(WSF*)(const char*,uint32_t,uint32_t,void*,uint32_t,uint32_t,void*))cf)(path,4u,3u,0,4u,0x80u,0);   /* FILE_APPEND_DATA, share r/w, OPEN_ALWAYS */
 if(!h||h==(void*)(intptr_t)-1)return 0;
 uint32_t done=0;int ok=((int(WSF*)(void*,const void*,uint32_t,uint32_t*,void*))wf)(h,text,n,&done,0);
 ((int(WSF*)(void*))ch)(h);return ok&&done==n;}
static int plat_handoff(char *out,uint32_t cap){static const char var[]="U3MP_HANDOFF";return plat_env_take(var,out,cap);}
static int plat_relay(char *out,uint32_t cap){static const char var[]="U3MP_RELAY";return plat_env_take(var,out,cap);}   /* v7.89 rf23 */
static int plat_env_take(const char *var,char *out,uint32_t cap){
 if(!out||cap<2)return 0;clear(out,cap);
 uint8_t *kb=(uint8_t*)pc_mod("kernelbase.dll"),*k32=(uint8_t*)pc_mod("kernel32.dll");
 void *get=ws_exp(kb,"GetEnvironmentVariableA",0),*set=ws_exp(kb,"SetEnvironmentVariableA",0);
 if(!get)get=ws_exp(k32,"GetEnvironmentVariableA",0);if(!set)set=ws_exp(k32,"SetEnvironmentVariableA",0);
 if(!get)return 0;
 uint32_t n=((uint32_t(WSF*)(const char*,char*,uint32_t))get)(var,out,cap);
 if(!n)return 0;                                                                /* not set: nothing to remove */
 void *put=ws_exp((uint8_t*)pc_mod("ucrtbase.dll"),"_putenv_s",0);
 if(put)((int(WSF*)(const char*,const char*))put)(var,"");                     /* the C runtime's copy ("" removes it) */
 if(set)((int(WSF*)(const char*,const char*))set)(var,0);                       /* the process environment block */
 if(n>=cap){clear(out,cap);return 0;}                                            /* too long: not ours */
 return (int)n;}

/* ---- x035: one Windows box with an edit field, so a value can be pasted (the game's text input has no clipboard) ---- */
static struct {
 void *(WSF *cwin)(uint32_t,const char*,const char*,uint32_t,int,int,int,int,void*,void*,void*,void*);
 uint16_t (WSF *reg)(const void*);int64_t (WSF *defp)(void*,uint32_t,uint64_t,int64_t);
 int (WSF *getm)(void*,void*,uint32_t,uint32_t);int (WSF *isdlg)(void*,void*);int (WSF *trans)(const void*);int64_t (WSF *disp)(const void*);
 int (WSF *gett)(void*,char*,int);int (WSF *dest)(void*);int (WSF *fg)(void*);int64_t (WSF *sendm)(void*,uint32_t,uint64_t,int64_t);void *(WSF *stock)(int);void *(WSF *modh)(const char*);void *(WSF *fcs)(void*);
} pc_u EXT_BSS;
static struct { const char *title,*label,*initial;int password;char *out;uint32_t cap;volatile uint8_t *res;volatile int32_t *busy; } pc_box EXT_BSS;
static volatile int32_t pc_ui_done EXT_BSS,pc_ui_ok EXT_BSS;
WSF __attribute__((force_align_arg_pointer)) static int64_t pc_wndproc(void *h,uint32_t m,uint64_t w,int64_t l){
 if(m==0x111&&((w&0xffff)==1||(w&0xffff)==2)){pc_ui_ok=(w&0xffff)==1;pc_ui_done=1;return 0;}
 if(m==0x10){pc_ui_ok=0;pc_ui_done=1;return 0;}
 return pc_u.defp(h,m,w,l);}
static int pc_ui_bind(void){
 if(pc_u.cwin)return 1;if(!plat_sys_bind())return 0;
 void *u=pc_f.loadlib("user32.dll"),*g=pc_f.loadlib("gdi32.dll"),*k=pc_f.loadlib("kernel32.dll");if(!u||!g||!k)return 0;
 *(void**)&pc_u.reg=pc_f.getproc(u,"RegisterClassA");*(void**)&pc_u.defp=pc_f.getproc(u,"DefWindowProcA");*(void**)&pc_u.getm=pc_f.getproc(u,"GetMessageA");*(void**)&pc_u.isdlg=pc_f.getproc(u,"IsDialogMessageA");
 *(void**)&pc_u.trans=pc_f.getproc(u,"TranslateMessage");*(void**)&pc_u.disp=pc_f.getproc(u,"DispatchMessageA");*(void**)&pc_u.gett=pc_f.getproc(u,"GetWindowTextA");*(void**)&pc_u.dest=pc_f.getproc(u,"DestroyWindow");
 *(void**)&pc_u.fg=pc_f.getproc(u,"SetForegroundWindow");*(void**)&pc_u.fcs=pc_f.getproc(u,"SetFocus");*(void**)&pc_u.sendm=pc_f.getproc(u,"SendMessageA");*(void**)&pc_u.stock=pc_f.getproc(g,"GetStockObject");*(void**)&pc_u.modh=pc_f.getproc(k,"GetModuleHandleA");
 void *cw=pc_f.getproc(u,"CreateWindowExA");
 if(!pc_u.reg||!pc_u.defp||!pc_u.getm||!pc_u.isdlg||!pc_u.trans||!pc_u.disp||!pc_u.gett||!pc_u.dest||!pc_u.fg||!pc_u.sendm||!pc_u.stock||!pc_u.modh||!pc_u.fcs||!cw)return 0;
 *(void**)&pc_u.cwin=cw;return 1;}
WSF __attribute__((force_align_arg_pointer)) static uint32_t pc_ui_thread(void *arg){(void)arg;
 static uint8_t reged EXT_BSS;void *inst=pc_u.modh(0);
 if(!reged){uint8_t wc[72];clear(wc,sizeof(wc));*(void**)(wc+8)=(void*)&pc_wndproc;*(void**)(wc+24)=inst;*(uint64_t*)(wc+48)=16;*(const char**)(wc+64)="U3TextBox";if(!pc_u.reg(wc)){*pc_box.busy=0;return 0;}reged=1;}
 pc_ui_done=0;pc_ui_ok=0;
 void *w=pc_u.cwin(8,"U3TextBox",pc_box.title,0x10C80000u,200,160,470,130,0,0,inst,0);   /* topmost, caption, sysmenu, visible */
 if(!w){*pc_box.busy=0;return 0;}
 void *font=pc_u.stock(17);void *ed=0;
 {void *st=pc_u.cwin(0,"STATIC",pc_box.label,0x50000000u,14,16,180,20,w,0,inst,0);if(st)pc_u.sendm(st,0x30,(uint64_t)font,0);
  ed=pc_u.cwin(0x200,"EDIT",pc_box.password?"":pc_box.initial,0x50010080u|(pc_box.password?0x20u:0u),200,12,240,26,w,0,inst,0);if(ed){pc_u.sendm(ed,0x30,(uint64_t)font,0);pc_u.sendm(ed,0xb1,0,-1);pc_u.fcs(ed);}}
 void *b1=pc_u.cwin(0,"BUTTON","OK",0x50010001u,250,50,90,30,w,(void*)1,inst,0),*b2=pc_u.cwin(0,"BUTTON","Cancel",0x50010000u,350,50,90,30,w,(void*)2,inst,0);
 if(b1)pc_u.sendm(b1,0x30,(uint64_t)font,0);if(b2)pc_u.sendm(b2,0x30,(uint64_t)font,0);
 pc_u.fg(w);
 uint8_t msg[48] __attribute__((aligned(8)));
 while(!pc_ui_done&&pc_u.getm(msg,0,0,0)>0){if(!pc_u.isdlg(w,msg)){pc_u.trans(msg);pc_u.disp(msg);}}
 if(pc_ui_ok){clear(pc_box.out,pc_box.cap);if(ed)pc_u.gett(ed,pc_box.out,(int)pc_box.cap-1);*pc_box.res=1;}
 pc_u.dest(w);*pc_box.busy=0;return 0;}
static int plat_textbox_open(const char *title,const char *label,const char *initial,int password,char *out,uint32_t cap,volatile uint8_t *res,volatile int32_t *busy){
 if(!pc_ui_bind())return 0;
 pc_box.title=title;pc_box.label=label;pc_box.initial=initial;pc_box.password=password;pc_box.out=out;pc_box.cap=cap;pc_box.res=res;pc_box.busy=busy;
 *busy=1;if(plat_thread(&pc_ui_thread,0,0x40000)<0){*busy=0;return 0;}return 1;}
#endif
