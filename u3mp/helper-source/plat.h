/* plat.h - the ONLY door between the U3 helper (u3_lan_profiles.c, agc_port.inc, kr_port.inc) and the
 * operating system. One source tree builds for both targets:
 *
 *     PC  (BilalReloaded runtime, Windows)   build_u3_pool_blob.py ... (default)          -> plat_pc.inc
 *     PS4 (fake-signed big3 SELF in a PKG)   build_u3_pool_blob_plat.py --cflags -DU3_PLAT_PS4 -> plat_ps4.inc
 *
 * RULE FOR ALL FUTURE HELPER CODE
 *   Game logic never calls the OS. No Winsock, no SChannel, no LoadLibrary/GetProcAddress, no PEB walks
 *   (gs:0x60), no VirtualAlloc/CreateThread/Sleep/GetSystemTime, no "/data/..." literals, no ms_abi.
 *   Everything goes through the plat_* functions below, and a plat_* function is added here (with both
 *   backends) before it is used. A build with U3_PLAT_PS4 defined does not include plat_pc.inc at all, so
 *   a Windows reference left in game logic fails to compile there.
 *
 * Two layers, both included from u3_lan_profiles.c:
 *   1. this header, right after the ProfileState typedef: macros, types, the file wrappers (shared bodies)
 *      and the prototypes (static: one translation unit);
 *   2. plat_pc.inc / plat_ps4.inc at the "LAN x001" spot, inside the .text.ext section pragma, once
 *      ExtState X, SceSockaddrIn, be16, clear, copy and the NETF_* slots exist.
 *
 * Conventions: the raw socket functions keep the shape the thread code used on PC (a 64-bit socket
 * handle, PLAT_BADSOCK on failure, plat_sock_err() for the last OS error compared against PLAT_E*), so
 * the PC backend is a plain rename of v7.53 and the diagnostics (X.ec[67], X.ec[95], X.room[14]) keep
 * their raw values. The X.net[NETF_*] slots (sceNet semantics) stay the game logic's own socket API for
 * the LAN / online code; plat_net_pick() decides once which backend fills them.
 */
#ifndef U3_PLAT_H
#define U3_PLAT_H

/* ---- files -------------------------------------------------------------------------------------
 * Every helper file lives under PLAT_DATA_DIR; paths are built as PLAT_DATA_DIR "/name" at compile time
 * or by the path builders, whose buffers are sized with PLAT_PATH(n) (n = the size that fits "/data").
 * The open/read/write/close/rename slots in ProfileState are the same sceKernel* functions on both targets
 * (the PC runtime relocates them, the PS4 loader / plat table fills them), so the wrappers are shared. */
#ifdef U3_PLAT_PS4
#define PLAT_DATA_DIR "/download0/u3mp"      /* the title's own writable area; /data needs the sandbox patch */
#else
#define PLAT_DATA_DIR "/data"                /* the runtime's --user dir, as before */
#endif
#define PLAT_PATH(n) ((n)-5+(int)sizeof(PLAT_DATA_DIR)-1)
static inline int64_t plat_open(ProfileState *s,const char *path,int flags,int mode){return ((int64_t(*)(const char*,int,int))s->open)(path,flags,mode);}
static inline int64_t plat_read(ProfileState *s,int fd,void *b,uint64_t n){return ((int64_t(*)(int,void*,uint64_t))s->read)(fd,b,n);}
static inline int64_t plat_write(ProfileState *s,int fd,const void *b,uint64_t n){return ((int64_t(*)(int,const void*,uint64_t))s->write)(fd,b,n);}
static inline int64_t plat_close(ProfileState *s,int fd){return ((int64_t(*)(int))s->close)(fd);}
static inline int64_t plat_rename(ProfileState *s,const char *from,const char *to){return ((int64_t(*)(const char*,const char*))s->rename)(from,to);}

/* ---- lifetime ----------------------------------------------------------------------------------
 * plat_init      kind 44 (natives_link, early engine init) with the game image base.
 *                PC: nothing (the builder's IMPORTS/NET_IMPORTS/KIMPORTS are relocated by the runtime).
 *                PS4: reads the U3PLAT1 table written by image2elf.py and fills the slots with sceKernelDlsym.
 * plat_net_pick  before the first socket of each module; decides once what fills X.net[] (X.room[0]).
 *                PC: the x001 probe, Winsock shims when sceNetSocket answers 0x80410133. PS4: native.
 * plat_sock_ready   1 when the raw socket functions below can be used from a helper thread.
 * plat_sys_bind     resolves alloc/thread/sleep/utc/resolve/tls once; 1 when all are available.
 * plat_thread_ready 1 once plat_sys_bind found the thread function (the UPnP thread checks it). */
static int plat_init(uintptr_t base);
static void plat_net_pick(void);
static int plat_sock_ready(void);
static int plat_sys_bind(void);
static int plat_thread_ready(void);

/* ---- raw sockets for the helper threads (blocking, IPv4) ---------------------------------------
 * addr is in network order (as SceSockaddrIn / sockaddr_in), port_be too (callers use be16()).
 * flags: 0 or 2 (MSG_PEEK, the same value on both targets). plat_sock_timeout: opt 0x1005 send /
 * 0x1006 receive, milliseconds. plat_sock_err: the raw OS error of the last failing call. */
typedef uint64_t plat_sock;
#define PLAT_BADSOCK (~(uint64_t)0)
#define PLAT_SO_SNDTIMEO 0x1005
#define PLAT_SO_RCVTIMEO 0x1006
static plat_sock plat_sock_open(int family,int type,int proto);
static int plat_sock_close(plat_sock f);
static int plat_sock_connect(plat_sock f,uint32_t addr,uint16_t port_be);
static int plat_sock_send(plat_sock f,const void *b,int n,int flags);
static int plat_sock_recv(plat_sock f,void *b,int n,int flags);
static int plat_sock_sendto(plat_sock f,const void *b,int n,uint32_t addr,uint16_t port_be);
static int plat_sock_recvfrom(plat_sock f,void *b,int n);
static int plat_sock_timeout(plat_sock f,int opt,int32_t ms);
static int plat_sock_nonblock(plat_sock f,uint32_t on);
static int plat_sock_err(void);
static int plat_resolve(const char *host,uint32_t *addr);       /* 0 ok (*addr set), -1 not found (*addr untouched) */

/* ---- absolute pointers in writable data (plat_tls.inc / u3_bearssl.inc vtables) --------------------
 * The split linker (build_u3_pool_blob.py) resolves PC-relative relocations only; a const struct holding
 * function pointers (BearSSL's vtables) needs absolute addresses. Such objects are placed in the writable
 * section .data.u3bssl (region C) by the relro pragma, the linker stores helper-relative values in them and
 * lists their offsets in u3_fix (section .data.u3fix: [0] count, then offsets from the start of region A),
 * and u3_fix_apply() adds the run-time address of region A once (region A starts with u3_profile_event). */
#define U3_FIX_MAX 191
__attribute__((visibility("hidden"))) int u3_profile_event(uintptr_t base,int32_t *event,int kind,ProfileState *s,uintptr_t a5);   /* hidden: its address is taken RIP-relative, not through a GOT */
static volatile uint64_t u3_fix[1+U3_FIX_MAX] __attribute__((section(".data.u3fix"),used,aligned(16)))={~(uint64_t)0};
EXT static void u3_fix_apply(void){
 static volatile uint8_t done __attribute__((section(".bss.u3ext")));
 if(done)return;done=1;uint64_t n=u3_fix[0];if(n==~(uint64_t)0||n>U3_FIX_MAX)return;
 uintptr_t a=(uintptr_t)&u3_profile_event;for(uint64_t i=0;i<n;i++)*(volatile uint64_t*)(a+u3_fix[1+i])+=a;}
static int plat_random(void *b,uint32_t n);                      /* OS entropy: 0 ok, -1 none (PC RtlGenRandom, PS4 sceNetGetRandom) */

/* ---- TLS client (raw TLS 1.2 on a connected plat_sock, self-signed server: no validation) ---------
 * One implementation for both targets: BearSSL in plat_tls.inc (part C). PlatTls is its state and must
 * live in the caller's memory block (the first member of RcMem). One connection at a time.
 *   handshake  1 ok / 0 failed            read   1 when n plaintext bytes were delivered
 *   write      encrypts into wbuf (cap bytes) and sends everything; 1 ok
 *   pending    1 while decrypted bytes wait in the buffer   reset  drop buffered plaintext
 *   close      release the backend's context; the socket is closed by the caller */
typedef struct PlatTls PlatTls;
static int plat_tls_handshake(PlatTls *t,plat_sock f,const char *host);
static int plat_tls_read(PlatTls *t,plat_sock f,uint8_t *o,uint32_t n);
static int plat_tls_write(PlatTls *t,plat_sock f,uint8_t *wbuf,uint32_t cap,const uint8_t *p,uint32_t n);
static int plat_tls_pending(const PlatTls *t);
static void plat_tls_reset(PlatTls *t);
static void plat_tls_close(PlatTls *t);

/* ---- memory, threads, time --------------------------------------------------------------------- */
#ifdef U3_PLAT_PS4
#define PLAT_THREAD_ATTR
#define PLAT_THREAD_RET void *
#else
#define PLAT_THREAD_ATTR __attribute__((ms_abi,force_align_arg_pointer))
#define PLAT_THREAD_RET uint32_t
#endif
typedef PLAT_THREAD_RET (PLAT_THREAD_ATTR *plat_thread_fn)(void *arg);
static void *plat_alloc(uint64_t bytes);                              /* committed, zeroed; 0 on failure */
static int plat_thread(plat_thread_fn fn,void *arg,uint64_t stack);   /* 0 started, -1 not */
static void plat_sleep_ms(uint32_t ms);
static int plat_utc(uint16_t st[8]);                                  /* {year,month,weekday,day,hour,min,sec,ms} UTC */

/* ---- one-line text box (PC: a native window; consoles: none) -------------------------------------
 * Opens a box with one edit field on its own thread; when OK is pressed the text is copied into out (cap
 * bytes, cleared first) and *res is set to 1; *busy is 1 while the box is open. 1 = opened. */
static int plat_textbox_open(const char *title,const char *label,const char *initial,int password,char *out,uint32_t cap,volatile uint8_t *res,volatile int32_t *busy);

/* ---- launcher hand-off (v7.59 lh01) --------------------------------------------------------------
 * Copies the value of the environment variable U3MP_HANDOFF (the launcher's sign-in settings for this launch, encoded;
 * u3_lan_profiles.c rp_handoff decodes it) into out (cap bytes, NUL-ended) and removes the variable from the process
 * environment. Returns its length; 0 when it is not set or does not fit (removed either way when it was set).
 * PC: kernelbase / kernel32 / ucrtbase exports. PS4: no launcher environment, always 0. */
static int plat_handoff(char *out,uint32_t cap);
/* v7.89 rf23: the same for U3MP_RELAY (the launcher's RPCN relay, "port:key"); 0 on PS4 */
static int plat_relay(char *out,uint32_t cap);
/* v7.90 rf24: append text to a file under the working folder from any thread (PC: kernel32; PS4: nothing) */
static int plat_diag_append(const char *path,const char *text,uint32_t n);

enum{PLAT_E_NOTLS=-1,PLAT_E_NOSYS=-2,PLAT_E_NOMEM=-3};
#endif
