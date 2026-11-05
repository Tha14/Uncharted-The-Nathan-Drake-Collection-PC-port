# ldn01: London draw-gate diagnostic (read-only). Six regs stubs count what the background cull loop 0xea1680 does
# per instance and what the instance builder reads for fade-flagged prototypes; a tick writes /data/u3-ldn-report.txt
# every ~5 s. Kinds 82..87, x-stub slots 0x14300..0x14580. U3MP_LDN_DIAG=0 at build time leaves the hooks out.
# usage: python scratch/tools/build_next.py 7.38 7.39 scratch/london/edits_ldn01.py [--install]

# --- helper: forward declaration (far above u3_profile_event; nothing is inserted next to its entry attribute)
rep('u3_lan_profiles.c', 'EXT static void x72_set(ProfileState *s);',
    'EXT static void x72_set(ProfileState *s);\nEXT static void ld_ev(int kind,uint64_t *r);   /* ldn01 */')

# --- helper: dispatch (right after the coop-score kind 81 line)
rep('u3_lan_profiles.c',
    ' if(kind==81){cs_add((uint64_t*)event);return 1;}            /* coop-score cs01: 0x7abb70 entry, count only */',
    ' if(kind==81){cs_add((uint64_t*)event);return 1;}            /* coop-score cs01: 0x7abb70 entry, count only */\n'
    ' if(kind>=82&&kind<=87){ld_ev(kind,(uint64_t*)event);return 1;}   /* ldn01 draw-gate diagnostic (scratch/london) */')

# --- helper: the counters, the event handler and the report writer (before sr_flush, which is in the same region)
LD = r'''/* ===== ldn01 (scratch/london): London draw-gate diagnostic, kinds 82..87 (160 bytes of ext data: region C is ~190 bytes under its limit) =====
 * regs block r[]: 0 r15, 1 r14, 2 r13, 3 r12, 4 rbx, 5 r11, 6 r10, 7 r9, 8 r8, 9 rdi, 10 rsi, 11 rdx, 12 rcx, 13 rax.
 * 82 0xe9b3ce instance builder, fade-flagged prototype (r15): +0x48 fade record (NULL -> engine default 0 / 1e-8).
 * 83 0xea1950 cull loop head (one per runtime instance per pass).  84 0xea196d distance-class table gate (rax = class index,
 * rsi = table): reject when table[idx+1] == 0.  85 0xea1305 PVS test (rcx = leaf bits, edi = vis id, movbe word, bt).
 * 86 0xea1dd3 fade verdict (ecx == 2 = beyond fade end -> not submitted).  87 0xea1e33 submit call 0xe99530.
 * Runtime instance = *(u64*)(r13+0x168) + r14*0x3a0, vis id = instance+0xd4 (= PS3 instance id = pak instance +4).
 * ld[]: 0 flagged 1 nullrec 2 iters 3 table_rej 4 pvs_rej 5 fade_rej 6 submits 7 pvs_tests 8 fade_tests 9 table_tests 10 tick
 *       11..17 last[2..8]  18..21 table_rej ids+1  22..24 pvs_rej ids+1  25..27 fade_rej ids+1  28..39 four {sid, start bits, end bits} */
static uint32_t ld[40] EXT_BSS;
EXT static void ld_push(uint32_t *a,int n,uint32_t v){v+=1u;for(int i=0;i<n;i++){if(a[i]==v)return;if(!a[i]){a[i]=v;return;}}}
EXT static void ld_ev(int kind,uint64_t *r){
 if(!r)return;
 if(kind==82){uint8_t *pr=(uint8_t*)r[0];uint64_t rec=*(uint64_t*)(pr+0x48);uint32_t n=ld[0]++;if(!rec)ld[1]++;
  if(n<4){ld[28+3*n]=*(uint32_t*)(pr+0xc);ld[29+3*n]=rec?*(uint32_t*)rec:0xffffffffu;ld[30+3*n]=rec?*(uint32_t*)(rec+4):0xffffffffu;}return;}
 if(kind==83){ld[2]++;return;}
 uint8_t *inst=(uint8_t*)(*(uint64_t*)(r[2]+0x168)+r[1]*0x3a0u);
 if(kind==84){uint8_t *tb=(uint8_t*)r[10];int64_t idx=(int64_t)(int32_t)r[13];ld[9]++;if(!tb[idx+1]){ld[3]++;ld_push(ld+18,4,*(uint32_t*)(inst+0xd4));}return;}
 if(kind==85){uint32_t id=(uint32_t)r[9],w=*(uint32_t*)(r[12]+(uint64_t)(id>>5)*4u);w=(w>>24)|((w>>8)&0xff00u)|((w<<8)&0xff0000u)|(w<<24);ld[7]++;
  if(!((w>>(id&31))&1u)){ld[4]++;ld_push(ld+22,3,id);}return;}
 if(kind==86){ld[8]++;if((uint32_t)r[12]==2u){ld[5]++;ld_push(ld+25,3,*(uint32_t*)(inst+0xd4));}return;}
 if(kind==87){ld[6]++;return;}}
EXT static void ld_flush(void){ProfileState *s=x72_s;if(!s||++ld[10]%300||!ld[2])return;
 char buf[640];char *q=buf,*e=buf+sizeof buf-8;
 q=fmt_append(q,e,"ldn01 tick=");q=fmt_int(q,e,ld[10],0);q=fmt_append(q,e," builder flagged=");q=fmt_int(q,e,ld[0],0);q=fmt_append(q,e," nullrec=");q=fmt_int(q,e,ld[1],0);q=fmt_append(q,e,"\n");
 for(int i=0;i<4&&ld[28+3*i];i++){q=fmt_append(q,e," sid=");q=sr_hex(q,e,ld[28+3*i]);
  q=fmt_append(q,e," start_x100=");q=fmt_int(q,e,ld[29+3*i]==0xffffffffu?-1:(int64_t)(bitsf(ld[29+3*i])*100.f),0);
  q=fmt_append(q,e," end_x100=");q=fmt_int(q,e,ld[30+3*i]==0xffffffffu?-1:(int64_t)(bitsf(ld[30+3*i])*100.f),0);q=fmt_append(q,e,"\n");}
 for(int k=2;k<=9;k++){
  q=fmt_append(q,e,k==2?"iters":k==3?"table_rej":k==4?"pvs_rej":k==5?"fade_rej":k==6?"submits":k==7?"pvs_tests":k==8?"fade_tests":"table_tests");
  q=fmt_append(q,e,"=");q=fmt_int(q,e,ld[k],0);if(k<=8){q=fmt_append(q,e," (+");q=fmt_int(q,e,ld[k]-ld[9+k],0);q=fmt_append(q,e,")");ld[9+k]=ld[k];}q=fmt_append(q,e,"\n");}
 q=fmt_append(q,e,"table_rej_ids");for(int i=18;i<22&&ld[i];i++){q=fmt_append(q,e," ");q=sr_hex(q,e,ld[i]-1u);}
 q=fmt_append(q,e,"\npvs_rej_ids");for(int i=22;i<25&&ld[i];i++){q=fmt_append(q,e," ");q=sr_hex(q,e,ld[i]-1u);}
 q=fmt_append(q,e,"\nfade_rej_ids");for(int i=25;i<28&&ld[i];i++){q=fmt_append(q,e," ");q=sr_hex(q,e,ld[i]-1u);}q=fmt_append(q,e,"\n");
 int64_t fd=((int64_t(*)(const char*,int,int))s->open)("/data/u3-ldn-report.txt",0x601,0600);if(fd<0)return;
 ((int64_t(*)(int,const void*,uint64_t))s->write)((int)fd,buf,(uint64_t)(q-buf));((int64_t(*)(int))s->close)((int)fd);}
'''
rep('u3_lan_profiles.c', 'EXT static void sr_flush(void){', LD + 'EXT static void sr_flush(void){')
rep('u3_lan_profiles.c', 'emb_rep119(base);sr_flush();tt_flush();}', 'emb_rep119(base);sr_flush();tt_flush();ld_flush();}')

# --- probe: the six regs stubs, appended to the coop-score block
rep('build_u3_profile_probe.py',
    " at,w=regs_stub(0x14280,81,lambda h:o,0x7abb78);agc_site(0x7abb70,o,at,w,81)",
    " at,w=regs_stub(0x14280,81,lambda h:o,0x7abb78);agc_site(0x7abb70,o,at,w,81)\n"
    " # ldn01 (scratch/london/edits_ldn01.py): London draw-gate diagnostic, kinds 82..87, slots 0x14300..0x14580, report\n"
    " # /data/u3-ldn-report.txt. No branch targets inside any displaced range (checked against the full disassembly).\n"
    " if os.environ.get('U3MP_LDN_DIAG','1')=='1':\n"
    "  o=bytes.fromhex('498b57484885d2');agc_check(0xe9b3ce,o,'ldn01 builder fade');at,w=regs_stub(0x14300,82,lambda h:o,0xe9b3d5);agc_site(0xe9b3ce,o,at,w,82)\n"
    "  o=bytes.fromhex('498b8d68010000');agc_check(0xea1950,o,'ldn01 loop head');at,w=regs_stub(0x14380,83,lambda h:o,0xea1957);agc_site(0xea1950,o,at,w,83)\n"
    "  o=bytes.fromhex('807c300100');agc_check(0xea196d,o,'ldn01 table gate');at,w=regs_stub(0x14400,84,lambda h:o,0xea1972);agc_site(0xea196d,o,at,w,84)\n"
    "  o=bytes.fromhex('89fac1ea05');agc_check(0xea1305,o,'ldn01 pvs gate');at,w=regs_stub(0x14480,85,lambda h:o,0xea130a);agc_site(0xea1305,o,at,w,85)\n"
    "  o=bytes.fromhex('83f902746c');agc_check(0xea1dd3,o,'ldn01 fade verdict');at,w=regs_stub(0x14500,86,lambda h:b'\\x83\\xf9\\x02\\x0f\\x84'+P('<i',0xea1e44-(h+9)),0xea1dd8);agc_site(0xea1dd3,o,at,w,86)\n"
    "  o=bytes.fromhex('e8f876ffff');agc_check(0xea1e33,o,'ldn01 submit');at,w=regs_stub(0x14580,87,lambda h:b'\\xe8'+P('<i',0xe99530-(h+5)),0xea1e38);agc_site(0xea1e33,o,at,w,87)")
