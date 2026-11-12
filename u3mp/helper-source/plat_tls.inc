/* plat_tls.inc - the TLS client shared by both backends (PLAN.md part C): BearSSL 0.6 (u3_bearssl.inc, MIT),
 * included from plat_pc.inc and plat_ps4.inc after the plat_sock_* / plat_random bodies, inside the .text.ext
 * pragma. One code path on PC and PS4; the SChannel path of v7.53/v7.54 is gone.
 *
 * Profile: TLS 1.2 only; ECDHE on P-256 (br_ec_p256_m31) with RSA (PKCS#1 v1.5, br_rsa_i31_pkcs1_vrfy) or ECDSA
 * (br_ecdsa_i31_vrfy_asn1) signatures; AES-128/256-GCM (SHA-256 / SHA-384 PRF) and ChaCha20-Poly1305. Offered in
 * this order: C02B ECDHE_ECDSA_AES128_GCM, C02F ECDHE_RSA_AES128_GCM, CCA9 ECDHE_ECDSA_CHACHA20, CCA8
 * ECDHE_RSA_CHACHA20, C02C ECDHE_ECDSA_AES256_GCM, C030 ECDHE_RSA_AES256_GCM: the TLS 1.2 set of rustls (the
 * account server is a Rust/rustls service; its client in RPCS3 uses wolfSSL with verification off) and of OpenSSL.
 *
 * Certificate: the server's certificate is self-signed, and the previous SChannel code did not validate it
 * (SCH_CRED_MANUAL_CRED_VALIDATION, no check), so the X.509 engine here is "plat_x509": BearSSL's br_x509_decoder
 * run over the first (end-entity) certificate only, to extract its public key for the ServerKeyExchange signature;
 * the chain is NOT validated and the name is not checked (same trust as before: the TLS gives privacy against
 * passive listeners, not server authentication). Switching to a pinned key later = compare the decoded key with a
 * stored one in plat_x509_end_chain.
 *
 * Entropy: br_prng_seeder_system below feeds plat_random() (PC: RtlGenRandom, PS4: sceNetGetRandom) into BearSSL's
 * HMAC-DRBG at every handshake, plus rdtsc and the caller's addresses as extra input.
 *
 * Diagnostics: X.room[14] = 0 after a good handshake, 0x1000 | BearSSL error (bearssl_ssl.h BR_ERR_*) for an engine
 * error, otherwise the raw socket error (as before). */
#ifndef U3_PLAT_TLS_INC
#define U3_PLAT_TLS_INC
#ifdef U3_NO_TLS
/* the public (GitHub) build: no TLS client at all; the sign-in code is stripped there as well */
struct PlatTls { uint32_t ready; };
static int plat_tls_handshake(PlatTls *t,plat_sock f,const char *host){(void)t;(void)f;(void)host;X.room[14]=(uint32_t)PLAT_E_NOTLS;return 0;}
static int plat_tls_read(PlatTls *t,plat_sock f,uint8_t *o,uint32_t n){(void)t;(void)f;(void)o;(void)n;return 0;}
static int plat_tls_write(PlatTls *t,plat_sock f,uint8_t *wbuf,uint32_t cap,const uint8_t *p,uint32_t n){(void)t;(void)f;(void)wbuf;(void)cap;(void)p;(void)n;return 0;}
static int plat_tls_pending(const PlatTls *t){(void)t;return 0;}
static void plat_tls_reset(PlatTls *t){t->ready=0;}
static void plat_tls_close(PlatTls *t){t->ready=0;}
#else
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)   /* v7.106: size first (sign-in only) */
#include "u3_bearssl.inc"
#pragma clang attribute pop
#pragma clang section text=".text.ext" rodata=".rodata.ext"
/* ---- plat_x509: public key of the end-entity certificate, no validation (see the header comment) ---- */
typedef struct { const br_x509_class *vtable; br_x509_decoder_context dec; int depth,err; } PlatX509;
static void plat_x509_start_chain(const br_x509_class **ctx,const char *server_name){PlatX509 *x=(PlatX509*)ctx;(void)server_name;x->depth=0;x->err=0;}
static void plat_x509_start_cert(const br_x509_class **ctx,uint32_t length){PlatX509 *x=(PlatX509*)ctx;(void)length;x->depth++;if(x->depth==1)br_x509_decoder_init(&x->dec,0,0);}
static void plat_x509_append(const br_x509_class **ctx,const unsigned char *buf,size_t len){PlatX509 *x=(PlatX509*)ctx;if(x->depth==1)br_x509_decoder_push(&x->dec,buf,len);}
static void plat_x509_end_cert(const br_x509_class **ctx){PlatX509 *x=(PlatX509*)ctx;if(x->depth==1){int e=br_x509_decoder_last_error(&x->dec);if(e)x->err=e;else if(!br_x509_decoder_get_pkey(&x->dec))x->err=BR_ERR_X509_WRONG_KEY_TYPE;}}
static unsigned plat_x509_end_chain(const br_x509_class **ctx){PlatX509 *x=(PlatX509*)ctx;if(x->depth==0)return BR_ERR_X509_EMPTY_CHAIN;return (unsigned)x->err;}
static const br_x509_pkey *plat_x509_get_pkey(const br_x509_class *const *ctx,unsigned *usages){PlatX509 *x=(PlatX509*)ctx;if(usages)*usages=BR_KEYTYPE_KEYX|BR_KEYTYPE_SIGN;return br_x509_decoder_get_pkey(&x->dec);}
#pragma clang section relro=".data.u3bssl"
static const br_x509_class plat_x509_vtable={sizeof(PlatX509),plat_x509_start_chain,plat_x509_start_cert,plat_x509_append,plat_x509_end_cert,plat_x509_end_chain,plat_x509_get_pkey};
#pragma clang section relro=""
/* ---- entropy ---- */
static int plat_tls_seed(const br_prng_class **ctx){
 uint8_t s[48];uint64_t t=__builtin_ia32_rdtsc();copy(s,&t,8);uintptr_t a=(uintptr_t)ctx;copy(s+8,&a,8);
 if(plat_random(s+16,32)<0){for(int i=16;i<48;i++)s[i]=(uint8_t)(t>>((i&7)*8))^(uint8_t)(a>>((i&7)*8))^(uint8_t)i;}
 (*ctx)->update(ctx,s,sizeof(s));clear(s,sizeof(s));return 1;}
br_prng_seeder br_prng_seeder_system(const char **name){if(name)*name="u3plat";return &plat_tls_seed;}
/* ---- the connection ---- */
struct PlatTls { br_ssl_client_context cc; PlatX509 x; uint32_t ready; unsigned char iobuf[BR_SSL_BUFSIZE_BIDI]; };
static const uint16_t plat_tls_suites[]={BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
 BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384};
static int plat_tls_step(PlatTls *t,plat_sock f){   /* one record exchange with the socket; 0 = socket closed / error / engine closed */
 br_ssl_engine_context *e=&t->cc.eng;unsigned st=br_ssl_engine_current_state(e);
 if(st&BR_SSL_CLOSED){X.room[14]=0x1000u|(uint32_t)br_ssl_engine_last_error(e);return 0;}
 if(st&BR_SSL_SENDREC){size_t n;unsigned char *b=br_ssl_engine_sendrec_buf(e,&n);int r=plat_sock_send(f,b,(int)n,0);if(r<=0){X.room[14]=(uint32_t)plat_sock_err();return 0;}br_ssl_engine_sendrec_ack(e,(size_t)r);return 1;}
 if(st&BR_SSL_RECVREC){size_t n;unsigned char *b=br_ssl_engine_recvrec_buf(e,&n);int r=plat_sock_recv(f,b,(int)n,0);if(r<=0){X.room[14]=r<0?(uint32_t)plat_sock_err():0;return 0;}br_ssl_engine_recvrec_ack(e,(size_t)r);return 1;}
 return 0;}
static int plat_tls_handshake(PlatTls *t,plat_sock f,const char *host){
 u3_fix_apply();
 br_ssl_client_context *cc=&t->cc;br_ssl_engine_context *e=&cc->eng;
 br_ssl_client_zero(cc);t->ready=0;
 br_ssl_engine_set_versions(e,BR_TLS12,BR_TLS12);
 br_ssl_engine_set_suites(e,plat_tls_suites,sizeof(plat_tls_suites)/sizeof(plat_tls_suites[0]));
 br_ssl_engine_set_hash(e,br_sha256_ID,&br_sha256_vtable);br_ssl_engine_set_hash(e,br_sha384_ID,&br_sha384_vtable);
 br_ssl_engine_set_prf_sha256(e,&br_tls12_sha256_prf);br_ssl_engine_set_prf_sha384(e,&br_tls12_sha384_prf);
 br_ssl_engine_set_aes_ctr(e,&br_aes_ct64_ctr_vtable);br_ssl_engine_set_ghash(e,&br_ghash_ctmul64);br_ssl_engine_set_gcm(e,&br_sslrec_in_gcm_vtable,&br_sslrec_out_gcm_vtable);
 br_ssl_engine_set_chacha20(e,&br_chacha20_ct_run);br_ssl_engine_set_poly1305(e,&br_poly1305_ctmul_run);br_ssl_engine_set_chapol(e,&br_sslrec_in_chapol_vtable,&br_sslrec_out_chapol_vtable);
 br_ssl_engine_set_ec(e,&br_ec_p256_m31);br_ssl_engine_set_ecdsa(e,&br_ecdsa_i31_vrfy_asn1);br_ssl_engine_set_rsavrfy(e,&br_rsa_i31_pkcs1_vrfy);
 t->x.vtable=&plat_x509_vtable;br_ssl_engine_set_x509(e,&t->x.vtable);
 br_ssl_engine_set_buffer(e,t->iobuf,sizeof(t->iobuf),1);
 {uint8_t s[32];if(plat_random(s,32)==0)br_ssl_engine_inject_entropy(e,s,32);uint64_t k=__builtin_ia32_rdtsc();br_ssl_engine_inject_entropy(e,&k,8);clear(s,sizeof(s));}
 if(!br_ssl_client_reset(cc,host,0)){X.room[14]=0x1000u|(uint32_t)br_ssl_engine_last_error(e);return 0;}
 for(;;){
  unsigned st=br_ssl_engine_current_state(e);
  if(st&BR_SSL_CLOSED){X.room[14]=0x1000u|(uint32_t)br_ssl_engine_last_error(e);return 0;}
  if(st&BR_SSL_SENDAPP)break;                     /* handshake done: application data may be sent */
  if(!plat_tls_step(t,f))return 0;}
 t->ready=1;X.room[14]=0;return 1;}
static int plat_tls_read(PlatTls *t,plat_sock f,uint8_t *o,uint32_t n){
 if(!t->ready)return 0;br_ssl_engine_context *e=&t->cc.eng;
 while(n){
  size_t have;unsigned char *b=br_ssl_engine_recvapp_buf(e,&have);
  if(b&&have){uint32_t c=have>n?n:(uint32_t)have;for(uint32_t i=0;i<c;i++)o[i]=b[i];br_ssl_engine_recvapp_ack(e,c);o+=c;n-=c;continue;}
  if(!plat_tls_step(t,f))return 0;}
 return 1;}
static int plat_tls_write(PlatTls *t,plat_sock f,uint8_t *wbuf,uint32_t cap,const uint8_t *p,uint32_t n){
 (void)wbuf;(void)cap;if(!t->ready)return 0;br_ssl_engine_context *e=&t->cc.eng;
 while(n){
  size_t room;unsigned char *b=br_ssl_engine_sendapp_buf(e,&room);
  if(b&&room){uint32_t c=room>n?n:(uint32_t)room;for(uint32_t i=0;i<c;i++)b[i]=p[i];br_ssl_engine_sendapp_ack(e,c);p+=c;n-=c;continue;}
  if(!plat_tls_step(t,f))return 0;}
 br_ssl_engine_flush(e,0);
 for(;;){unsigned st=br_ssl_engine_current_state(e);if(st&BR_SSL_CLOSED){X.room[14]=0x1000u|(uint32_t)br_ssl_engine_last_error(e);return 0;}if(!(st&BR_SSL_SENDREC))break;if(!plat_tls_step(t,f))return 0;}
 return 1;}
static int plat_tls_pending(const PlatTls *t){if(!t->ready)return 0;size_t n;return br_ssl_engine_recvapp_buf((br_ssl_engine_context*)&t->cc.eng,&n)!=0&&n!=0;}
static void plat_tls_reset(PlatTls *t){t->ready=0;}
static void plat_tls_close(PlatTls *t){t->ready=0;}   /* no close_notify: the caller closes the socket, as the SChannel path did */
#endif
#endif
