"""Add native IME/file-provider bindings and restored LAN profile callbacks.

Input is the frozen menu-shell-v1 prepared image. Originals remain read-only.
"""
from pathlib import Path
import struct,hashlib
from build_u3_bootstrap_probe import parse
from build_u3_pool_blob import split_blob,EXT_CODE_SIZE,EXT_DATA_SIZE
IMPORTS=[('NUeBrN7hzf0','sceImeDialogInit','libSceImeDialog'),('IADmD4tScBY','sceImeDialogGetStatus','libSceImeDialog'),('x01jxu+vxlc','sceImeDialogGetResult','libSceImeDialog'),('gyTyVn+bXMw','sceImeDialogTerm','libSceImeDialog'),('1G3lF1Gg1k8','sceKernelOpen','libkernel'),('Cg4srZ6TKbU','sceKernelRead','libkernel'),('4wSze92BhLI','sceKernelWrite','libkernel'),('UK2Tl2DWUns','sceKernelClose','libkernel'),('52NcYU9+lEo','sceKernelRename','libkernel')]
# v4.93: libSceNet imports for the helper's LAN discovery, written to the ext data segment (ExtState.net[] at rw2+0x200,
# order = NETF_* in u3_lan_profiles.c). The PC port answers the socket calls with ENETUNREACH unless GOW3_NET_SOCKETS=1.
NET_IMPORTS=[('Q4qBuN-c0ZM','sceNetSocket','libSceNet'),('45ggEzakPJQ','sceNetSocketClose','libSceNet'),('bErx49PgxyY','sceNetBind','libSceNet'),('gvD1greCu0A','sceNetSendto','libSceNet'),('304ooNZxWDY','sceNetRecvfrom','libSceNet'),('2mKX2Spso7I','sceNetSetsockopt','libSceNet'),('obuxdTiwkF8','sceNetCtlGetInfo','libSceNetCtl'),
 # v4.98 LAN room (TCP 9013, PS3 room protocol c9..d1): NETF_CONNECT..NETF_RECV = 7..11
 ('OXXX4mUk3uk','sceNetConnect','libSceNet'),('kOj1HiAGE54','sceNetListen','libSceNet'),('PIWqhn9oSxc','sceNetAccept','libSceNet'),('beRjXBn-z+o','sceNetSend','libSceNet'),('9wO9XrMsNhc','sceNetRecv','libSceNet')]
# Native PS4 functions exposed to the helper as ProfileState.natives[i] (u3_lan_profiles.c NATIVE_*).
NATIVES=[0x623660,0x7503c0,0x720610,0x750a90,0xf32ae0,0x752cf0,0x753330,0x66e9b0,0x72eca0,0x11fd4c0,0x66b1d0,0x66b510,0x7a2360,0x11f2e30,0x64ce90,0x76bbf0,0x7699e0,0x76a580,0x11f2170,0x5ada60,0x1189430,0x781ea0,0x753b70,0x6882b0,0x7bb470,0x7bba70,0x751b30,0x7270f0,0x780220,0x7596a0,0x687d30,0x775d30,0x7bda50,0x74eba0,0x7800d0,0x7bca00,0x7bd6f0,0x7bdc70,0xf32e00,0x771cf0,0x10e95b0,0x10eb990,0x10eb1d0,0x761ea0,0x7508f0,0x10f25b0,0x5b7020,0x5b4e60,0x10ebaa0,0x996db0,0x10eb380,0x575230,0x5752a0,
 # v4.86 local-player respawn machine (u3_lan_profiles.c NATIVE_NETPLAYER_SPAWN .. NATIVE_DC_TYPE_ID, 53..62)
 0x787a00,0x7838b0,0x786250,0x793280,0x11a6250,0x10ec0c0,0x10ebe40,0x10ef200,0x10ee3f0,0x125fdb0]
# Bluepoint gated every non-whitelisted menu datasource (all but 0x63/0xcf/0xe1) behind
# net-game+9 (0x17d5c61, global multiplayer-active flag, never set on PS4). Gate them on
# net-game+8 (0x17d5c60, LAN flag, set when LAN Party starts) instead: cmp byte [reg+9],0 -> [reg+8].
DS_GATES=[int(x,16) for x in '0x764045 0x764191 0x7642f5 0x7644e9 0x764639 0x7647b5 0x764901 0x764a51 0x764ba5 0x764cf9 0x764e57 0x764fa9 0x7650ff 0x765249 0x7653b9 0x765519 0x76568f 0x7657d5 0x765925 0x765a81 0x765be9 0x765d31 0x765e79 0x765fbb 0x766105 0x7703b2 0x7704d5'.split()]
EXPECTED='8979cc0d2e0590d61bdf7935ab8c29e617010aa2aa096584cd868d004058554b'
def build(raw,blob,pool_blob=None):
 if hashlib.sha256(raw).hexdigest()!=EXPECTED:raise ValueError('requires frozen menu-shell-v1 input')
 # v4.93: the helper may be a U3HX container (build_u3_pool_blob.py split link): region A goes to rx+0x1000 as
 # before, region B (.text.ext*) to the new code segment rx2 = rw+0x4000 and region C (writable sections) to the
 # new data segment rw2 = rx2+EXT_CODE_SIZE. rx, rw and state (0x2064100) keep their addresses.
 blob,ext_code,ext_code_delta,ext_data_size,ext_data_init,ext_data_delta=split_blob(blob)
 if not blob or len(blob)>0xe000:raise ValueError('profile helper size')
 if pool_blob is None:pool_blob=(Path(__file__).resolve().parent.parent/'work/u3_pool_init.bin').read_bytes()
 if not pool_blob or len(pool_blob)>0x1000:raise ValueError('pool helper size')
 size,ns,sp,np,ip,segs=parse(raw);nr,ni=struct.unpack_from('<2Q',raw,32)
 rx=(size+0x3fff)&~0x3fff;rw=rx+0x10000;state=rw+0x100;helper=rx+0x1000;pool=rx+0xf000
 rx2=rw+0x4000;rw2=rx2+EXT_CODE_SIZE
 if ext_code_delta!=rx2-helper or ext_data_delta!=rw2-helper:raise ValueError('helper linked for another segment layout')
 if len(ext_code)>EXT_CODE_SIZE or ext_data_size>EXT_DATA_SIZE or len(ext_data_init)>ext_data_size:raise ValueError('ext helper size')
 image=bytearray(raw[ip:]);code=bytearray(0x10000);XSTUB=0x8000;xcode=bytearray(XSTUB)   # v5.81 (main): 256 slots 0x10000..0x17f80 (0x14000.. new: A 0x14000-0x14f80, B 0x15000-0x15f80, C 0x16000-0x16f80, TEST 0x17000-0x17f80). v5.66: was 0x2000 (64 slots, all allocated); now 128 slots 0x10000..0x13f80
 code[0x1000:0x1000+len(blob)]=blob
 code[0xf000:0xf000+len(pool_blob)]=pool_blob
 guards={0x672c60:bytes.fromhex('c36666666666662e0f1f840000000000'),0x673450:bytes.fromhex('c36666666666662e0f1f840000000000'),0x76f4d0:bytes.fromhex('554889e541574156415453'),0x672c30:bytes.fromhex('c36666666666662e0f1f840000000000'),0x66e560:bytes.fromhex('c36666666666662e0f1f840000000000'),0x66edf0:bytes.fromhex('c36666666666662e0f1f840000000000'),0x672c50:bytes.fromhex('c36666666666662e0f1f840000000000'),0x672450:bytes.fromhex('c36666666666662e0f1f840000000000')}
 wrappers=[]
 for site,offset,kind in [(0x672c60,0,0),(0x673450,0x80,1),(0x76f4d0,0x100,2),(0x672c30,0x200,3),(0x66e560,0x280,6),(0x66edf0,0x300,9),(0x672c50,0x800,11),(0x672450,0x880,12)]:
  expected=guards[site]
  if image[site:site+len(expected)]!=expected:raise ValueError('profile hook signature mismatch '+hex(site))
  at=rx+offset;w=bytearray.fromhex('4883ec18 48893c24 4889fe')
  here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
  w.extend(b'\xba'+struct.pack('<I',kind))
  here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
  if kind==2:
   w.extend(bytes.fromhex('85c0'));branch=len(w);w.extend(b'\x0f\x85'+bytes(4))
   w.extend(bytes.fromhex('488b3c244883c418'));w.extend(expected)
   here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',site+len(expected)-(here+5)))
   struct.pack_into('<i',w,branch+2,len(w)-(branch+6))
  w.extend(bytes.fromhex('4883c418c3'))
  if len(w)>0x80:raise ValueError('wrapper overlap')
  code[offset:offset+len(w)]=w
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=kind,original=expected.hex(),wrapper_hex=w.hex()))

 # WaitReady (update 0x67f240) starts a Cinema film recording through stream 0x17c3ba8; the PS4
 # film path setup is missing (path "/-4851"), the open fails and 0x6a2110 dereferences the null
 # bitstream. Skip film recording: the function only builds the film title and starts it.
 # x023: PS3 1.19 LoadLevels f_28d104 0x28d3a0: a guest (f_3275f0 == 0) also reports ready (f_2f3538) and enters WaitReady. PS4 0x67fa49 skips that
 # for a guest, so it stayed in LoadLevels and the host's WaitReady never got its +0x5ed.
 if image[0x67fa49:0x67fa4f]!=bytes.fromhex('759d31ffe85e'):raise ValueError('LoadLevels guest branch mismatch')
 image[0x67fa49:0x67fa4b]=bytes.fromhex('9090')
 # x024: HudNet::Draw 0xc32ba0 gets no render context on PS4 (0x570db5 passes 0; PS3 hands down *(sceneWindow+0x28)), and its element draw 0xc2fd10 binds
 # the 0x1acaaf8 constants on that context before queueing. That section runs once a remote player has a HUD element: skip the bind, the rest does not use it.
 if image[0xc2fd7b:0xc2fd80]!=bytes.fromhex('4d85ff7457'):raise ValueError('HUD element bind mismatch')
 image[0xc2fd7e]=0xeb
 if image[0x67f240:0x67f244]!=bytes.fromhex('554889e5'):raise ValueError('film recording hook mismatch')
 # v5.47: U3MP_CINEMA=1 replaces this RET with the cinema port (value stub kind 54 at 0x67f240, see below), which sets up
 # the film directory and slot first and skips the recording (as the RET did) whenever that fails. Default off.
 # agent B b001: the cinema port is default ON (U3MP_CINEMA=0 restores the RET and leaves every cinema site out).
 CINEMA=__import__('os').environ.get('U3MP_CINEMA','1')!='0'
 if not CINEMA:image[0x67f240]=0xc3

 # HUD-net (class "David", vtable 0x16dafe0) slot 3 = PS3 0x6603f4: draws a script-set HUD image for the
 # local player. Bluepoint removed the draw and left int 0x41 at 0xc3b822, reached whenever a local
 # net player tracker exists. Replace the trap with a 2-byte NOP (nothing drawn).
 if image[0xc3b822:0xc3b824]!=b'\xcd\x41':raise ValueError('hud trap mismatch')
 image[0xc3b822:0xc3b824]=b'\x66\x90'
 # v7.56b: NpcManager::Register 0x342cb0 asserts m_npcCount < 9 (npc-manager.cpp:1193) before adding an NPC, but
 # the table holds 32 (the add below checks 0x20 / 0x1f). PS3 1.19's retail build has the assert compiled out; co-op
 # adventures go past 8 (London chapter 2 end crashed with int 0x41). jl over the assert -> jmp: the 32 limit stays.
 if image[0x342ced:0x342cf2]!=bytes.fromhex('833b097c35'):raise ValueError('npc count assert mismatch')
 image[0x342cf0]=0xeb
 # v7.56c: the story characters Chloe / Elena / Marlowe register their process type with a 0x24400-byte heap
 # (static initializers 0x394120 / 0x398b80 / 0x39d710). Co-op London chapter 2's final cutscene ran one full
 # ("Allocation failed: 5136 bytes ... Unnamed [F:3056 T:148480]") once more than 8 enemies were up. Doubled.
 for at in (0x3943ba,0x399fba,0x39d98a):
  if image[at:at+4]!=bytes.fromhex('00440200'):raise ValueError('character heap size mismatch')
  image[at:at+4]=(0x48800).to_bytes(4,'little')
 # v5.28: port the PS3 draw (kind 52, see hud_spotted_draw). The whole slot is replaced: jmp at 0xc3b770 -> full-save stub,
 # helper, then ret (the PS4 body only had the checks and the trap). U3MP_SPOTTED_HUD=0 keeps the NOP-only shortcut.
 # (Installed later, after fullsave_stub exists: see 'v5.28 slot 3'.)
 # Lobby camera 0x5b4e60: PS3 (0x207458) synced the lobby character's anim layer 0x7339d08c to the
 # phase of anim 0x73fd4b71 when present; Bluepoint replaced it with int 0x41 (0x5b6113 in the camera setup,
 # 0x5b739b in the per-frame MenuScene update). Both traps become jumps to the anim-absent path (cosmetic sync only).
 for site,jmp in ((0x5b6113,b'\xeb\x11'),(0x5b739b,b'\xeb\x7c')):
  if image[site:site+2]!=b'\xcd\x41':raise ValueError('menu scene trap mismatch '+hex(site))
  image[site:site+2]=jmp

 # MP entity-spawn filter 0x72fbc0 binary-searches the entity's tag table (entity+0x68: {data,count})
 # without a count check. PS4 level data stores empty tables with a null data pointer (PS3 had a valid
 # pointer past the end), so MP level loads fault at 0x72fd62. Point empty tables at a one-entry
 # sentinel (key 0xffffffff, never matches) before the call; count stays 0, so PS3 semantics are kept.
 tgt=0x72fbc0;at=rx+0x420;sent=state+0x2100
 w=bytearray.fromhex('488b4668 4885c0 7416 48833800 7510')
 here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',sent-(here+7)))
 w.extend(bytes.fromhex('c701ffffffff 488908'))
 here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',tgt-(here+5)))
 assert w[7]==0x74 and w[13]==0x75 and 9+0x16==len(w)-5 and 15+0x10==len(w)-5
 code[0x420:0x420+len(w)]=w
 # v5.25: retail PS4 itself stores a few empty tables as {null, 0} (menu/ap spawners, e.g. yemen-airport, ldn-multi-comp);
 # the converted paks get a terminator entry since ASSETS batch37 (PS3 never has null data). U3MP_TAGTABLE_GUARD=0 leaves
 # the 12 call sites unredirected to test the data alone.
 for site in (0x56561c,0x722939,0x72f9ea,0x730eeb,0x732d09,0x73a6e3,0x73b0d4,0x73b21a,0x73ed30,0x744578,0x777513,0x7778a2):
  if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=tgt:raise ValueError('tag filter site mismatch '+hex(site))
  if __import__('os').environ.get('U3MP_TAGTABLE_GUARD','1')!='0':image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))
 wrappers.append(dict(site='12 sites',address=hex(at),kind='empty-tag-table',wrapper_hex=w.hex()))

 # DIAGNOSTIC (temporary): along the PS4 respawn/spawn chain record {count, low32(ret), esi arg} at
 # state+0x2200+i*16. Works on every thread (debug-register watches only see the main thread).
 # 0 0x6b4c0f->0x70d180 local respawn request   1 0x70d71f->0x6b41e0 send RespawnPlayer
 # 2 0x6e9e9c->0x732bd0 / 3 0x6e9eae->0x730a00 spawner pick   4 0x6e8dc4->0x7854c0 net player spawn
 # 5..11 callers of NetPlayer spawn 0x787a00   12 0x787cf2->0x7838b0 GetSpawnLocator
 # 13/14 process spawns 0x119b250 inside net player update 0x789a60 (0x78aed2 = character)
 diag=((0x6b4c0f,0x70d180),(0x70d71f,0x6b41e0),(0x6e9e9c,0x732bd0),(0x6e9eae,0x730a00),(0x6e8dc4,0x7854c0),
  (0x7248a4,0x787a00),(0x7388aa,0x787a00),(0x738bc8,0x787a00),(0x738dbc,0x787a00),(0x739cfb,0x787a00),(0x739d2b,0x787a00),(0x73c532,0x787a00),
  (0x787cf2,0x7838b0),(0x78a32b,0x119b250),(0x78aed2,0x119b250))
 for i,(site,tgt) in enumerate(diag):
  if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=tgt:raise ValueError('diag site mismatch '+hex(site))
  at=rx+0x600+i*0x20;cnt=state+0x2200+i*16
  w=bytearray.fromhex('4883ec08')
  here=at+len(w);w.extend(b'\x89\x35'+struct.pack('<i',cnt+8-(here+6)))
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',tgt-(here+5)))
  here=at+len(w);w.extend(b'\xff\x05'+struct.pack('<i',cnt-(here+6)))
  here=at+len(w);w.extend(b'\x89\x05'+struct.pack('<i',cnt+4-(here+6)))
  w.extend(bytes.fromhex('4883c408c3'));assert len(w)<=0x20
  code[0x600+i*0x20:0x600+i*0x20+len(w)]=w
  # s001: U3MP_SPAWN_DIAG=0 leaves the 15 diagnostic call redirects out (diagnostic only; counters at state+0x2200)
  if __import__('os').environ.get('U3MP_SPAWN_DIAG','1')!='0':image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))

 # NetPlayer spawn 0x787a00, local player without a character (0x787b32 path): PS3 f_360af8 stored the
 # spawner and then called f_3609cc, which raised RequestCharacter (PS3 +0x6a0/+0x6a4/+0x6a8 = PS4
 # +0x770/+0x774/+0x778; the PS4 setter 0x785480 has no callers left). Without it the net player update
 # 0x789a60 never reaches character creation (it needs +0x770). Restore the request before returning.
 # v4.69: NetCharacter::Init (0x690190 loop over 5 kit parts) asserts at 0x69025c when a part's actor
 # package is not loaded. Request it (LevelMgr want-load actor 0x10ebaa0(mgr r14, name r13, 0, 0)) and
 # skip the part instead, so a late loadout costs one missing piece instead of the session.
# v4.76: match setup 0x72b570 unloads the menu loadout (0x997690: drops the kit actors from the
 # want list, clears the custom manager) before the net characters spawn; PS3 kept the local players'
 # kits streamed (f_411444 every frame). Keep them: skip that call (0x72bda9).
# v4.77 (test): skip the lobby character placement (MenuScene 0x5b7240 -> 0x5b6640). Rendering it
 # faults in a frame resource set (0xe747e3); this isolates whether the fault follows the kit models
 # into the match. v5.12: the cause was the converter's per-instance cbuffer capacity (fixed v4.79, kits re-converted),
# so the placement is on again by default as on PS3 (f_206e60 puts the local character on menu-spawner-1 of the
# lobby stage). U3MP_NO_LOBBY_CHAR=1 restores the skip.
 import os
 if os.environ.get('U3MP_NO_LOBBY_CHAR')=='1':
  site=0x5b76e8
  if image[site:site+5]!=b'\xe8'+struct.pack('<i',0x5b6640-(site+5)):raise ValueError('lobby character call mismatch')
  image[site:site+5]=bytes.fromhex('0f1f440000')
  wrappers.append(dict(site=hex(site),address=hex(site),kind='no-lobby-character',wrapper_hex='0f1f440000'))
# diagnostics: level error path (0x10f45b7, reached only when a level is about to go to state 12)
 # calls helper kind 14 with the Level, which records the name and its package request statuses.
 site=0x10f45b7
 if image[site:site+11]!=bytes.fromhex('41837d300c0f8460150000'):raise ValueError('level error path mismatch')
 at=rx+0xa00;w=bytearray.fromhex('50 51 52 56 57 4150 4151 4152 4153 4883ec08')
 here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
 w.extend(bytes.fromhex('4c89ee'))
 w.extend(b'\xba'+struct.pack('<I',14))
 here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
 w.extend(bytes.fromhex('4883c408 415b 415a 4159 4158 5f 5e 5a 59 58'))
 w.extend(bytes.fromhex('41837d300c'))
 here=at+len(w);w.extend(b'\x0f\x84'+struct.pack('<i',0x10f5b22-(here+6)))
 here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',0x10f45c2-(here+5)))
 code[0xa00:0xa00+len(w)]=w
 image[site:site+11]=b'\xe9'+struct.pack('<i',at-(site+5))+bytes.fromhex('0f1f440000 90')
 wrappers.append(dict(site=hex(site),address=hex(at),kind='level-error-diag',wrapper_hex=w.hex()))
# v4.79: frame resource set guard. The cbuffer update loop 0xe87a50 takes each binding's resource
 # pointer from its entry (+0); with several converted kits on one character a binding carries a
 # non-pointer (0x33d5c036f1b17bc2) and 0xe747c0 faults reading it. Skip such a binding (the
 # loop's own "not bound" path 0xe88218) and record the first one at state+0x2700 for diagnosis.
 site=0xe87ba9
 if image[site:site+8]!=bytes.fromhex('4a8b1428488955c0'):raise ValueError('cbuffer loop mismatch')
 at=rx+0xb00;w=bytearray.fromhex('4a8b1428 488955c0 51 4889d1 48c1e928 59')
 jbad=len(w);w.extend(b'\x0f\x85'+bytes(4))
 here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',0xe87bb1-(here+5)))
 struct.pack_into('<i',w,jbad+2,len(w)-(jbad+6))
 w.extend(bytes.fromhex('51 56'))
 here=at+len(w);w.extend(b'\x48\x8d\x35'+struct.pack('<i',state+0x2700-(here+7)))
 w.extend(bytes.fromhex('ff06 833e01'));jd=len(w);w.extend(b'\x0f\x85'+bytes(4))
 w.extend(bytes.fromhex('4c894e08 4c896e10 4c897e18 48894620 418b09 48894e28 488b4d08 48894e30'))
 for i in range(6):w.extend(bytes.fromhex('4a8b4c28%02x 48894e%02x'%(i*8,0x38+i*8)))
 w.extend(bytes.fromhex('4a8b542810 4889d1 48c1e928'));jn=len(w);w.extend(b'\x75\x00')
 w.extend(bytes.fromhex('4885d2'));jz=len(w);w.extend(b'\x74\x00')
 for i in range(3):w.extend(bytes.fromhex('488b4a%02x 48894e%02x'%(i*8,0x68+i*8)))
 w[jn+1]=len(w)-(jn+2);w[jz+1]=len(w)-(jz+2)
 struct.pack_into('<i',w,jd+2,len(w)-(jd+6))
 w.extend(bytes.fromhex('5e 59'))
 here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',0xe88218-(here+5)))
 assert len(w)<0x100
 code[0xb00:0xb00+len(w)]=w
 # s001: U3MP_CBUF_GUARD=0 leaves the cbuffer guard out (bisect only: a non-pointer binding then faults in 0xe747c0)
 if __import__('os').environ.get('U3MP_CBUF_GUARD','1')!='0':image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+bytes.fromhex('0f1f00')
 wrappers.append(dict(site=hex(site),address=hex(at),kind='cbuffer-guard',wrapper_hex=w.hex(),state=hex(state)))
# v4.79: Material::Apply 0xe90510, after the resource gather 0xe8f130: helper kind 15 replaces
 # resource slots holding a non-pointer with the default resource and records the first (state+0x2780).
 site=0xe90701
 if image[site:site+9]!=bytes.fromhex('48895c2438458b7e10'):raise ValueError('material apply mismatch')
 at=rx+0xc00;w=bytearray.fromhex('50 51 52 56 57 4150 4151 4152 4153 4157 4155 53 488d442460 50 4889e6 4883ec08')
 here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
 w.extend(b'\xba'+struct.pack('<I',15))
 here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
 w.extend(bytes.fromhex('4883c428 415b 415a 4159 4158 5f 5e 5a 59 58 48895c2438 458b7e10'))
 here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',0xe9070a-(here+5)))
 code[0xc00:0xc00+len(w)]=w
 # s001: U3MP_MAT_GUARD=0 leaves the Material::Apply guard (kind 15) out (bisect only)
 if __import__('os').environ.get('U3MP_MAT_GUARD','1')!='0':image[site:site+9]=b'\xe9'+struct.pack('<i',at-(site+5))+bytes.fromhex('0f1f4000')
 wrappers.append(dict(site=hex(site),address=hex(at),kind='material-apply-guard',wrapper_hex=w.hex()))
# v4.81: title camera handoff. The title state's enter 0x5752a0 spawns camera `menu-camera-3`
 # (priority 0x50000000) and its exit 0x575230 kills it; on PS4 nothing leaves that state when the
 # MP lobby opens, so the lobby camera (0x5b4e60) never wins. Record the state object (state+0x26c0)
 # so the helper can run the exit when the lobby stage camera starts and the enter when LAN ends.
 # s012 (C2): the handoff is dead code. Nothing calls the exit 0x575230 any more (NATIVE_TITLE_CAM_EXIT has no caller since
 # the v5.03 NetInit level step net_init_levels takes the title levels out of the want list, as PS3 f_2901e0 does), and
 # the helper's enter call (stage_tick) only runs when state+0x26c8 is set, which nothing writes. So the record hook in the
 # title state's enter is left out by default (title state enter 1:1). U3MP_TITLE_CAM_HOOK=1 installs it again.
 site=0x5752a0
 if image[site:site+7]!=bytes.fromhex('554889e5415653'):raise ValueError('title camera enter mismatch')
 if os.environ.get('U3MP_TITLE_CAM_HOOK','0')=='1':
  at=rx+0xd00;w=bytearray()
  here=at+len(w);w.extend(b'\x48\x89\x3d'+struct.pack('<i',state+0x26c0-(here+7)))
  w.extend(bytes.fromhex('554889e5415653'))
  here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',site+7-(here+5)))
  code[0xd00:0xd00+len(w)]=w
  image[site:site+7]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x66\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind='title-camera-record',wrapper_hex=w.hex()))
# v4.85: text Draw 0xf1b7d0 asserts on a null window context. With a local Player in the match the MP
 # HUD element 0xc32ba0 (PS4 kept it, the caller 0x570db5 passes 0) reaches it; return instead of
 # trapping. c001 (agent C) KEEP, reason: no HUD text depends on it. Every HUD Text::Draw call on PS4 is gated by Bluepoint
 # on *(u32*)0x18c6280 in 2..4 and that render option is 1 (static ctor 0xe382a0, never changed), so HudNet never reaches it
 # in retail; its texts are flushed by DrawHud 0xc1a680. What can still reach Text::Draw(0) is a menu list image without
 # a texture (0x609ff0 -> 0x60afd5, see tab_source) or a debug render mode 2..4: PS3 never calls Text::Draw without a
 # context (nothing to reproduce), and PS4 would assert. U3MP_TEXT_GUARD=0 removes it (bisect only).
 site=0xf1b7fd
 if image[site:site+7]!=bytes.fromhex('488b05b46fcd00'):raise ValueError('text draw guard mismatch')
 # s001: U3MP_TEXT_GUARD=0 leaves the guard out (bisect only: Text::Draw then asserts on the HUD's null context)
 if __import__('os').environ.get('U3MP_TEXT_GUARD','1')!='0':
  image[site:site+7]=b'\xe9'+struct.pack('<i',0xf1babd-(site+5))+b'\x66\x90'
  wrappers.append(dict(site=hex(site),address=hex(site),kind='text-draw-null-context',wrapper_hex=image[site:site+7].hex()))
 # v4.86: HUD window context. PS3 f_63eaf8 draws screen 0's HUD with f_972828(hud 0x1297bb0, &sceneWindow,
 # ...), which passes ctx = *(sceneWindow+0x28) to the manager Draw (vt+0x20) and so to every element Draw
 # (HudNet f_6634c0), whose text flushes need it. PS4 0x570db5 calls the same 0x11b7a10(hud 0x1805fa0, ctx,
 # sceneWindow = r14) with ctx = 0 (xor esi,esi); 0x11b7a10 and 0x570c21 read *(sceneWindow+0x28) themselves
 # (passing only its +0x38 on), and the manager Draw 0x11b7bb0 (vtable 0x16bf040 slot 8) forwards ctx to
 # each element (vt+8) -> HudNet::Draw 0xc32ba0 -> Text::Draw 0xf1b7d0(ctx) asserts. Pass PS3's ctx.
 # v4.89: the ctx patch above (v4.86) crashed: with a real ctx the HUD glyph Draw (glyph.cpp:149) asserts
 # pVertexStream->IsMapped(); Bluepoint's HUD passes 0 on purpose. Disabled; the v4.85 text guard stays.

 site=0x72bda9
 if image[site:site+5]!=b'\xe8'+struct.pack('<i',0x997690-(site+5)):raise ValueError('loadout unload call mismatch')
 # s007: the call is PS3's own: f_310afc 0x3114c8 calls f_40de9c (= 0x997690: unwant the slot sids +0xda4/+0xdd0/+0xd4c/+0xd78,
 # names +0xe00/+0xe58 -> "", +0xf70 = 0) when param_2 != 0 and param_4 >= 1, else f_41206c + f_4120f0 (= 0x997670 / 0x996db0),
 # exactly the PS4 branch at 0x72bd98. The match kits are requested per player right after (f_308408 -> f_2faae4 = 0x742a70
 # at 0x72be24..), and PS4 never writes the +0xda4.. slot sids, so the unwant drops nothing the match needs. Default now 1:1
 # (call kept). U3MP_KEEP_LOADOUT=1 brings back the v4.76 NOP and the helper's in-match re-request (switch word state+0x20c0 bit1).
 if __import__('os').environ.get('U3MP_KEEP_LOADOUT','0')=='1':
  image[site:site+5]=bytes.fromhex('0f1f440000')
  wrappers.append(dict(site=hex(site),address=hex(site),kind='keep-loadout',wrapper_hex='0f1f440000'))
 site=0x69022d
 if image[site:site+7]!=bytes.fromhex('488b0584255601') or image[0x69025c:0x690260]!=bytes.fromhex('cd4148ff'):raise ValueError('net character kit check mismatch')
 # v5.07: non-blocking. 0x10ebaa0 = AddActorWantLoad 0x10ebb90 + LoadLevelsBlocking 0x10ebd00 (tail jump), which must not
 # run inside a match frame (no frame swap between its level updates). The stub at rx+0x940 builds the same request as
 # 0x10ebaa0(mgr, name, 0, 0) ({u64 name hash 0x10e95b0, u32 sid [0x1702720](0, name, 0), u16 0, u8 flags 1, u8 0, u64 0})
 # and calls only 0x10ebb90(mgr r14, &req); the part is skipped as before and the level manager loads it on its own.
 # rsp is 32-aligned in NetCharacter::Init 0x68fbe0; the stub's request lives below it (sub rsp,0x20), which keeps the
 # 16-byte alignment for its calls. r13 = part actor name, r14 = level manager 0x1afcf10 (0x69017a).
 at=rx+0x940
 if any(code[0x940:0x9c0]):raise ValueError('kit want stub slot in use')
 w=bytearray.fromhex('4883ec20 4c89ef')
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',0x10e95b0-(here+5)))
 w.extend(bytes.fromhex('48890424 31ff 4c89ee 31d2'))
 here=at+len(w);w.extend(b'\x48\x8d\x05'+struct.pack('<i',0x1702720-(here+7)))
 w.extend(bytes.fromhex('ff10 89442408 c744240c00000100 48c744241000000000 4c89f7 4889e6'))
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',0x10ebb90-(here+5)))
 w.extend(bytes.fromhex('4883c420'))
 here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',0x69025e-(here+5)))
 assert len(w)<=0x80
 code[0x940:0x940+len(w)]=w
 if image[0x10ebb90:0x10ebb9d]!=bytes.fromhex('554889e5415741565350498 9f6'.replace(' ','')) or image[0x10e95b0]!=0x55:raise ValueError('actor want-load mismatch')
 # s001: U3MP_KIT_WANT=0 keeps NetCharacter::Init's original assert for a kit part whose actor is not loaded
 if __import__('os').environ.get('U3MP_KIT_WANT','1')!='0':image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
 wrappers.append(dict(site=hex(site),address=hex(at),kind='kit-want-nonblocking',wrapper_hex=w.hex()))
 site=0x787b73
 if image[site:site+3]!=b'\x31\xdb\xe9' or site+7+struct.unpack_from('<i',image,site+3)[0]!=0x789a06:raise ValueError('spawn request site mismatch')
 # v4.86: PS3 spawns a *local* player as a Player (f_3609cc -> f_35fb10 -> f_584f60 = PS4 0xbe4c90) only from
 # the respawn state machine f_363ed4 state 2; f_360af8 (= 0x787a00) just stores the request and returns 0.
 # PS4 kept f_3609cc/f_35fb10 only inlined at 0x787bb8. The helper (kind 17, hooked below at 0x722e7f) runs
 # the restored f_363ed4 and, in state 2, calls 0x787a00 with the spawn-now byte at state+0x3d40 set: only
 # then this stub clears the request +0x420 (PS3 +0x3c0, re-set by the 0x787a00 prologue) and +0x7c4 (PS3
 # +0x6f0, the first store of f_3609cc) and continues at 0x787bb8. Otherwise the original local return
 # (xor ebx,ebx; jmp 0x789a06) runs, as on PS3. Replaces the v4.84 unconditional spawn.
 # v4.88: back to the v4.84 behaviour. Spawning from the restored f_363ed4 (v4.86/4.87) ran outside the
 # host's spawn answer and re-requested the map's levels; v4.87 asserted pVertexStream->IsMapped() as the
 # Player appeared. Spawn when the host answers (this call), as v4.84 did, which showed the player.
 # v4.90: both spawn triggers. (a) The restored f_363ed4 (kind 18 from the net task, PS3 f_31b458) calls
 # 0x787a00(p, spawner, 1) in its state 2 with the spawn-now byte (state+0x3d40) set: clear +0x420 and +0x7c4
 # (f_3609cc's first store) and continue at 0x787bb8 (f_3609cc -> f_35fb10). (b) The host answer (Play Task
 # 0x73ad00 / net task 0x7248a4) reaching this local return: unless mode bit 0 (state+0x3d78) asks for the
 # PS3-only trigger, spawn right away as v4.84 did (counted at state+0x3d7c). If a corpse still holds the
 # screen, the inlined f_35fb10 returns 1 without spawning (0x787c7f) and the restored f_363ed4 spawns after
 # the corpse kill. With mode bit 0 set: the original local return (request stays pending, PS3 behaviour).
 at=rx+0x900;w=bytearray()
 here=at+len(w);w.extend(b'\x80\x3d'+struct.pack('<i',state+0x3d40-(here+7))+b'\x00')
 jdo=len(w);w.extend(b'\x75\x00')
 here=at+len(w);w.extend(b'\xf6\x05'+struct.pack('<i',state+0x3d78-(here+7))+b'\x01')
 jorig=len(w);w.extend(b'\x75\x00')
 here=at+len(w);w.extend(b'\xff\x05'+struct.pack('<i',state+0x3d7c-(here+6)))
 w[jdo+1]=len(w)-(jdo+2)
 w.extend(bytes.fromhex('41c6852004000000 41c685c407000000'))
 here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',0x787bb8-(here+5)))
 w[jorig+1]=len(w)-(jorig+2)
 w.extend(bytes.fromhex('31db'))
 here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',0x789a06-(here+5)))
 assert len(w)<=0x40
 code[0x900:0x900+len(w)]=w
 image[site:site+7]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x66\x90'
 wrappers.append(dict(site=hex(site),address=hex(at),kind='local-player-spawn-gate',wrapper_hex=w.hex()))

 # v4.86: PS3 f_31ad5c (= PS4 0x722e00) ran f_363ed4 for both local net players before the 10-player
 # update loop; PS4 dropped the loop with the function. At 0x722e7f (lea r15,[0x17cb060], right where PS3
 # cleared DAT_01223f4f = PS4 mgr+0x32f after that loop) call helper kind 17 with the NetGameManager (r14),
 # then redo the lea. rsp is 16-aligned here (7 pushes after the return address).
 # v4.90: full-save hook stub (all caller-saved GPRs, x87/MMX/XMM0-15/MXCSR via fxsave64, 16-aligned stack),
 # helper(base, arg, kind, state), then the displaced instruction(s) and a jump back.
 # v5.15: the rx stub page (0x1000) is full. Offsets >= 0x10000 select slots in the x-stub area, the last XSTUB bytes of
 # the ext code segment (rx2 + EXT_CODE_SIZE - XSTUB, executable, never written by anything else): 0x10000 + n*0x80.
 def stub_slot(off):
  if off>=0x10000:
   x=off-0x10000
   if x+0x80>XSTUB:raise ValueError('x-stub slot out of range '+hex(off))
   return rx2+EXT_CODE_SIZE-XSTUB+x,xcode,x
  return rx+off,code,off
 def fullsave_stub(off,argmov,kind,replay,back):
  at,buf,o=stub_slot(off);w=bytearray.fromhex('554889e5 50515256574150415141524153 4883e4f0 4881ec00020000 480fae0424')
  here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
  w.extend(argmov)   # NOTE (v5.32): runs after rdi = image base, so an argmov of rdi (4889fe) passes the BASE, not the hooked
                     # function's rdi. Kinds 44/52 rely on that (event only has to be non-null); v5.26-v5.30's kind 51 watch
                     # therefore inspected the image base instead of the instance (now a value stub, which is correct).
  w.extend(b'\xba'+struct.pack('<I',kind))
  here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
  w.extend(bytes.fromhex('480fae0c24 488d65b8 415b415a41594158 5f5e5a5958 5d'))
  w.extend(replay(at+len(w)))
  here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',back-(here+5)))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use '+hex(off))
  buf[o:o+len(w)]=w
  return at,w
 # PS3 f_31ad5c (= PS4 0x722e00, between-round / post-match net states) ran f_363ed4 for both local players
 # (skipping a leaving one) right before clearing DAT_01223f4f (PS4 mgr+0x32f). At 0x722e7f (lea r15,
 # [0x17cb060]; next: mov byte [r15+0x32f],0) call kind 17 with the NetGameManager (r14). Jump targets
 # 0x722e19/0x722e40 land on 0x722e7f itself; nothing jumps into 0x722e80..0x722e85.
 site=0x722e7f
 if image[site:site+7]!=bytes.fromhex('4c8d3dda810a01') or image[site+7:site+15]!=bytes.fromhex('41c6872f03000000'):raise ValueError('net update hook mismatch')
 at,w=fullsave_stub(0xe00,bytes.fromhex('4c89f6'),17,lambda here:b'\x4c\x8d\x3d'+struct.pack('<i',0x17cb060-(here+7)),site+7)
 image[site:site+7]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x66\x90'
 wrappers.append(dict(site=hex(site),address=hex(at),kind=17,wrapper_hex=w.hex()))
 # PS3 f_31b458 (= PS4 net task 0x7230f0, called by the Playing update 0x67de9d and the pre-load state
 # 0x66a5a0 = PS3 f_272350) ran f_363ed4 for both local players right before the netgame+0x1a0 countdown
 # (PS4: 0x72628e mov eax,[r12+0x320]; r12 = NetGameManager). Hook there: kind 18 with r12, replay the
 # mov, back to 0x726296. 0x72628e is a jump target (0x7260bb, 0x7260cc, 0x726122); nothing jumps into
 # 0x72628f..0x726295; eax and the flags are dead there (eax is reloaded, test follows).
 site=0x72628e
 if image[site:site+8]!=bytes.fromhex('418b842420030000') or image[site+8:site+12]!=bytes.fromhex('85c07e0a'):raise ValueError('net task hook mismatch')
 at,w=fullsave_stub(0xea0,bytes.fromhex('4c89e6'),18,lambda here:bytes.fromhex('418b842420030000'),site+8)
 image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+bytes.fromhex('0f1f00')
 wrappers.append(dict(site=hex(site),address=hex(at),kind=18,wrapper_hex=w.hex()))

 # v5.11 PS3 f_310afc type-8 branch: PS4 0x72b570 wants anim-npc-multiplayer for types {4,8,9,11,13,16,17,18}
 # (bt 0x72b1) then anim-soldier-mp if r15d>=3, but dropped PS3's "netgame type == 8 -> want anim-shade-mp" in between.
 # At 0x72d28b (cmp r15d,3; jl 0x72d2ef) call kind 43, replay the cmp/jl (rel32), back to 0x72d291. No branch targets
 # 0x72d28c..0x72d290; flags are produced by the replayed cmp.
 # v5.14: value stub. Like fullsave_stub, but the helper gets rsi = the saved-register block [r11,r10,r9,r8,rdi,rsi,rdx,
 # rcx,rax] and returns 1 to make the hooked function return the (possibly rewritten) saved rax, 0 to run the original
 # (the displaced instructions, with any rewritten argument registers, then a jump back).
 def value_stub(off,kind,replay,back):
  at,buf,o=stub_slot(off);w=bytearray.fromhex('554889e5 50515256574150415141524153 4883e4f0 4881ec00020000 480fae0424')
  here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
  w.extend(bytes.fromhex('488d75b8'))                      # lea rsi,[rbp-0x48]
  w.extend(b'\xba'+struct.pack('<I',kind))
  here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
  w.extend(bytes.fromhex('480fae0c24 488d65b8 488945b8'))  # fxrstor; lea rsp,[rbp-0x48]; mov [rbp-0x48],rax (decision -> r11 slot)
  w.extend(bytes.fromhex('415b415a41594158 5f5e5a5958 5d'))
  w.extend(bytes.fromhex('4d85db 7401 c3'))               # test r11,r11; jz +1; ret
  w.extend(replay(at+len(w)))
  here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',back-(here+5)))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use '+hex(off))
  buf[o:o+len(w)]=w
  return at,w
 # v5.14 PS3 front-end prompts (helper kinds 45/46/47, see prompt_value): datasource 0x28 text 0x6341b0 (push rbp; mov
 # rbp,rsp; push r14 = 6 bytes), button 0x634340 and action 0x634370 (lea rax,[0x17b6368] = 7 bytes, rip-relative,
 # replayed with the same target). Nothing jumps into the displaced bytes (all three are function entries).
 if image[0x6341b0:0x6341b6]!=bytes.fromhex('554889e54156') or image[0x634340:0x634347]!=bytes.fromhex('488d0521201801') or image[0x634370:0x634377]!=bytes.fromhex('488d05f11f1801'):raise ValueError('prompt source mismatch')
 for site,off,kind,rep,back in ((0x6341b0,int(os.environ.get('U3MP_PS0','0xa80'),16),45,lambda h:bytes.fromhex('554889e54156'),0x6341b6),
                                (0x634340,int(os.environ.get('U3MP_PS1','0xc80'),16),46,lambda h:b'\x48\x8d\x05'+struct.pack('<i',0x17b6368-(h+7)),0x634347),
                                (0x634370,int(os.environ.get('U3MP_PS2','0xf00'),16),47,lambda h:b'\x48\x8d\x05'+struct.pack('<i',0x17b6368-(h+7)),0x634377)):
  at,w=value_stub(off,kind,rep,back)
  ln=back-site
  image[site:site+ln]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*(ln-5)
  wrappers.append(dict(site=hex(site),address=hex(at),kind=kind,wrapper_hex=w.hex()))
 # v5.15: prompt count (slot 8, 0x6342b0: push rbp; mov rbp,rsp; push rbx; push rax = 6 bytes, function entry) -> kind 48,
 # so the main/campaign menus can draw Title Menu as a third prompt (U3MP_TITLE_MENU, default on).
 if image[0x6342b0:0x6342b6]!=bytes.fromhex('554889e55350'):raise ValueError('prompt count mismatch')
 at,w=value_stub(int(os.environ.get('U3MP_PS3','0x10000'),16),48,lambda h:bytes.fromhex('554889e55350'),0x6342b6)
 image[0x6342b0:0x6342b6]=b'\xe9'+struct.pack('<i',at-(0x6342b0+5))+b'\x90'
 wrappers.append(dict(site='0x6342b0',address=hex(at),kind=48,wrapper_hex=w.hex()))
 # v5.15d: R1 Title Menu on the campaign menu (PAGE_PROFILE 0x82d5c00b). The prompt bar input (0x625b9b) only
 # sees a button after the page's focused child declined it (0x625b38: child vt+0x58, nonzero = consumed ->
 # 0x625cf3); on the campaign menu the focused row consumes R1 (button 0xa), so the drawn Title Menu prompt never
 # fires. With Title Menu on (state+0x3d79 bit 0) and that page active, R1 skips the child and reaches the prompt bar
 # (-> OnButton 0x23cf0b71 -> page 0xf0cba5f6, as on the main menu). Counter RS2[13] (state+0x3dac).
 # The 14 displaced bytes (mov rax,[rdi]; call [rax+0x58]; test al,al; jne 0x625cf3) are replayed; nothing jumps into them.
 if image[0x625b38:0x625b46]!=bytes.fromhex('488b07ff505884c00f85ad010000'):raise ValueError('prompt input site mismatch')
 at,buf,o=stub_slot(int(os.environ.get('U3MP_R1S','0x10080'),16))
 if any(buf[o:o+0x80]):raise ValueError('stub slot in use r1')
 w=bytearray()
 def rel(t,end):return struct.pack('<i',t-end)
 w+=bytes.fromhex('488b45b8 83780 80a'.replace(' ',''))   # mov rax,[rbp-0x48]; cmp dword [rax+8],0xa
 j1=len(w);w+=b'\x75\x00'                                # jne orig
 w+=b'\xf6\x05'+bytes(4)+b'\x01';fix_t=len(w)-5          # test byte [rip+state+0x3d79],1
 struct.pack_into('<i',w,fix_t,(state+0x3d79)-(at+len(w)))
 j2=len(w);w+=b'\x74\x00'                                # jz orig
 w+=b'\x48\x8d\x05'+bytes(4);struct.pack_into('<i',w,len(w)-4,0x17bd080-(at+len(w)))
 w+=bytes.fromhex('486388b00d0000 4885c9'.replace(' ',''))
 j3=len(w);w+=b'\x78\x00'
 w+=bytes.fromhex('488b84c8b0080000 4885c0'.replace(' ',''))
 j4=len(w);w+=b'\x74\x00'
 w+=bytes.fromhex('817810 0bc0d582'.replace(' ',''))
 j5=len(w);w+=b'\x75\x00'
 w+=b'\xff\x05'+bytes(4);struct.pack_into('<i',w,len(w)-4,(state+0x3dac)-(at+len(w)))
 w+=b'\xe9'+bytes(4);struct.pack_into('<i',w,len(w)-4,0x625b46-(at+len(w)))
 orig=len(w)
 for j in (j1,j2,j3,j4,j5):w[j+1]=orig-(j+2)
 w+=bytes.fromhex('488b07ff505884c0')
 w+=b'\x0f\x85'+bytes(4);struct.pack_into('<i',w,len(w)-4,0x625cf3-(at+len(w)))
 w+=b'\xe9'+bytes(4);struct.pack_into('<i',w,len(w)-4,0x625b46-(at+len(w)))
 assert len(w)<=0x80
 buf[o:o+len(w)]=w
 image[0x625b38:0x625b46]=b'\xe9'+struct.pack('<i',at-(0x625b38+5))+b'\x90'*9
 wrappers.append(dict(site='0x625b38',address=hex(at),kind='r1-campaign',wrapper_hex=w.hex()))
 # v5.16: kill-reward port (PS3 f_381ae4, see kr_port.inc). PS3's kill handler f_318d88 calls it first; PS4's kill
 # handler 0x7350c0 (rdi = NetGameManager, rsi = kill info) lost it. Kind 49 at its entry (push rbp; mov rbp,rsp;
 # push r15 = 6 bytes, replayed; the only caller is 0x714940 and nothing jumps into the displaced bytes).
 # U3MP_KR=0 leaves the hook out; U3MP_KR_NOHIST / _NOKD / _NOSTAT / _NOMEDAL=1 turn single parts off (state+0x3d79).
 if os.environ.get('U3MP_KR','1')!='0':
  if image[0x7350c0:0x7350c6]!=bytes.fromhex('554889e54157'):raise ValueError('kill handler entry mismatch')
  at,w=fullsave_stub(int(os.environ.get('U3MP_KRS','0x10100'),16),b'',49,lambda h:bytes.fromhex('554889e54157'),0x7350c6)
  image[0x7350c0:0x7350c6]=b'\xe9'+struct.pack('<i',at-(0x7350c0+5))+b'\x90'
  wrappers.append(dict(site='0x7350c0',address=hex(at),kind=49,wrapper_hex=w.hex()))
 # v5.23: mission completion (PS3 f_34b91c: "The mission '..' was accomplished. You've earned $.." / booster "Level N
 # Unlocked" HUD message, the mission cash reward f_5af2c and the completed-mission bit; see kr_port.inc). PS3's tracker
 # f_34c45c calls it as soon as progress reaches the target, before the chain/reset branch; PS4's tracker 0x773b50 lost
 # the call. At 0x77421c (mov rax,[rbp-0x1d0]; test byte [rax],1 = the chain-flag test, 10 bytes, replayed; flags come
 # from the replayed test; nothing jumps into 0x77421d..0x774225): kind 50 with rsi = tracker (r10), r8 = mission def
 # (r15). U3MP_MISSIONS=0 leaves the hook out; U3MP_MSS moves the stub slot.
 if os.environ.get('U3MP_MISSIONS','1')!='0':
  if image[0x77421c:0x774226]!=bytes.fromhex('488b8530feffff f60001'.replace(' ','')):raise ValueError('mission tracker hook mismatch')
  at,w=fullsave_stub(int(os.environ.get('U3MP_MSS','0x10200'),16),bytes.fromhex('4c89d6 4d89f8'.replace(' ','')),50,lambda h:bytes.fromhex('488b8530fefffff60001'),0x774226)
  image[0x77421c:0x774226]=b'\xe9'+struct.pack('<i',at-(0x77421c+5))+b'\x90'*5
  wrappers.append(dict(site='0x77421c',address=hex(at),kind=50,wrapper_hex=w.hex()))
 # f002 (ASSETS request after batch47): Shade process heap. PS4-ONLY ADAPTATION, NOT A PS3 PORT. The static init 0x3a46c5
 # (mov qword [0x1b559e0], imm32 = registry entry 0x1b55980 +0x60, the process heap size; rounded by 0x10e76b0, process created
 # by 0x119b250) registers the Shade NPC process with 0x1e400 bytes, the same value as PS3 1.19 (process-type registration
 # 0xa181a0, Shade at 0x10e128; the whole NPC heap table matches one for one). PS4's 64-bit structs are larger, so the same
 # budget no longer fits: NpcKnowledge::Init (vtable 0x16d14d0, 0x3b9a80) asks 32 x 0xa0 + 0x10 = 5136 bytes for contacts with
 # 3312 left -> HeapArena out of memory in Shade Survival. Raise it to 0x2d600 (1.5x); process heaps come from
 # ALLOCATION_PROCESS_HEAP (0x58, 24.5 MB). U3MP_SHADE_HEAP=0 keeps 0x1e400. Villager (0x3ab12c) and Chasee (0x38ce15) register
 # the same 0x1e400 but have shown no overflow; U3MP_NPC_HEAP_ALL=1 raises them too (opt-in, untested).
 def heap_imm(site,glob,label):
  want=b'\x48\xc7\x05'+struct.pack('<i',glob-(site+11))+struct.pack('<I',0x1e400)
  if image[site:site+11]!=want:raise ValueError(label+' heap registration mismatch')
  image[site+7:site+11]=struct.pack('<I',0x2d600)
 if os.environ.get('U3MP_SHADE_HEAP','1')!='0':heap_imm(0x3a46c5,0x1b559e0,'shade')
 if os.environ.get('U3MP_NPC_HEAP_ALL','0')=='1':heap_imm(0x3ab12c,0x1b56038,'villager');heap_imm(0x38ce15,0x1b525f8,'chasee')
 # h027 (fixer H): Co-op Adventure Airport crash = HeapArena OOM in a Soldier-family NPC process heap (run
 # baseline-1793346138171179300: "Unnamed [F:10752 T:144384]", 11184 B = 233 x 0x30 per-joint block from 0x111bb40 (anim/joint data,
 # [proc+0x198]) under Soldier::Init 0x3a48b0 -> Npc::Init 0x345a90 -> 0x10bf760). PS4-ONLY ADAPTATION like f002's Shade heap: the
 # static inits register Soldier / Pirate / EuroThug with 0x23400 and Enforcer with 0x25c00, the same values as PS3 1.19 (PS3
 # EuroThug registration 0x108674: lis 7,2; ori 7,7,0x3400 -> 0xa181a0), but PS4's 64-bit structs are larger. The co-op bosses /
 # MP-hero-skinned NPCs (big skeletons) no longer fit. Raise all four by 1.5x (as Shade): 0x23400 -> 0x34e00, 0x25c00 -> 0x38a00.
 # Heaps come from ALLOCATION_PROCESS_HEAP (0x58, 0x175c000); spawn rounds +0x60 to 128 (0x10e781e) and 0x119b250 allocates it.
 # U3MP_SOLDIER_HEAP=0 keeps the retail sizes.
 def heap_imm2(site,glob,label,old,new):
  want=b'\x48\xc7\x05'+struct.pack('<i',glob-(site+11))+struct.pack('<I',old)
  if image[site:site+11]!=want:raise ValueError(label+' heap registration mismatch')
  image[site+7:site+11]=struct.pack('<I',new)
 if os.environ.get('U3MP_SOLDIER_HEAP','1')!='0':
  heap_imm2(0x3a6f45,0x1b55a88,'soldier',0x23400,0x34e00)
  heap_imm2(0x3a1a7c,0x1b55940,'pirate',0x23400,0x34e00)
  heap_imm2(0x39b4dc,0x1b548b0,'eurothug',0x23400,0x34e00)
  heap_imm2(0x39a3e5,0x1b54810,'enforcer',0x25c00,0x38a00)
 # v5.28 Oasis splash swap (PS3 f_a00098 0xa00510..: if DAT_013b9d8d and the picked splash id is 0x8cfd9612 -> 0x24d2bfd4,
 # 0xb3c8ae25 -> 0x4c84b262; PS4 counterpart 0x128b860, same spot = the id load before the spawn at line 0x330, 0x128be4b:
 # mov rax,[r8+8]; mov edx,[rax+rcx*4] = 7 bytes, replayed; flags dead (the next 'and' sets them). Flag byte state+0x3db8 is
 # kept by the helper (oasis_tick), swaps counted at state+0x3dbc. v5.48: default ON (U3MP_OASIS_SPLASH=0 disables) since ASSETS
 # confirmed both effects (print-sand-hard-step-oasis / -left-oasis) in PS4's part-oasis-multi.bin.
 if os.environ.get('U3MP_OASIS_SPLASH','1')!='0':
  site=0x128be4b
  if image[site:site+7]!=bytes.fromhex('498b40088b1488'):raise ValueError('oasis splash site mismatch')
  at,buf,o=stub_slot(int(os.environ.get('U3MP_OSS','0x10400'),16))
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use oasis')
  back=site+7
  w=bytearray.fromhex('498b40088b1488')
  def rip(n):return lambda pos,tgt:struct.pack('<i',tgt-(at+pos+n))
  w+=b'\x80\x3d'+struct.pack('<i',(state+0x3db8)-(at+len(w)+7))+b'\x00'      # cmp byte [flag],0
  w+=b'\x74\x00';j1=len(w)-1                                                   # je back
  w+=b'\x81\xfa'+struct.pack('<I',0x8cfd9612)+b'\x75\x07'
  w+=b'\xba'+struct.pack('<I',0x24d2bfd4)+b'\xeb\x00';j2=len(w)-1           # mov edx,new; jmp cnt
  w+=b'\x81\xfa'+struct.pack('<I',0xb3c8ae25)+b'\x75\x00';j3=len(w)-1
  w+=b'\xba'+struct.pack('<I',0x4c84b262)
  cnt=len(w);w[j2]=cnt-(j2+1)
  w+=b'\xff\x05'+struct.pack('<i',(state+0x3dbc)-(at+len(w)+6))               # inc dword [count]
  bk=len(w);w[j1]=bk-(j1+1);w[j3]=bk-(j3+1)
  w+=b'\xe9'+struct.pack('<i',back-(at+len(w)+5))
  assert len(w)<=0x80
  buf[o:o+len(w)]=w
  image[site:site+7]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x66\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind='oasis-splash',wrapper_hex=w.hex()))
 # v5.43 marked Shade: PS3 Npc init (f_e7f34 0xe99e0) reads spawner property 0x22b25554 (bool, default false) into Npc
 # +0x578 bit 14, between 0xc456dde5 (bit 19) and 0x628c91b5. PS4's Npc init (0x345a90) keeps the neighbours (0xc456dde5 ->
 # +0x708 bit 44 at 0x34c390, then 0x628c91b5 at 0x34c3c1) but dropped this one (PS4 bit 49 = 63-14). At 0x34c3c1
 # (mov rax,[r14+0x40]; test rax,rax = 7 bytes, replayed; flags come from the replayed test; nothing jumps into
 # 0x34c3c2..0x34c3c7) call kind 53 with rsi = Npc (r15), r8 = spawn record (r14, +0x40 = property set).
 # U3MP_MARKED_SHADE=0 leaves it out; U3MP_MKS moves the stub slot.
 if os.environ.get('U3MP_MARKED_SHADE','1')!='0':
  if image[0x34c3c1:0x34c3c8]!=bytes.fromhex('498b46404885c0'):raise ValueError('npc init marked-shade site mismatch')
  at,w=fullsave_stub(int(os.environ.get('U3MP_MKS','0x10700'),16),bytes.fromhex('4c89fe 4d89f0'.replace(' ','')),53,lambda h:bytes.fromhex('498b46404885c0'),0x34c3c8)
  image[0x34c3c1:0x34c3c8]=b'\xe9'+struct.pack('<i',at-(0x34c3c1+5))+b'\x66\x90'
  wrappers.append(dict(site='0x34c3c1',address=hex(at),kind=53,wrapper_hex=w.hex()))
 # v5.47 cinema (U3MP_CINEMA=1): WaitReady's recording start 0x67f240 (push rbp; mov rbp,rsp; push r15 = 6 bytes, function
 # entry, replayed) -> value stub kind 54. The helper runs the parts PS4 lost (PS3 f_2b1870 film list + f_2afe68 directory
 # init, f_2b1cdc film scan, f_2b2228 slot allocation) and returns 1 (skip the recording, as the RET did) if the film
 # directory cannot be used; otherwise the original function runs and opens the film through the stream's own Open
 # (vtable 0x167f710 slot 1 = 0x6a2760). U3MP_CNS moves the stub slot.
 if CINEMA:
  if image[0x67f240:0x67f246]!=bytes.fromhex('554889e54157'):raise ValueError('film recording entry mismatch')
  at,w=value_stub(int(os.environ.get('U3MP_CNS','0x10780'),16),54,lambda h:bytes.fromhex('554889e54157'),0x67f246)
  image[0x67f240:0x67f246]=b'\xe9'+struct.pack('<i',at-(0x67f240+5))+b'\x90'
  wrappers.append(dict(site='0x67f240',address=hex(at),kind=54,wrapper_hex=w.hex()))
 # t003 (test agent): named callback 0x66e520 for "Leave Multiplayer?" Yes (0xafb7834c) = PS3 1.19 f_2818ec, which runs
 # NetShutdown f_280f68 before ShowPage(PAGE_ROOT_MAIN); PS4 lost the shutdown. Value stub kind 130 (slot 0x13800): the helper
 # shuts MP down, returns 0, and the replayed lea rdi,[0x17bd080] (7 bytes, rip-relative) + the original ShowPage tail run.
 # U3MP_LEAVE_MP=0 leaves it native.
 if os.environ.get('U3MP_LEAVE_MP','1')!='0':
  if image[0x66e520:0x66e527]!=b'\x48\x8d\x3d'+struct.pack('<i',0x17bd080-(0x66e520+7)):raise ValueError('leave-MP callback mismatch')
  at,w=value_stub(0x13800,130,lambda h:b'\x48\x8d\x3d'+struct.pack('<i',0x17bd080-(h+7)),0x66e527)
  image[0x66e520:0x66e527]=b'\xe9'+struct.pack('<i',at-(0x66e520+5))+b'\x66\x90'
  wrappers.append(dict(site='0x66e520',address=hex(at),kind=130,wrapper_hex=w.hex()))
 # ===== agent B: cinema value stubs (helper kinds 70..74, x-stub slots 0x11500..0x119ff) =====
 # kind 70: writer Open 0x6a2760 (PS3 1.19 f_2b98f0 + f_2b8ebc): push rbp; mov rbp,rsp; push r15; push r14 = 8 bytes, function entry,
 #   replayed. Record mode runs the 1.19 scan/free-slot f_2b9650 and the record-header part of f_2b8ebc that PS4 dropped.
 # kind 71: delete confirmation 0x664ba0 (button 0x7bb3c875, PS3 1.19 f_274ebc): lea rax,[rip->0x17c3ba8] = 7 bytes, rip-relative,
 #   replayed with the same target; the helper runs 1.19 f_2b959c (delete + rescan) first.
 # kind 72: PAGE_PROGRESS / PAGE_PROGRESS_CANCEL page handler 0x6648a0 (a RET followed by 15 padding bytes; PS3 1.19 f_27501c).
 # kind 73: button 0xfb637366 handler 0x665400 (RET + padding; PS3 1.19 f_274204: toggle the local/online film list, refresh).
 # kind 74 (b003): RePlaying::Update, vtable 0x167e180 slot 5 = 0x667410 'jmp 0x125c770' (PS4's stripped update: base only;
 #   PS3 1.19 f_279d6c [1.10 f_27536c]). The jmp is replayed re-encoded for the stub (runs only if the helper answers 0); the
 #   helper's port calls the base update 0x125c770 itself in PS3's order and answers 1 (the stub returns to the vtable caller).
 # Nothing jumps into the displaced bytes (all four are function entries reached through pointers/calls).
 if CINEMA:
  # x038: Cinema Protect / Unprotect handlers 0x664b80 / 0x664b90 (RET + padding; PS3 1.19 f_274f70 / f_274f38 -> f_2b7570).
  for site,off,kind in ((0x664b80,0x17100,169),(0x664b90,0x17180,170)):
   if image[site:site+7]!=bytes.fromhex('c3666666666666'):raise ValueError('protect site mismatch '+hex(site))
   at,_b,_o=stub_slot(off)
   if any(_b[_o:_o+0x80]):raise ValueError('stub slot in use '+hex(off))
   at,w=value_stub(off,kind,lambda h:b'\xc3',site+5)
   image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  # x041: Elimination match end check 0x721850. PS3 1.19 0x3194a0 runs the "only one team alive" test for type 10 only when
  # round (+0x66fc) == rounds (+0x6a44); PS4's 1.10 code runs it always, so the last kill ended the match before the round end.
  site=0x721eb0;done=0x721f0e;back=0x721eb5
  if image[site:back]!=bytes.fromhex('83f80a7459'):raise ValueError('elim end site mismatch')
  at,_b,_o=stub_slot(0x17600)
  if any(_b[_o:_o+0x80]):raise ValueError('stub slot in use 0x17600')
  w=bytearray.fromhex('83f80a')
  w.extend(b'\x0f\x85'+struct.pack('<i',back-(at+len(w)+6)))   # jne back
  w.extend(bytes.fromhex('488b45a8 8b88fc660000 3b88446a0000'))   # mov rax,[rbp-0x58]; mov ecx,[rax+0x66fc]; cmp ecx,[rax+0x6a44]
  w.extend(b'\x0f\x8c'+struct.pack('<i',back-(at+len(w)+6)))   # jl back
  w.extend(b'\xe9'+struct.pack('<i',done-(at+len(w)+5)))        # jmp done
  _b[_o:_o+len(w)]=w
  image[site:back]=b'\xe9'+struct.pack('<i',at-back)
  # x042: Elimination HUD, PS3 1.19 0x66a898..0x66ac2c: for type 10 (PS4 frame [rbp-0x11f0]) the two team numbers are the round
  # wins (team +2, not +4) and GOAL is (rounds + 1) / 2, not the score limit.
  ISTEN=bytes.fromhex('83bd10eeffff0a')
  for site,n,slot,old,body in (
    (0xc3773f,8,0x10e80,'488d9408185c0000',ISTEN+bytes.fromhex('7504 4883ea02')),
    (0xc39c33,9,0x11480,'440fbfa408185c0000',ISTEN+bytes.fromhex('7509 440fbfa408165c0000')),
    (0xc3aae4,8,0x11980,'440f48b3386a0000',ISTEN+bytes.fromhex('750d 8b83446a0000 ffc0 d1f8 4189c6'))):
   if image[site:site+n]!=bytes.fromhex(old):raise ValueError('elim hud site mismatch '+hex(site))
   at,_b,_o=stub_slot(slot)
   if any(_b[_o:_o+0x80]):raise ValueError('stub slot in use '+hex(slot))
   w=bytearray.fromhex(old)+body
   w.extend(b'\xe9'+struct.pack('<i',site+n-(at+len(w)+5)))
   _b[_o:_o+len(w)]=w
   image[site:site+n]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*(n-5)
  # x043: scoreboard cell source 0x638800 answers column 0xe6e0b346 with type 0x29 (level), which PS4's item draw has no case for.
  # At its common return (out struct = r13) kind 171 turns that answer into the rank texture.
  site=0x63a173
  if image[site:site+7]!=bytes.fromhex('4c89e84883c418'):raise ValueError('rank cell site mismatch')
  at,w=fullsave_stub(0x12800,bytes.fromhex('4c89ee'),171,lambda h:bytes.fromhex('4c89e84883c418'),site+7)
  image[site:site+7]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x66\x90'
  # x044: Elimination HUD, second layout: both team numbers there also read team +4; type 10 reads +2 (PS3 0x66ab0c / 0x66ac2c).
  ISTEN4=bytes.fromhex('83bd10eeffff0a')
  for site,n,slot,old,body in (
    (0xc3add9,8,0x12880,'488db401185c0000',ISTEN4+bytes.fromhex('7504 4883ee02')),
    (0xc3af36,9,0x12900,'440fbfbc01185c0000',ISTEN4+bytes.fromhex('7509 440fbfbc01165c0000'))):
   if image[site:site+n]!=bytes.fromhex(old):raise ValueError('elim hud2 site mismatch '+hex(site))
   at,_b,_o=stub_slot(slot)
   if any(_b[_o:_o+0x80]):raise ValueError('stub slot in use '+hex(slot))
   w=bytearray.fromhex(old)+body
   w.extend(bytes([0xe9])+struct.pack('<i',site+n-(at+len(w)+5)))
   _b[_o:_o+len(w)]=w
   image[site:site+n]=bytes([0xe9])+struct.pack('<i',at-(site+5))+bytes([0x90])*(n-5)
  # x046: player level 0x7274f4: member data +0xe + player +0x1e8 picked the second account's level (0) for a guest. Second account is
  # removed, so always read +0xe.
  if image[0x7274f4:0x7274fb]!=bytes.fromhex('418b8ee8010000'):raise ValueError('level index site mismatch')
  image[0x7274f4:0x7274fb]=bytes.fromhex('31c90000000000')[:2]+bytes([0x90])*5
  # x049: fixed loadout path 0xbbc681: for a non co-op game go straight to 0xbbcb28 (no 0x556e3fb5 co-op mods); the path now also serves Classic.
  site=0xbbc67e
  if image[site:site+5]!=bytes.fromhex('0fa3c1730e'):raise ValueError('coop mods site mismatch')
  at,_b,_o=stub_slot(0x12980)
  if any(_b[_o:_o+0x80]):raise ValueError('stub slot in use 0x12980')
  w=bytearray.fromhex('0fa3c1')
  w.extend(bytes([0x0f,0x83])+struct.pack('<i',0xbbcb28-(at+len(w)+6)))
  w.extend(bytes([0xe9])+struct.pack('<i',0xbbc683-(at+len(w)+5)))
  _b[_o:_o+len(w)]=w
  image[site:site+5]=bytes([0xe9])+struct.pack('<i',at-(site+5))
  # x050: x049 missed types above the range check: 0xbbc677 'ja 0xbbc691' let Classic (type 0x13) read the co-op mods. Out of range also goes to 0xbbcb28.
  site=0xbbc674
  if image[site:site+5]!=bytes.fromhex('83f80e7718'):raise ValueError('coop mods range site mismatch')
  at,_b,_o=stub_slot(0x12a00)
  if any(_b[_o:_o+0x80]):raise ValueError('stub slot in use 0x12a00')
  w=bytearray.fromhex('83f80e')
  w.extend(bytes([0x0f,0x87])+struct.pack('<i',0xbbcb28-(at+len(w)+6)))
  w.extend(bytes([0xe9])+struct.pack('<i',0xbbc679-(at+len(w)+5)))
  _b[_o:_o+len(w)]=w
  image[site:site+5]=bytes([0xe9])+struct.pack('<i',at-(site+5))
  # x051: Classic keeps boosters. Kickback and weapon mod tests read netgame +0x68 (new data copy of +0x6c); Classic has +0x6c = 0, +0x68 = 1.
  for site in (0x6e7f71,0x6e7ff8,0x6e80b2,0x6e8139,0x725c74,0x74e90d,0x74ed9b,0x74f2b8,0xc3d1c0,0xbbd2e8):
   if image[site:site+5]!=bytes.fromhex('837c086c00'):raise ValueError('netgame flag site mismatch '+hex(site))
   image[site+3]=0x68
  # x054: no builder change, helper only
  # x056..x062: helper only
  # x063: helper only
  # x064: helper only
  # x065: helper only
  # x066,x067: helper only
  # x068: helper only
  # x068: helper only
  # x068: helper only
  # x068: helper only
  # x076: treasure preview camera z -0.65 -> -1.0, model x -0.1 -> -0.154 (same screen x); no per-model fit exists on PS3 or PS4
  for va,old,new in ((0x1401088,'666626bf','000080bf'),(0x1443a58,'cdccccbd','9ab21dbe'),(0x1443a70,'cdccccbd','9ab21dbe')):
   if image[va:va+4]!=bytes.fromhex(old):raise ValueError('treasure camera mismatch')
   image[va:va+4]=bytes.fromhex(new)
  # x078: mission progress toast time, PS3 0x359154: f_7aba74(..,0,seconds); PS4 0x774b82 passes 0 seconds. Value stub sets r8d.
  if image[0x774b82:0x774b88]!=bytes.fromhex('4531c04889df'):raise ValueError('mission toast mismatch')
  at,w=value_stub(0x12d80,172,lambda h:bytes.fromhex('4889df'),0x774b88)
  image[0x774b82:0x774b88]=bytes([0xe9])+struct.pack('<i',at-(0x774b82+5))+bytes([0x90])
  wrappers.append(dict(site='0x774b82',address=hex(at),kind=172,wrapper_hex=w.hex()))
  # x080: Medals: PS4 0x5cc6e3 = PS3 0x240520 stops after the description on the LAN byte; the PS3 reference shows Earned and Value
  if image[0x5cc6ee:0x5cc6f4]!=bytes.fromhex('0f85f2c00000'):raise ValueError('medal gate mismatch')
  image[0x5cc6ee:0x5cc6f4]=bytes([0x90])*6
  # x085: x083 stub removed: a value stub overwrites r11, which the consumer at 0xe4b130 still uses
  # x086: helper only
  # x087: helper only
  # x088: helper only
  # x089: helper and data only
  # x090: helper only
  # x093: stat lines are in the agc block
  # x095: helper changes
  # x100: see agc block
  for site,off,kind,old,rep,ln in ((0x6a2760,0x11500,70,'554889e541574156',lambda h:bytes.fromhex('554889e541574156'),8),
                                   (0x664ba0,0x11580,71,'488d0501f01501',lambda h:b'\x48\x8d\x05'+struct.pack('<i',0x17c3ba8-(h+7)),7),
                                   (0x6648a0,0x11600,72,'c3666666666666',lambda h:b'\xc3',5),
                                   (0x665400,0x11680,73,'c3666666666666',lambda h:b'\xc3',5),
                                   (0x667410,0x11700,74,'e95b53bf0066',lambda h:b'\xe9'+struct.pack('<i',0x125c770-(h+5)),5)):
   if image[site:site+len(old)//2]!=bytes.fromhex(old):raise ValueError('agent B cinema site mismatch '+hex(site))
   at,w=value_stub(off,kind,rep,site+ln)
   image[site:site+ln]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*(ln-5)
   wrappers.append(dict(site=hex(site),address=hex(at),kind=kind,wrapper_hex=w.hex()))
  # b004 (user's no-dead-options rule): the Screenshot hot key/prompt cannot work on PS4 (no capture object, no
  # cellPhotoUtility; see b003 notes), so it is not offered. PS3 1.19 f_6651a8 [1.10 f_65a4e8] adds it as entry 0 of the
  # HUD hot-key list ({msg 0xc1f65aeb, button 0x44e0b0fa, text 0x4db8b2d3}, count +0x15c = 1) when f_449a8 [f_44718]
  # allows; PS4 0xc3dd5d replaces that check by title-id compares (BCET70043 / NPUA70180 / NPHA80158) and two state ids, and
  # skips the entry at 0xc3de58 with ecx = 0. The first compare's 'je 0xc3de58' (0xc3dd79, 6 bytes) becomes 'nop; jmp',
  # so the list starts empty exactly as on those builds. U3MP_CINEMA_HIDE=0 keeps it native (with the b002 buttons).
  if os.environ.get('U3MP_CINEMA_HIDE','1')!='0':
   if image[0xc3dd75:0xc3dd7f]!=bytes.fromhex('31c985c00f84d9000000'):raise ValueError('agent B screenshot prompt site mismatch')
   image[0xc3dd79:0xc3dd7f]=b'\x90\xe9'+struct.pack('<i',0xc3de58-(0xc3dd7a+5))
 # b012: the two PS3 1.19 time-sync reset calls (f_280d28) PS4 lost, as value stubs (the helper resets the transport's sync
 # state and answers 0, so the displaced instruction always runs):
 # kind 75: 0x6a6690 = PS3 f_2bc124 entry (push rbp; mov rbp,rsp; push r14 = 6 bytes, function entry, replayed).
 # kind 76: 0x6a6e68 in 0x6a6900 = the PS3 f_2bd584 state-0xf case after f_2fc150(0) / (1) (PS4 0x6b7b30 x2): mov dword
 #   [rip->0x1b68810],0x12 = 10 bytes, rip-relative, replayed re-encoded with the same target. No branch lands inside either.
 for site,off,kind,old,rep,ln in ((0x6a6690,0x11780,75,'554889e54156',lambda h:bytes.fromhex('554889e54156'),6),
                                  (0x6a6e68,0x11800,76,'c7059e194c0112000000',lambda h:b'\xc7\x05'+struct.pack('<i',0x1b68810-(h+10))+struct.pack('<I',0x12),10)):
  if image[site:site+len(old)//2]!=bytes.fromhex(old):raise ValueError('agent B b012 site mismatch '+hex(site))
  at,w=value_stub(off,kind,rep,site+ln)
  image[site:site+ln]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*(ln-5)
  wrappers.append(dict(site=hex(site),address=hex(at),kind=kind,wrapper_hex=w.hex()))
 # v7.91 rf25: kind 236 at 0x6d85b4 = 'call 0x739070' in the round start event Execute 0x6d8480 (5 bytes, nothing jumps
 # inside; replayed re-encoded). The helper may rewrite the start time rdx points at (a guest's clock behind the host's).
 site=0x6d85b4
 if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=0x739070:raise ValueError('rf25 round start site mismatch')
 at,w=value_stub(0x15780,236,lambda h:b'\xe8'+struct.pack('<i',0x739070-(h+5)),site+5)
 image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
 wrappers.append(dict(site=hex(site),address=hex(at),kind=236,wrapper_hex=w.hex()))
 # b014: U3MP_ONLINE_MM=1: the named callback 0x66e500 ("Play Online", PS3 1.19 f_296628) goes to helper kind 90 (the PS3
 # online NetInit path, agent B onb_play_online) instead of the frozen shell's jump into the LAN lobby (0x2050100). The same
 # 5-byte site the s003 opt-in U3MP_PLAY_ONLINE_PS3=1 used (b015: that switch is removed, b014 supersedes it).
 # agent B b044: also without U3MP_ONLINE_MM (offline mode: the shell's jump showed PAGE_MULTIPLAYER without NetInit, so
 #   every lobby source was empty). U3MP_ONLINE_OFFLINE=0 without U3MP_ONLINE_MM keeps the shell jump.
 if os.environ.get('U3MP_ONLINE_MM','0')=='1' or os.environ.get('U3MP_ONLINE_OFFLINE','1')!='0':
  site=0x66e500
  if image[site]!=0xe9 or site+5+struct.unpack_from('<i',image,site+1)[0]!=0x2050100:raise ValueError('agent B b014 play online site mismatch')
  at,buf,o=stub_slot(0x11880)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x11880')
  w=bytearray.fromhex('4883ec18 48893c24 4889fe')
  here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
  w.extend(b'\xba'+struct.pack('<I',90))
  here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
  w.extend(bytes.fromhex('4883c418c3'))
  buf[o:o+len(w)]=w
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=90,original='e9->0x2050100',wrapper_hex=w.hex()))
 # b019: kind 77 at 0x571235 = PS4's only call of the menu draw 0x656e30 (= PS3 f_26f874) in the frame: 'call 0x656e30'
 #   (5 bytes, replayed re-encoded, back to 0x57123a). The helper runs PS3 1.19 f_64efb8's f_376b14 + f_3768cc pair (round
 #   screens: Select scoreboard, results) first and answers 0. rdi/rsi are preserved by the stub. U3MP_RESULTS=0 leaves it out.
 if os.environ.get('U3MP_RESULTS','1')!='0':
  site=0x571235
  if image[site:site+5]!=bytes.fromhex('e8f65b0e00'):raise ValueError('agent B b019 site mismatch')
  at,w=value_stub(0x11900,77,lambda h:b'\xe8'+struct.pack('<i',0x656e30-(h+5)),site+5)
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=77,wrapper_hex=w.hex()))
 # agent B b021: kind 78 at 0x1133c6c = the packfile load 'call 0xdfcd0' (5 bytes) in the level add 0x1133bf0 (PS3 f_8f64d8).
 #   0xdfcd0 returns 0 for a blob it already loaded (header +0x38 bit 0), so a level still listed when the next physics world is
 #   built (world init 0x11509d0 re-adds the whole list) gets contents 0 and 0x122540 faults at 0x122551 (Start Cinema).
 #   PS3 builds a fresh mesh shape from resident data on every add (f_8f5c38). The helper answers 1 with the saved rax = the
 #   contents address of the already-loaded blob, and the stub then goes on at 0x1133c71 (mov r13,rax) without the call;
 #   answer 0 replays the call (first load). r11 is dead at 0x1133c71 (the replayed call clobbers it; next use 0x1133c8a
 #   loads it). Nothing jumps into 0x1133c6d..0x1133c70. U3MP_HK_REUSE=0 leaves it out.
 if os.environ.get('U3MP_HK_REUSE','1')!='0':
  site=0x1133c6c
  if image[site:site+5]!=b'\xe8'+struct.pack('<i',0xdfcd0-(site+5)):raise ValueError('agent B b021 site mismatch')
  at,buf,o=stub_slot(0x15000);w=bytearray.fromhex('554889e5 50515256574150415141524153 4883e4f0 4881ec00020000 480fae0424')
  here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
  w.extend(bytes.fromhex('488d75b8'))                      # lea rsi,[rbp-0x48] (saved r11,r10,r9,r8,rdi,rsi,rdx,rcx,rax)
  w.extend(b'\xba'+struct.pack('<I',78))
  here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
  w.extend(bytes.fromhex('480fae0c24 488d65b8 488945b8'))  # fxrstor; lea rsp,[rbp-0x48]; decision -> r11 slot
  w.extend(bytes.fromhex('415b415a41594158 5f5e5a5958 5d'))
  w.extend(bytes.fromhex('4d85db 7405'))                   # test r11,r11; jz replay
  here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',(site+5)-(here+5)))   # answer 1: rax = saved rax (the contents), back
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',0xdfcd0-(here+5)))    # answer 0: the original load
  here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',(site+5)-(here+5)))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15000')
  buf[o:o+len(w)]=w
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=78,wrapper_hex=w.hex()))
 # agent B b022: kind 79 at 0x66699f = the cinema init's 'call 0xd9f400' (PushSysModuleConfig("multi-player-cinema"), 5 bytes,
 #   replayed re-encoded, back to 0x6669a4). The helper registers the config PS4 never registers (PS3 1.19 does, 0x59528) and
 #   answers 0. rdi (the name) is preserved by the stub. U3MP_CINEMA_CONFIG=0 leaves it out.
 if os.environ.get('U3MP_CINEMA_CONFIG','1')!='0':
  site=0x66699f
  if image[site:site+5]!=b'\xe8'+struct.pack('<i',0xd9f400-(site+5)):raise ValueError('agent B b022 site mismatch')
  at,w=value_stub(0x15080,79,lambda h:b'\xe8'+struct.pack('<i',0xd9f400-(h+5)),site+5)
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=79,wrapper_hex=w.hex()))
 # agent B b023: kind 160 at 0xc35346 in the HUD draw 0xc32ba0: 'cmp qword [rax+0x20],0; jne 0xc35388; int 0x41; int 0x41; jmp
 #   0xc35388' (13 bytes, 0xc35346..0xc35352) -> jmp to a register-saving stub (rsi = saved {r15,r14,r13,r12,rbx,r11,r10,r9,r8,
 #   rdi,rsi,rdx,rcx,rax}; r12 = the HUD), which calls the helper (PS3 1.19 f_664c7c on hud+0x2408 when *(0x17b4a48+0x20) == 0)
 #   and always continues at 0xc35388 (the PS4 code after the trap). Nothing jumps into 0xc35347..0xc35352 (targets 0xc35353
 #   and 0xc35388 only); all registers are restored, flags are dead at 0xc35388 (test ebx). U3MP_CIN_HUD=0 leaves it out.
 #   eb[1] bit0 U3MP_CIN_HUD_SKIP=1 (set with the eb[0] switches): stub kept, element skipped.
 if os.environ.get('U3MP_CIN_HUD','1')!='0':
  site=0xc35346
  if image[site:site+13]!=bytes.fromhex('4883782000753bcd41cd41eb35'):raise ValueError('agent B b023 site mismatch')
  at,buf,o=stub_slot(0x15100)
  w=bytearray.fromhex('554889e5 50515256574150415141524153 53 4154 4155 4156 4157 488d7590 4883e4f0 4881ec00020000 480fae0424')
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=b'\xba'+struct.pack('<I',160)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d6590 415f415e415d415c5b 415b415a41594158 5f5e5a5958 5d')
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0xc35388-(here+5))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15100')
  buf[o:o+len(w)]=w
  image[site:site+13]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*8
  wrappers.append(dict(site=hex(site),address=hex(at),kind=160,wrapper_hex=w.hex()))
 # agent B b024: kind 161 at 0xc37a8f in the HUD draw: 'mov rdi,r15; call 0x745240; int 0x41' (10 bytes, 0xc37a8f..0xc37a98) ->
 #   jmp to a register-saving stub (rbx = the DC HUD object, original rbp at saved[14]) that calls the helper (PS3 1.19 ring
 #   f_667368 at 0x66b194, which calls 0x745240 itself) and continues at 0xc37a99. Nothing jumps into 0xc37a90..0xc37a98.
 #   U3MP_HUD_RING_X=0 leaves it out.
 if os.environ.get('U3MP_HUD_RING_X','1')!='0':
  site=0xc37a8f
  if image[site:site+10]!=b'\x4c\x89\xff\xe8'+struct.pack('<i',0x745240-(site+8))+b'\xcd\x41':raise ValueError('agent B b024 site mismatch')
  at,buf,o=stub_slot(0x15180)
  w=bytearray.fromhex('554889e5 50515256574150415141524153 53 4154 4155 4156 4157 488d7590 4883e4f0 4881ec00020000 480fae0424')
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=b'\xba'+struct.pack('<I',161)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d6590 415f415e415d415c5b 415b415a41594158 5f5e5a5958 5d')
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0xc37a99-(here+5))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15180')
  buf[o:o+len(w)]=w
  image[site:site+10]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*5
  wrappers.append(dict(site=hex(site),address=hex(at),kind=161,wrapper_hex=w.hex()))
 # agent B b025: kind 162 at 0xe41d19 in the UI packet executor 0xe41a60: 'mov [rsp+0x18],rax' (5 bytes; the 3-entry texture array
 #   is complete, rax = its address rsp+0xe0 when handle 1 was valid) -> register-saving stub: the helper fills array entries
 #   that have no handle with the material's own default texture, then the mov is replayed and execution goes on at 0xe41d1e.
 #   0xe41d19 is a jump target (0xe41c72/7e/8c, 0xe41cbe/ef, 0xe41d0c): the jmp sits at the target, nothing jumps into
 #   0xe41d1a..0xe41d1d. U3MP_UI_MATDEF=0 leaves it out.
 if os.environ.get('U3MP_UI_MATDEF','0')=='1':   # main: default OFF (TEST2: b025 brings back the Profile>Emblem fault at 0xe66b0d)
  site=0xe41d19
  if image[site:site+5]!=bytes.fromhex('4889442418'):raise ValueError('agent B b025 site mismatch')
  at,buf,o=stub_slot(0x15200)
  w=bytearray.fromhex('554889e5 50515256574150415141524153 53 4154 4155 4156 4157 488d7590 4883e4f0 4881ec00020000 480fae0424')
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=b'\xba'+struct.pack('<I',162)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d6590 415f415e415d415c5b 415b415a41594158 5f5e5a5958 5d')
  w+=bytes.fromhex('4889442418')
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0xe41d1e-(here+5))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15200')
  buf[o:o+len(w)]=w
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=162,wrapper_hex=w.hex()))
 # agent B b027: kind 163 at 0x69a55e in the player weapon state receiver 0x69a4c0 (= PS3 1.19 f_2b1970): the 1.10-style
 #   GetWeaponIdByStatIndex inline 0x69a55e..0x69a60b ('mov edi,0x9e354807', DC lookup, two asserts, 19-entry table) is
 #   skipped: the register-saving stub runs the helper (1.19 f_3053e8: 20 entries, DC at index-20; rbx = stat index in,
 #   rbx = cell holding the id and r15 = r12 out) and goes on at 0x69a60b ('mov r12d,[rbx]'). Nothing outside the range
 #   jumps into 0x69a55f..0x69a60a (the loop head 0x69a550 is before the site). U3MP_WSTAT119=0 leaves it out.
 if os.environ.get('U3MP_WSTAT119','1')!='0':
  site=0x69a55e
  if image[site:site+10]!=bytes.fromhex('bf0748359ee8c888b500'):raise ValueError('agent B b027 site mismatch')
  at,buf,o=stub_slot(0x15280)
  w=bytearray.fromhex('554889e5 50515256574150415141524153 53 4154 4155 4156 4157 488d7590 4883e4f0 4881ec00020000 480fae0424')
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=b'\xba'+struct.pack('<I',163)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d6590 415f415e415d415c5b 415b415a41594158 5f5e5a5958 5d')
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x69a60b-(here+5))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15280')
  buf[o:o+len(w)]=w
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=163,wrapper_hex=w.hex()))
 # agent B b030: NetEvent 0x39 apply = slot 4 of vtable 0x16d4240 (0x6e1b20; PS3 1.19 0x2f655c, vtable 0x10f6698). Both make
 #   the apply call (PS4 0x6ae7a0 at 0x6e1ea9 = PS3 f_2f6274 at 0x2f67bc); PS3 then sets the target character's flag
 #   (ld 9,1400(28); or 1<<47; std: 0x2f67c4..0x2f67d4) and returns, PS4 has int 0x41 (0x6e1eae). PS4 keeps that flag set
 #   after the same apply call elsewhere: 0x360b6c and 0x37ccb5 'or byte [r14+0x70a],1' right after 'call 0x6ae7a0' (PS3
 #   0xdbe3c / 0xe8c0c / 0x37e770 do 'or 1400 bit 47' after f_2f6274), so PS3 +1400 bit 47 (the dword's 0x8000<<32) = PS4
 #   byte +0x70a bit 0. Target = PS3 r28 (char of event+20) = PS4 r15 (callee-saved, the 3rd apply argument).
 #   Site 0x6e1ea9 'call 0x6ae7a0; int 0x41' (7 bytes) -> stub: same call, 'or byte [r15+0x70a],1', inc eb[247], jmp 0x6e1eb0
 #   (the epilogue the trap fell into; nothing jumps into 0x6e1eaa..0x6e1eaf). U3MP_NE39_FLAG=0 leaves it out.
 if os.environ.get('U3MP_NE39_FLAG','1')!='0':
  site=0x6e1ea9
  if image[site:site+7]!=bytes.fromhex('e8f2c8fcffcd41'):raise ValueError('agent B b030 site mismatch')
  at,buf,o=stub_slot(0x15300)
  w=bytearray(b'\xe8'+struct.pack('<i',0x6ae7a0-(at+5)))
  w+=bytes.fromhex('41808f0a07000001')
  here=at+len(w);w+=b'\xff\x05'+struct.pack('<i',(rw2+0x4050+4*247)-(here+6))
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x6e1eb0-(here+5))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15300')
  buf[o:o+len(w)]=w
  image[site:site+7]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind='b030-ne39-flag',wrapper_hex=w.hex()))
 # agent B b031: NetEvent 0x16 apply = slot 4 of vtable 0x16d5af0 (0x714060; PS3 1.19 0x2f92ac, vtable 0x10f6038). When the
 #   current net level (f_30e004 / DC 0x3b9a067d record [mgr+0x661c]) is not type 11 and its record has +188 and +192
 #   (PS4 +0xc0 / +0xc8) set, PS3 0x2f9884..0x2f98ec looks up the event's entity (f_308724(mgr, ev+28) = PS4 FNV hash of
 #   ev+0x20 in mgr+0x6520) and goes on to the local-player loop 0x2f98f0 only if: the event's player (r24 = f_3097d0(ev+24)
 #   when ev+16 bit 0; PS4 r12, set at 0x7141bf) is non-null, the handle {obj*, id} (PS4 node+0x18) is valid (obj+80 ==
 #   id; PS4 +0x7c) and the character's +1400 LSB bit 15 is clear (rldicl 49,63). Otherwise 0x2f9abc (PS4 0x71479e).
 #   PS4 0x714313..0x7143eb keeps the record tests and the hash walk, then has int 0x41 (0x7143ed, the walk's exit) and the
 #   loop (0x7143ef 'mov r12,[r15+0x370]' = PS3 0x2f98f0..). +1400 LSB bit 15 = PS4 byte +0x70e bit 0 (char event
 #   0x36a9bfa7: PS3 0xebd88 sets LSB 15, PS4 0x35eb5b sets +0x70e bit 0; rule LSB k -> 63-k, as kr_port).
 #   Site 0x7143ed 'int 0x41; mov r12,[r15+0x370]' (9 bytes; 0x7143ed is the walk's exit target, nothing jumps into
 #   0x7143ee..0x7143f5) -> stub with those checks (rax = node or 0 at the exit, r15 = mgr), then the mov and jmp 0x7143f6,
 #   or jmp 0x71479e. eb[248] passed, eb[249] skipped. U3MP_NE16_ENT=0 leaves it out.
 if os.environ.get('U3MP_NE16_ENT','1')!='0':
  site=0x7143ed
  if image[site:site+9]!=bytes.fromhex('cd414d8ba770030000'):raise ValueError('agent B b031 site mismatch')
  at,buf,o=stub_slot(0x15380)
  w=bytearray()
  js=[]
  def jz_skip(op):                    # op = 2-byte jcc rel32 opcode
   js.append(len(w));w.extend(op+b'\0\0\0\0')
  w+=bytes.fromhex('4d85e4');jz_skip(b'\x0f\x84')          # test r12,r12; jz skip
  w+=bytes.fromhex('4885c0');jz_skip(b'\x0f\x84')          # test rax,rax; jz skip
  w+=bytes.fromhex('488b5018 4885d2'.replace(' ',''));jz_skip(b'\x0f\x84')   # mov rdx,[rax+0x18]; test rdx,rdx
  w+=bytes.fromhex('8b7020 85f6'.replace(' ',''));jz_skip(b'\x0f\x84')       # mov esi,[rax+0x20]; test esi,esi
  w+=bytes.fromhex('488b12 4885d2'.replace(' ',''));jz_skip(b'\x0f\x84')     # mov rdx,[rdx]; test rdx,rdx
  w+=bytes.fromhex('39727c');jz_skip(b'\x0f\x85')          # cmp [rdx+0x7c],esi; jne skip
  w+=bytes.fromhex('f6820e07000001');jz_skip(b'\x0f\x85')  # test byte [rdx+0x70e],1; jnz skip
  here=at+len(w);w+=b'\xff\x05'+struct.pack('<i',(rw2+0x4050+4*248)-(here+6))
  w+=bytes.fromhex('4d8ba770030000')                         # mov r12,[r15+0x370]
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x7143f6-(here+5))
  skip=len(w)
  for p in js:w[p+2:p+6]=struct.pack('<i',skip-(p+6))
  here=at+len(w);w+=b'\xff\x05'+struct.pack('<i',(rw2+0x4050+4*249)-(here+6))
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x71479e-(here+5))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15380')
  buf[o:o+len(w)]=w
  image[site:site+9]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*4
  wrappers.append(dict(site=hex(site),address=hex(at),kind='b031-ne16-ent',wrapper_hex=w.hex()))
 # agent B b032: kind 164 at 0xc3106c in the HUD-net element draw 0xc2fd10 (PS3 1.19 f_672400): 'cmp [r12+0x50],3; je 0xc31076;
 #   int 0x41' (10 bytes) -> register-saving stub: the helper draws PS3's progress ring (f_667368, 0x673774..0x6737e0) for
 #   modes 1/2/5 (mode 3 skips it as PS3), then execution goes on at 0xc31076. Nothing outside jumps into 0xc3106d..0xc31075
 #   (0xc31076 is the je's own target). U3MP_HUDNET_RING=0 leaves it out.
 if os.environ.get('U3MP_HUDNET_RING','1')!='0':
  site=0xc3106c
  if image[site:site+10]!=bytes.fromhex('41837c2450037402cd41'):raise ValueError('agent B b032 site mismatch')
  at,buf,o=stub_slot(0x15400)
  w=bytearray.fromhex('554889e5 50515256574150415141524153 53 4154 4155 4156 4157 488d7590 4883e4f0 4881ec00020000 480fae0424')
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=b'\xba'+struct.pack('<I',164)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d6590 415f415e415d415c5b 415b415a41594158 5f5e5a5958 5d')
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0xc31076-(here+5))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15400')
  buf[o:o+len(w)]=w
  image[site:site+10]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*5
  wrappers.append(dict(site=hex(site),address=hex(at),kind=164,wrapper_hex=w.hex()))
 # agent B b035: NetCharacter::Init (0x68fbe0) second part loop (6 accessory parts, look indices 5..10 through the slot table
 #   0x13a68a0) traps at 0x69037b when the part's actor package (0x10eb380(0x1afcf10, sid)) is missing or not in state 8. This is
 #   1:1 with PS3 1.19 f_2b0??? 0x2b0760..0x2b087c: f_436db4 name, strcpy, f_436a6c item, then f_45f24(0x1352e20, name) true ->
 #   twu at 0x2b086c (the first loop's twu 0x2b0740 is PS4 0x69025c). PS3 only avoids it because the look's packages are
 #   resident by then (f_310b98 -> f_303c6c wants parts 0..4 and 5..10; PS4 keeps that want: 0x742a70, accessory loop
 #   0x743040..0x74310c). Same workaround as the 0x69025c fix (v4.69 / v5.07): the not-loaded path at 0x690348
 #   ('mov rax,[0x1bf27b8]', 7 bytes; only the je at 0x690340 lands there) -> x-stub 0x15480 that builds 0x10ebaa0's request
 #   {u64 hash 0x10e95b0(name), u32 sid [0x1702720](0, name, 0), u16 0, u8 flags 1, u8 0, u64 0} below rsp (rsp is 32-aligned
 #   in this frame, see the kit stub), calls only AddActorWantLoad 0x10ebb90(0x1afcf10, &req) (non-blocking) and skips the part
 #   (jmp 0x69037d, the loop increment). rbx = part actor name here; r12 / r14 / r15 and the stack slots the loop reads are kept.
 #   eb[166] parts skipped, eb[167] sid of the last skipped part. U3MP_ACC_WANT=0 leaves the trap.
 if os.environ.get('U3MP_ACC_WANT','1')!='0':
  site=0x690348
  if image[site:site+7]!=bytes.fromhex('488b0569245601') or image[0x69037b:0x69037f]!=bytes.fromhex('cd4149ff'):raise ValueError('agent B b035 site mismatch')
  if image[0x690340:0x690348]!=bytes.fromhex('7406837830087435'):raise ValueError('agent B b035 guard mismatch')
  at,buf,o=stub_slot(0x15480)
  w=bytearray.fromhex('4883ec20 4889df'.replace(' ',''))                    # sub rsp,0x20; mov rdi,rbx
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',0x10e95b0-(here+5))
  w+=bytes.fromhex('48890424 31ff 4889de 31d2'.replace(' ',''))            # mov [rsp],rax; xor edi,edi; mov rsi,rbx; xor edx,edx
  here=at+len(w);w+=b'\x48\x8d\x05'+struct.pack('<i',0x1702720-(here+7))
  w+=bytes.fromhex('ff10 89442408'.replace(' ',''))                         # call [rax]; mov [rsp+8],eax
  here=at+len(w);w+=b'\x89\x05'+struct.pack('<i',(rw2+0x4050+4*167)-(here+6))   # eb[167] = sid
  here=at+len(w);w+=b'\xff\x05'+struct.pack('<i',(rw2+0x4050+4*166)-(here+6))   # eb[166]++
  w+=bytes.fromhex('c744240c00000100 48c744241000000000'.replace(' ',''))  # +0xc: u16 0, flags 1, 0; +0x10: 0
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',0x1afcf10-(here+7))
  w+=bytes.fromhex('4889e6')                                                # mov rsi,rsp
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',0x10ebb90-(here+5))
  w+=bytes.fromhex('4883c420')
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x69037d-(here+5))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15480')
  buf[o:o+len(w)]=w
  image[site:site+7]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*2
  wrappers.append(dict(site=hex(site),address=hex(at),kind='b035-acc-want-nonblocking',wrapper_hex=w.hex()))
 # agent B b040 (diagnostics): where the film HUD tab actions go. 0x64ce90 (menu action, = PS3 f_269c70) looks the id up in the
 #   action map 0x1dd26a0 and switches on the kind (0..5, else the callback list menu+0x155c): stub A at 0x64cff0 ('mov eax,r14d;
 #   cmp r14d,5', 7 bytes, the flags reach the ja at 0x64cff7 unchanged) stores the kind in eb[127]. Stub B at the cinema event
 #   handler 0x667d50 (vtable 0x167e180 slot 2, = PS3 f_2791b4; 'push rbp; mov rbp,rsp; push r15', 6 bytes, function entry)
 #   counts calls in eb[95] and stores the event id [rsi] in eb[126]. U3MP_CIN_TRACE=0 leaves both out.
 if os.environ.get('U3MP_CIN_TRACE','1')!='0':
  site=0x64cff0
  if image[site:site+7]!=bytes.fromhex('4489f04183fe05'):raise ValueError('agent B b040 site A mismatch')
  at,buf,o=stub_slot(0x15500)
  w=bytearray()
  here=at+len(w);w+=b'\x44\x89\x35'+struct.pack('<i',(rw2+0x4050+4*127)-(here+7))   # mov [eb127],r14d
  w+=bytes.fromhex('4489f04183fe05')
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x64cff7-(here+5))
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15500')
  buf[o:o+len(w)]=w
  image[site:site+7]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*2
  wrappers.append(dict(site=hex(site),address=hex(at),kind='b040-action-kind',wrapper_hex=w.hex()))
  site=0x667d50
  if image[site:site+6]!=bytes.fromhex('554889e54157'):raise ValueError('agent B b040 site B mismatch')
  at,buf,o=stub_slot(0x15580)
  w=bytearray()
  here=at+len(w);w+=b'\xff\x05'+struct.pack('<i',(rw2+0x4050+4*95)-(here+6))        # inc [eb95]
  w+=bytes.fromhex('50 8b06'.replace(' ',''))                                          # push rax; mov eax,[rsi]
  here=at+len(w);w+=b'\x89\x05'+struct.pack('<i',(rw2+0x4050+4*126)-(here+6))       # mov [eb126],eax
  w+=bytes.fromhex('58 554889e5 4157'.replace(' ',''))                                 # pop rax; replay
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x667d56-(here+5))
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15580')
  buf[o:o+len(w)]=w
  image[site:site+6]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind='b040-cinema-handler',wrapper_hex=w.hex()))
 # agent B b040: kind 165 at 0x63dcbb in the UI quad 0x63dc40: 'mov qword [rbp-0x70],0' (8 bytes, the batch's second handle) is
 #   replayed first, then the register-saving stub calls the helper, which gives an alpha-icon24 batch (DXT1 UI images: the
 #   emblem picker cells) the material's own default texture as a second handle; back at 0x63dcc3. Nothing jumps into
 #   0x63dcbc..0x63dcc2. U3MP_UI_DEFTEX=0 leaves it out.
 if os.environ.get('U3MP_UI_DEFTEX','0')=='1' or os.environ.get('U3MP_EMBLEM_DIAG','1')!='0' or (int(os.environ.get('U3MP_EMBLEM_TEST','3'))&3):   # agent B b041: opt-in (TEST4 b040: every cell shows the injected texture); agent J j004: also for its diagnostic / tests (kind 165 then does nothing to the batch unless U3MP_UI_DEFTEX=1 or U3MP_EMBLEM_TEST)
  site=0x63dcbb
  if image[site:site+8]!=bytes.fromhex('48c7459000000000'):raise ValueError('agent B b040 site mismatch')
  at,buf,o=stub_slot(0x15600)
  w=bytearray.fromhex('48c7459000000000')
  w+=bytes.fromhex('554889e5 50515256574150415141524153 53 4154 4155 4156 4157 488d7590 4883e4f0 4881ec00020000 480fae0424'.replace(' ',''))
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=b'\xba'+struct.pack('<I',165)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d6590 415f415e415d415c5b 415b415a41594158 5f5e5a5958 5d'.replace(' ',''))
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x63dcc3-(here+5))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15600')
  buf[o:o+len(w)]=w
  image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*3
  wrappers.append(dict(site=hex(site),address=hex(at),kind=165,wrapper_hex=w.hex()))
 # agent B b042 (diagnostics): kind 167 at 0x6a7c79 in the host match-start state (PS3 0x2bd2d4..0x2bd2f0): 'mov r14,rax; mov
 #   eax,[rbx+8]' (6 bytes, replayed first; rax = the fade object from 0x125fdb0) -> register-saving stub, helper records it; back
 #   at 0x6a7c7f (the flags of the replayed code are not used before the cmp there). U3MP_B042_DIAG=0 leaves it out.
 if os.environ.get('U3MP_B042_DIAG','1')!='0':
  site=0x6a7c79
  if image[site:site+6]!=bytes.fromhex('4989c68b4308'):raise ValueError('agent B b042 site mismatch')
  at,buf,o=stub_slot(0x15680)
  w=bytearray.fromhex('4989c68b4308')
  w+=bytes.fromhex('554889e5 50515256574150415141524153 53 4154 4155 4156 4157 488d7590 4883e4f0 4881ec00020000 480fae0424'.replace(' ',''))
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=b'\xba'+struct.pack('<I',167)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d6590 415f415e415d415c5b 415b415a41594158 5f5e5a5958 5d'.replace(' ',''))
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x6a7c7f-(here+5))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15680')
  buf[o:o+len(w)]=w
  image[site:site+6]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind=167,wrapper_hex=w.hex()))
 # agent B b046: kind 168 at the entry of the UI item draw 0x641560 ('push rbp; mov rbp,rsp; push r15', 6 bytes, replayed
 #   first) -> register-saving stub, the helper draws the PS3 type-46 level background for tagged booster icons; back at
 #   0x641566. U3MP_BOOSTER_RING=0 leaves the stub out (and eb[1] bit12 turns the tag off).
 if os.environ.get('U3MP_BOOSTER_RING','1')!='0':
  site=0x641560
  if image[site:site+6]!=bytes.fromhex('554889e54157'):raise ValueError('agent B b046 site mismatch')
  at,buf,o=stub_slot(0x15700)
  w=bytearray.fromhex('554889e54157')
  w+=bytes.fromhex('554889e5 50515256574150415141524153 53 4154 4155 4156 4157 488d7590 4883e4f0 4881ec00020000 480fae0424'.replace(' ',''))
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=b'\xba'+struct.pack('<I',168)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d6590 415f415e415d415c5b 415b415a41594158 5f5e5a5958 5d'.replace(' ',''))
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x641566-(here+5))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x15700')
  buf[o:o+len(w)]=w
  image[site:site+6]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind=168,wrapper_hex=w.hex()))
 # ===== agent E stubs (kinds 220..255, x-stub slots 0x16800..0x16f80) =====
 # agent E common stub (x-stub slot 0x16800): entered by 'call' from a per-site trampoline that pushed flags and a site index.
 #   Saves every register (and the FPU/SSE state), calls the helper with rsi = saved area (r[14] = the caller's rbp,
 #   r[15] = return address, r[16] = site index) and edx = the kind in the trampoline's push (high word) ... see e_tramp.
 def e_common(off,kind):
  at,buf,o=stub_slot(off)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use '+hex(off))
  w=bytearray.fromhex('554889e5 50515256574150415141524153 53 4154 4155 4156 4157 488d7590 4883e4f0 4881ec00020000 480fae0424'.replace(' ',''))
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=b'\xba'+struct.pack('<I',kind)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d6590 415f415e415d415c5b 415b415a41594158 5f5e5a5958 5d c3'.replace(' ',''))
  assert len(w)<=0x80,len(w)
  buf[o:o+len(w)]=w
  wrappers.append(dict(site='common',address=hex(at),kind=kind,wrapper_hex=w.hex()))
  return at
 # per-site trampoline (32 bytes, 4 per slot): replay the displaced instruction, pushfq, push index, call common, drop
 #   the index, popfq, jump back. The site's own instruction is >= 5 bytes; the rest is NOP-filled.
 def e_tramp(off,slot_i,common,site,orig,index,kind_tag):
  at0,buf,o0=stub_slot(off);at=at0+32*slot_i;o=o0+32*slot_i
  if image[site:site+len(orig)]!=orig:raise ValueError('agent E %s site mismatch %#x'%(kind_tag,site))
  if any(buf[o:o+32]):raise ValueError('trampoline in use %#x+%d'%(off,slot_i))
  w=bytearray(orig)+b'\x9c'+b'\x68'+struct.pack('<I',index)
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',common-(here+5))
  w+=bytes.fromhex('488d642408 9d'.replace(' ',''))
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',(site+len(orig))-(here+5))
  assert len(w)<=32,len(w)
  buf[o:o+len(w)]=w
  image[site:site+len(orig)]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*(len(orig)-5)
  wrappers.append(dict(site=hex(site),address=hex(at),kind=kind_tag,wrapper_hex=w.hex()))
 # agent E e002 (user item 2): loadout-control booster cells. Each PS4 type-0x2e item build ends with the store
 #   +0x28 = 1.0 ('mov dword [rbp+off+0x28],0x3f800000', 10 bytes); the trampoline replays it and kind 220 turns the item into
 #   b046's tagged type 3. Sites (item rbp offset): 0x607a62 (-0x680), 0x607d7f (-0x6f0), 0x6083a5 (-0x650), 0x60844f
 #   (-0x6c0), 0x60ce48 (-0x2a0), 0x60d106 (-0x2e0), 0x60ee20 (-0x130), 0x611ead (-0x2f0); the index order is the helper's
 #   e02_off table. Each build's type store 'mov dword [rbp+off],0x2e' is checked too.
 if os.environ.get('U3MP_LOADOUT_BOOSTER_ICON','1')!='0':
  e02c=e_common(0x16800,220)
  for i,(site,off) in enumerate(((0x607a62,-0x680),(0x607d7f,-0x6f0),(0x6083a5,-0x650),(0x60844f,-0x6c0),(0x60ce48,-0x2a0),(0x60d106,-0x2e0),(0x60ee20,-0x130),(0x611ead,-0x2f0))):
   orig=b'\xc7\x85'+struct.pack('<i',off+0x28)+b'\x00\x00\x80\x3f'
   ts=b'\xc7\x85'+struct.pack('<i',off)+b'\x2e\x00\x00\x00'
   if image.find(ts,site-0x60,site)<0:raise ValueError('agent E e002 type store not found before %#x'%site)
   e_tramp(0x16880+0x80*(i//4),i%4,e02c,site,orig,i,'e002')
 # ===== end agent E stubs =====
 # ===== agent I stubs (kinds 221..239, x-stub slots 0x16980..0x16f80, then 0x17100..0x17780) =====
 # helper(base, ev, kind, state) with ev[0..2] = rdi / rsi / rdx and ev[3] = 0 (the a018 thunk shape); returns ev[3].
 def i_fn(at,kind):
  w=bytearray.fromhex('554889e5 4883ec20 48893c24 4889742408 4889542410 48c744241800000000'.replace(' ',''))
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=bytes.fromhex('4889e6')+b'\xba'+struct.pack('<I',kind)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('488b442418c9c3')
  return w
 # agent I i001: LIST_PLAYERS row click 0x3406420c (registered by 0x674120 -> 0x671d00, entered only at its first byte) ->
 #   kind 221 = PS3 1.19 f_2844fc (helper i01_click). U3MP_PARTY119=0 leaves 0x671d00 alone.
 if os.environ.get('U3MP_PARTY119','1')!='0':
  site=0x671d00
  # menu-shell-v1 already routes the entry through its own guard at 0x2050800 (returns while g_matchSession +8 is null, else the
  # displaced prologue and back to 0x671d0b); i01_click replaces both.
  if bytes(image[site:site+7])!=bytes.fromhex('554889e5415653') and not (image[site]==0xe9 and site+5+struct.unpack_from('<i',image,site+1)[0]==0x2050800):raise ValueError('agent I i001 click site mismatch')
  at,buf,o=stub_slot(0x16980)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x16980')
  w=i_fn(at,221);assert len(w)<=0x80;buf[o:o+len(w)]=w
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=221,wrapper_hex=w.hex()))
 # agent I i002: list stat bar renderer, content type 0x2a (PS4 0x645c96, case of the item draw switch 0x64205b), ported from PS3
 #   1.19 case 42 (0x25f79c): kind 222 draws the whole case (track, base fill in the element colour, bonus segment) and the stub
 #   then jumps to the switch's merge point 0x645eea. The site's registers: rbp = the draw frame, r10 = the content item,
 #   ebx / r14 / r12 / r13 = the colour channels as the type-0x1c case reads them. U3MP_STAT_BARS=0 leaves the PS4 case.
 if os.environ.get('U3MP_STAT_BARS','1')!='0':
  site=0x645c96
  if bytes(image[site:site+9])!=bytes.fromhex('41c1ed08c4c17a2ac5'):raise ValueError('agent I i002 bar site mismatch')
  at,buf,o=stub_slot(0x16a00)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x16a00')
  w=bytearray.fromhex('4889e0 4883e4f0 4883ec40 4889442430 48892c24 4c89542408 48895c2410 4c89642418 4c896c2420 4c89742428'.replace(' ',''))
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=bytes.fromhex('4889e6')+b'\xba'+struct.pack('<I',222)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('488b642430')
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x645eea-(here+5))
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=222,wrapper_hex=w.hex()))
 # ===== end agent I stubs =====
 # ===== agent J stubs / patches (kinds 223.., x-stub slots 0x16a80.., see the agent J region of u3_lan_profiles.c) =====
 # agent J j001 (1): Statistics card name. PS3 f_20e450 fills its name buffer with f_364678(profile, buf): the 0x24-byte NpId at
 #   profile +0x7940 (PS4 +0x7ac0), else the local user's. PS4's 0x61cc40 never writes its buffer [rbp-0x88]. At 0x61d4c0
 #   ('lea rdi,[rbp-0x88]', 7 bytes; r12 = the viewed profile) the stub copies it (own name 0x17d6010 when +0x7ac0 is empty),
 #   terminates the 16-char handle, replays the lea and returns to the strlen at 0x61d4c7. U3MP_STATS_NAME=0 leaves it out.
 if os.environ.get('U3MP_STATS_NAME','1')!='0':
  site=0x61d4c0
  if bytes(image[site:site+12])!=bytes.fromhex('488dbd78ffffffe884f2d700'):raise ValueError('agent J j001 stats name site mismatch')
  at,buf,o=stub_slot(0x16a80)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x16a80')
  w=bytearray.fromhex('565751 498db424c07a0000 803e00 7507'.replace(' ',''))   # push rsi/rdi/rcx; lea rsi,[r12+0x7ac0]; cmp byte [rsi],0; jne +7
  here=at+len(w);w+=b'\x48\x8d\x35'+struct.pack('<i',0x17d6010-(here+7))         # lea rsi,[own name 0x17d6010]
  w+=bytes.fromhex('488dbd78ffffff b924000000 f3a4 c6458800 595f5e 488dbd78ffffff'.replace(' ',''))   # copy 0x24, [rbp-0x78]=0, pops, lea
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',(site+7)-(here+5))
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+7]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind='j001-name',wrapper_hex=w.hex()))
 # agent J j001 (2): layout engine second pass (PS4 0x65c5f0 = PS3 f_270f20). PS3 lays a flexible child out with the share
 #   (W - fixed widths) / n (0x271348..0x27139c: fsel on child w + flag), PS4 with child w + share, so a sized row overflows by
 #   its flexible children's widths (the stats card values right of the panel). Patched in place, PS3's rule:
 #   horizontal 0x65cb05 (42 bytes) / vertical 0x65cc5d (42): size = share; a fixed child (+0xb4 / +0xb8 > 0) gets 0 when its
 #   first-pass size is 0; no flexible child: 0x65ce3d / 0x65cf05 (8 each) size = share. xmm2 is dead at both sites (reloaded
 #   after the child call). U3MP_LAYOUT119=0 leaves PS4's rule.
 if os.environ.get('U3MP_LAYOUT119','1')!='0':
  for site,orig,new in (
    (0x65cb05,'c5fa10839c000000c5fa108bb4000000c5f82e0db36ede00c5f057c97708c5fa108dfcfeffffc5fa58c1',
              'c5e857d2c5fa1085fcfeffffc5f82e93b4000000730ec5f82e939c0000007504c5f828c2660f1f440000'),
    (0x65cc5d,'c5fa1083a0000000c5fa108bb8000000c5f82e0d5b6dde00c5f057c97708c5fa108dfcfeffffc5fa58c9',
              'c5e857d2c5fa108dfcfeffffc5f82e93b8000000730ec5f82e93a00000007504c5f828ca660f1f440000'),
    (0x65ce3d,'c5fa58839c000000','0f1f840000000000'),
    (0x65cf05,'c5fa588ba0000000','c5f828c80f1f4000')):
   o_=bytes.fromhex(orig);n_=bytes.fromhex(new);assert len(o_)==len(n_),hex(site)
   if bytes(image[site:site+len(o_)])!=o_:raise ValueError('agent J j001 layout site mismatch %#x'%site)
   image[site:site+len(n_)]=n_
 # agent J j001 (3): Statistics rows. PS3 1.19 tables 0xdb57b0 (competitive, 7 pairs {text id, record stat}: Objective Points
 #   0xf84a8361 / 0x194 last) and 0xdb5780 (co-op, 5 pairs); PS4 0x13a5e60 / 0x13a5e90 hold 6 + 6 (Objective Points in the co-op
 #   list). Same 12 pairs in all: PS3's written over PS4's, the co-op base moves to 0x13a5e98 (leas 0x61e970 / 0x61eaef) and the
 #   loop limits become 14 (0x61f762, competitive) / 10 (0x61edc8, co-op). U3MP_STATS_ROWS119=0 leaves PS4's rows.
 if os.environ.get('U3MP_STATS_ROWS119','1')!='0':
  t0=0x13a5e60
  ps4=(0x49f5e231,0x167,0xcd3394e9,0x168,0x99e71e1b,0x16b,0xb9602dbb,0x169,0x966fa9ae,0x16a,0xb8fc005b,0x193,
       0x5c042e1b,0x195,0xf84a8361,0x194,0x5bc37867,0x196,0xd275cc11,0x197,0xad74aaa0,0x198,0x27e60793,0x199)
  ps3=(0x49f5e231,0x167,0xcd3394e9,0x168,0x99e71e1b,0x16b,0xb9602dbb,0x169,0x966fa9ae,0x16a,0xb8fc005b,0x193,0xf84a8361,0x194,
       0x5c042e1b,0x195,0x5bc37867,0x196,0xd275cc11,0x197,0xad74aaa0,0x198,0x27e60793,0x199)
  if bytes(image[t0:t0+0x60])!=struct.pack('<24I',*ps4):raise ValueError('agent J j001 stats table mismatch')
  image[t0:t0+0x60]=struct.pack('<24I',*ps3)
  for site in (0x61e970,0x61eaef):
   if bytes(image[site:site+3])!=b'\x48\x8d\x05' or site+7+struct.unpack_from('<i',image,site+3)[0]!=0x13a5e90:raise ValueError('agent J j001 co-op table lea %#x'%site)
   image[site+3:site+7]=struct.pack('<i',0x13a5e98-(site+7))
  for site,n in ((0x61f762,14),(0x61edc8,10)):
   if bytes(image[site:site+4])!=b'\x41\x83\xfd\x0c':raise ValueError('agent J j001 row loop %#x'%site)
   image[site+3]=n
 # agent J j002: Cinema film net game index. The film player 0x6699c0 (format 0xe) reads the stream's net game index and calls
 #   SetSelectedNetGameIdx 0x720610, which asserts on a slot the net game table DC 0x3b9a067d lacks (tester crash after two LAN
 #   matches; cin_select is guarded in the helper). At 0x669c37 ('movsx esi,r14w; lea rdi,[0x17cb060]', 11 bytes) the stub
 #   passes r14 to kind 223 (j_kind), which answers the checked index; then esi/rdi as before and back to 0x669c42. rax, rcx,
 #   rdx, rsi, rdi, r8-r11 are dead there (0x669c42 sets edx/ecx and calls). U3MP_CIN_GAME_GUARD=0 leaves the site alone.
 if os.environ.get('U3MP_CIN_GAME_GUARD','1')!='0':
  site=0x669c37
  if bytes(image[site:site+11])!=bytes.fromhex('410fbff6488d3d1e141601'):raise ValueError('agent J j002 film select site mismatch')
  at,buf,o=stub_slot(0x16b00)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x16b00')
  w=bytearray.fromhex('554889e5 4883e4f0 4883ec20 4c893424 48c744240800000000 48c744241000000000 48c744241800000000'.replace(' ',''))
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))            # rdi = image base
  w+=bytes.fromhex('4889e6')+b'\xba'+struct.pack('<I',223)                    # rsi = ev; edx = kind
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('488b442418 4889ec 5d 0fbff0'.replace(' ',''))              # rax = ev[3]; leave; movsx esi,ax
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',0x17cb060-(here+7))     # lea rdi,[0x17cb060]
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x669c42-(here+5))
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+11]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*6
  wrappers.append(dict(site=hex(site),address=hex(at),kind=223,wrapper_hex=w.hex()))
 # agent J j003: Statistics card rank badge. 0x61cc40 builds it as content item type 0x29, which PS4's item draw switch never
 #   draws. At 0x61d28b ('mov [rcx+0xf8],rdi', 7 bytes: rdi = the badge node's content item, 0 when the pool is full) the
 #   register-saving stub (b040's form) replays the store and calls kind 224, which turns the item into the texture icon the
 #   list badges use (ranks/<n>); back at 0x61d292. U3MP_STATS_BADGE=0 leaves it out.
 if os.environ.get('U3MP_STATS_BADGE','1')!='0':
  site=0x61d28b
  if bytes(image[site:site+7])!=bytes.fromhex('4889b9f8000000'):raise ValueError('agent J j003 badge site mismatch')
  at,buf,o=stub_slot(0x16b80)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x16b80')
  w=bytearray.fromhex('4889b9f8000000')
  w+=bytes.fromhex('554889e5 50515256574150415141524153 53 4154 4155 4156 4157 488d7590 4883e4f0 4881ec00020000 480fae0424'.replace(' ',''))
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=b'\xba'+struct.pack('<I',224)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d6590 415f415e415d415c5b 415b415a41594158 5f5e5a5958 5d'.replace(' ',''))
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',(site+7)-(here+5))
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+7]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind=224,wrapper_hex=w.hex()))
 # agent J j005: label adapter 0x167d370 op 1 (0x634f40) looks text id 0 up without a zero test (first db string, "Choose LAN
 #   Profile" above PAGE_BOOSTERS). At 0x634f4c ('mov r14d,[rdi+0x10]; lea rax,[0x1b24b80]', 11 bytes, op-1 branch only) the
 #   register-saving stub replays both and calls kind 225; the helper sets the saved rax to 1 to ask for "", which the stub sends
 #   to 0x634fa6 ('lea rbx,""', return), else it continues at 0x634f57 with rax = 0x1b24b80. U3MP_TEXT_ID0=0 leaves it out.
 if os.environ.get('U3MP_TEXT_ID0','1')!='0':
  site=0x634f4c
  if bytes(image[site:site+11])!=bytes.fromhex('448b7710488d0529fc4e01'):raise ValueError('agent J j005 label site mismatch')
  at,buf,o=stub_slot(0x16c00)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x16c00')
  w=bytearray.fromhex('448b7710')
  here=at+len(w);w+=b'\x48\x8d\x05'+struct.pack('<i',0x1b24b80-(here+7))
  w+=bytes.fromhex('554889e5 50515256574150415141524153 53 4154 4155 4156 4157 488d7590 4883e4f0 4881ec00020000 480fae0424'.replace(' ',''))
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=b'\xba'+struct.pack('<I',225)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d6590 415f415e415d415c5b 415b415a41594158 5f5e5a5958 5d'.replace(' ',''))
  w+=bytes.fromhex('4883f801')                                            # cmp rax,1
  here=at+len(w);w+=b'\x0f\x84'+struct.pack('<i',0x634fa6-(here+6))        # je 0x634fa6 (return "")
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x634f57-(here+5))
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+11]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*6
  wrappers.append(dict(site=hex(site),address=hex(at),kind=225,wrapper_hex=w.hex()))
 # agent J j006: Start Cinema 0x665060 ('push rbp; mov rbp,rsp; push r15; push r14', 8 bytes, a function entry reached through the
 #   button callback) -> value stub kind 226: the helper re-reads the selected film's stream head and answers 1 (return without
 #   starting) when it is not format 14, which would assert in the player's Enter (0x669d72). U3MP_CIN_START_GUARD=0 leaves it out.
 if os.environ.get('U3MP_CIN_START_GUARD','1')!='0' and CINEMA:
  site=0x665060
  if bytes(image[site:site+8])!=bytes.fromhex('554889e541574156'):raise ValueError('agent J j006 start site mismatch')
  at,w=value_stub(0x16c80,226,lambda h:bytes.fromhex('554889e541574156'),site+8)
  image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*3
  wrappers.append(dict(site=hex(site),address=hex(at),kind=226,wrapper_hex=w.hex()))
 # agent J j007: film writer buffer slide (0x6a2ea8..0x6a2f03): 'call 0x139c5e0' at 0x6a2ee5 (memcpy(buf, buf+0x8000, n)) -> x-stub
 #   0x16d00: the same memcpy, then the 0x8000 bytes it vacated (buf+n ..) are zeroed, so PS4's inlined bit writers (keep-mask
 #   ~0 << 32 = ~0 on x86 for an aligned write) never OR new words into old ones. rax/rcx/rdi are dead after the call (0x6a2eea
 #   reloads them). U3MP_CIN_ZERO=0 leaves it out.
 if os.environ.get('U3MP_CIN_ZERO','1')!='0' and CINEMA:
  site=0x6a2ee5
  if bytes(image[site:site+5])!=bytes.fromhex('e8f696cf00'):raise ValueError('agent J j007 slide site mismatch')
  at,buf,o=stub_slot(0x16d00)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x16d00')
  w=bytearray.fromhex('57 52 4883ec08'.replace(' ',''))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',0x139c5e0-(here+5))
  w+=bytes.fromhex('4883c408 5a 5f 4885d2 780c 4801d7 b900800000 31c0 f3aa c3'.replace(' ',''))
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind='j007-slide',wrapper_hex=w.hex()))
 # agent J j008 (2): text lookup 0xf32ae0(db, id): id 0 is not in any text db, so it returns null and its callers fall back to
 #   the db's first string ("Choose LAN Profile", the grey title above BOOSTER SLOT n). At the entry ('push rbp; mov rbp,rsp;
 #   push r14; push rbx', 7 bytes) a prefix stub tests esi: non-zero -> replay and back to 0xf32ae7 (no other cost); zero ->
 #   value stub kind 227 (records the caller; answers "" by setting the saved rax and returning, except on
 #   PAGE_LAN_CHOOSE_PROFILE). Slots 0x16d80 (prefix) / 0x16e00 (value stub). U3MP_TEXT_ID0B=0 leaves both out.
 if os.environ.get('U3MP_TEXT_ID0B','1')!='0':
  site=0xf32ae0
  if bytes(image[site:site+7])!=bytes.fromhex('554889e5415653'):raise ValueError('agent J j008 text lookup site mismatch')
  vat,vw=value_stub(0x16e00,227,lambda h:bytes.fromhex('554889e5415653'),site+7)
  wrappers.append(dict(site=hex(site),address=hex(vat),kind=227,wrapper_hex=vw.hex()))
  at,buf,o=stub_slot(0x16d80)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x16d80')
  w=bytearray.fromhex('85f6')                                              # test esi,esi
  here=at+len(w);w+=b'\x0f\x84'+struct.pack('<i',vat-(here+6))             # jz value stub
  w+=bytes.fromhex('554889e5415653')                                      # replay
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',(site+7)-(here+5))
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+7]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind='j008-text0',wrapper_hex=w.hex()))
 # agent J j009 (2): text lookup misses. 0xf32b36 is the lookup's shared epilogue ('pop rbx; pop r14; pop rbp; ret', 5 bytes,
 #   only ever a branch target at its first byte; 0xf32b3b.. is padding). x-stub 0x16e80 keeps id/db (edx = ebx, rcx = r14), pops,
 #   returns at once for a non-null result or for the parent-db recursion (return address 0xf32afc), else value stub kind 228
 #   (0x16f00, replay = 'ret'): the helper answers "" for db 0x17026b8 (see the j009 header). U3MP_TEXT_MISS=0 leaves it out.
 if os.environ.get('U3MP_TEXT_MISS','1')!='0':
  site=0xf32b36
  if bytes(image[site:site+5])!=bytes.fromhex('5b415e5dc3'):raise ValueError('agent J j009 text lookup epilogue mismatch')
  vat,vw=value_stub(0x16f00,228,lambda h:b'\xc3',site+5)
  wrappers.append(dict(site=hex(site),address=hex(vat),kind=228,wrapper_hex=vw.hex()))
  at,buf,o=stub_slot(0x16e80)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x16e80')
  w=bytearray.fromhex('89da4c89f15b415e5d4885c0')                         # mov edx,ebx; mov rcx,r14; pops; test rax,rax
  w+=b'\x75\x00';jnz=len(w)-1                                              # jnz ret
  here=at+len(w);w+=b'\x48\x8d\x35'+struct.pack('<i',0xf32afc-(here+7))  # lea rsi,[0xf32afc]
  w+=bytes.fromhex('48393424')                                            # cmp [rsp],rsi
  w+=b'\x74\x00';je=len(w)-1                                               # je ret
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',vat-(here+5))                 # jmp value stub
  r=len(w);w+=b'\xc3'
  w[jnz]=r-(jnz+1);w[je]=r-(je+1)
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind='j009-textmiss',wrapper_hex=w.hex()))
 # ===== agent K stubs (x-stub slots 0x17300..0x17780, kinds 240..255) =====
 # agent K k001 (4): PAGE_CUSTOMRULES rows that do not apply to the game type (label "") are hidden as on PS3: PS3 builds styles
 #   6 and 13 with one class (ctor 0x210c7c) whose measure f_212e38 / focus test 0x206d54 / draw f_230194 all test the label
 #   (f_24d4a8: op-1 text null or ""). PS4's style-6 class (vtable 0x1678ef0) lost the tests. Value stubs (helper decides by the
 #   object's vtable): focusable 0x622ee0 ('mov al,[rdi+0xad]', 6 bytes, then ret) kind 240 slot 0x17300; measure 0x5df5f0
 #   ('push rbp; mov rbp,rsp; push r15; push r14', 8 bytes, function entry) kind 241 slot 0x17380; draw 0x5e2ae0 (same 8 bytes)
 #   kind 242 slot 0x17400. Nothing jumps into the displaced bytes (function entries; 0x622ee6 is the ret the stub returns to).
 #   U3MP_SETTINGS119=0 leaves them out (and sets byte rw2+0x4059 bit1).
 if os.environ.get('U3MP_SETTINGS119','1')!='0':
  for site,orig,slot,kind,back in ((0x622ee0,'8a87ad000000',0x17300,240,0x622ee6),
                                   (0x5df5f0,'554889e541574156',0x17380,241,0x5df5f8),
                                   (0x5e2ae0,'554889e541574156',0x17400,242,0x5e2ae8)):
   ob=bytes.fromhex(orig)
   if bytes(image[site:site+len(ob)])!=ob:raise ValueError('agent K k001 settings site mismatch %#x'%site)
   at,w=value_stub(slot,kind,lambda h,ob=ob:ob,back)
   image[site:site+len(ob)]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*(len(ob)-5)
   wrappers.append(dict(site=hex(site),address=hex(at),kind=kind,wrapper_hex=w.hex()))
 # ===== end agent K stubs =====
 # ===== agent M stubs (x-stub slots 0x17480..0x17780, kinds 243..255, switch byte rw2+0x4059 bits 2..7) =====
 # agent M m002 (1): Advanced Settings > Weapon Mode in the match. The spawn (0x788826 -> 0xbe68b0 r8d) already gets the mode's
 #   weapon set from the v550 stub at 0x7884f4, but the inventory init (PS4 0xbbc230 = vt+0x30 of the 0x16a0b70 class; 1.10
 #   code) gives the set's weapons only when not in MP (0x17d5c61 == 0) or in co-op (r12b), so competitive got the profile
 #   loadout. PS3 1.19 f_594260 (0x594304..0x594328, 0x594ef8..0x594f1c): when player +0x906 (PS4 +0xcea) is 0 and the set is
 #   neither the default 0x48f5f44 nor the competitive record set 0x8cf8e1cc, it sets r28 = 1 (0x5943b0), and 0x5943c8 then takes
 #   the set path even with g+5. PS4's copy of that branch is 0xbbc34a ('mov r8,rbx; jmp 0xbbc474', 8 bytes; reached only when
 #   +0xcea == 0 and the set != 0x48f5f44; nothing jumps into 0xbbc34b..0xbbc351): the stub adds the 0x8cf8e1cc test and sets
 #   r12d = 1 (r12 is reset by the set path at 0xbbc555 before any other use). 0x11f2e30 clobbers only what the call at 0xbbc336
 #   already clobbered; rsp is 16-aligned here (same frame as that call). U3MP_WEAPON_MODE119=0 leaves it out (+ byte bit2).
 if os.environ.get('U3MP_WEAPON_MODE119','1')!='0':
  site=0xbbc34a;orig=bytes.fromhex('4989d8e922010000')
  if bytes(image[site:site+8])!=orig:raise ValueError('agent M m002 inventory-init site mismatch')
  at,buf,o=stub_slot(0x17480)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x17480')
  w=bytearray()
  here=at+len(w);w+=b'\xf6\x05'+struct.pack('<i',(rw2+0x4059)-(here+7))+b'\x04'   # test byte [rw2+0x4059],4
  w+=b'\x75\x00';j1=len(w)-1                                                   # jnz orig
  w+=b'\xbf'+struct.pack('<I',0x8cf8e1cc)                                      # mov edi,0x8cf8e1cc
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',0x11f2e30-(here+5))               # call 0x11f2e30 (DC symbol lookup)
  w+=bytes.fromhex('483b85a0feffff')                                           # cmp rax,[rbp-0x160] (the set)
  w+=b'\x74\x00';j2=len(w)-1                                                   # je orig
  w+=bytes.fromhex('41bc01000000')                                             # mov r12d,1
  og=len(w);w[j1]=og-(j1+1);w[j2]=og-(j2+1)
  w+=bytes.fromhex('4989d8')                                                   # mov r8,rbx
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0xbbc474-(here+5))                # jmp 0xbbc474
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*3
  wrappers.append(dict(site=hex(site),address=hex(at),kind='m002-weapon-mode-set',wrapper_hex=w.hex()))
 # agent M m004 (2): NetMenuHost Refresh 0x7503c0 -> PS3 1.19 f_355ae0 port (kind 243, value stub slot 0x17900; replay
 #   'push rbp; mov rbp,rsp; push r15; push r14', back 0x7503c8), and the table moved to rw2+0xed000 for 1.19's 11 game types
 #   (PS4's 0x17d7b30 holds 10; 0x17d9988 is another global): the 11 'lea r,[rip->0x17d7b30]' sites get the new target.
 #   U3MP_GAMETYPES119=0 leaves all of it out (and sets byte rw2+0x4059 bit7).
 if os.environ.get('U3MP_GAMETYPES119','1')!='0':
  site=0x7503c0;ob=bytes.fromhex('554889e541574156')
  if bytes(image[site:site+8])!=ob:raise ValueError('agent M m004 refresh site mismatch')
  at,w=value_stub(0x17900,243,lambda h:ob,0x7503c8)
  image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*3
  wrappers.append(dict(site=hex(site),address=hex(at),kind=243,wrapper_hex=w.hex()))
  for site in (0x751198,0x752d0e,0x752e92,0x752eb9,0x753355,0x7534df,0x75350d,0x753875,0x753898,0x753af4,0x754d53):
   if image[site+1]!=0x8d or image[site] not in (0x48,0x4c) or site+7+struct.unpack_from('<i',image,site+3)[0]!=0x17d7b30:
    raise ValueError('agent M m004 NetMenuHost lea mismatch %#x'%site)
   struct.pack_into('<i',image,site+3,(rw2+0xed000)-(site+7))
 # ===== end agent M stubs =====
 # fixer L n033 (opt-in, U3MP_N033=1 diagnostic / =2 diagnostic + constant fix): wrap the type-2 particle geometry draw at 0xf67534
 # (xor ecx,ecx; mov rdi,r14; mov rdx,[rbp-0x108]; call [rax+0x10] = 15 bytes, no jump targets inside) with two kind-244 value
 # stubs: x-stub 0x13880 (r11 = 3, r10 = r14 the drawable, rsi = [r12+8] the command buffer) replays the 15 bytes, so the draw
 # returns into the stub, then sets r11 = 4 and goes to x-stub 0x13900 (post), which goes back to 0xf67543.
 if os.environ.get('U3MP_N033','0') in ('1','2'):
  site=0xf67534;o=bytes.fromhex('31c94c89f7488b95f8feffffff5010')
  if image[site:site+15]!=o:raise ValueError('fixer L n033 ring draw site mismatch')
  S2,w2=value_stub(0x13900,244,lambda h:b'',0xf67543)
  S1,w1=value_stub(0x13880,244,lambda h:o+bytes.fromhex('41bb040000004d89f2'),S2)
  image[site:site+15]=bytes.fromhex('41bb030000004d89f2')+b'\xe9'+struct.pack('<i',S1-(site+14))+b'\x90'
  wrappers.append(dict(site=hex(site),address=hex(S1),kind=244,wrapper_hex=w1.hex()))
  wrappers.append(dict(site='0xf67543',address=hex(S2),kind=244,wrapper_hex=w2.hex()))
 # ===== fixer L stubs (kind 244, x-stub slots 0x17c00 / 0x17c80, switch bits ExtState.ec[2] = rw2+0x4458..0x445b) =====
 # n003: the "(Triangle) Friends" header element (menu widget class 0x2b, factory case 0x64b07a, vtable 0x167a7b0): slot 3 draw
 #   0x61f830 (PS3 1.19 f_20a05c) and slot 2 update 0x61fb00 (f_209fa4, Triangle -> PAGE_FRIENDS) both return while the LAN byte
 #   0x17d5c60 is set, which offline mode (b044) keeps set. The vtable slots are relocated pointers (relocations 0x167a7d8 /
 #   0x167a7d0, type 0): they are retargeted below (after the relocation copy) to two value stubs, kind 244 with the sub id in r11
 #   (0 draw, 1 update); helper 0 = jump to the native method. The FB glyph " |FB|" (0x145dadb, PS4 passes it always; PS3 only
 #   with Facebook linked) becomes "" (its NUL 0x145dae0) at both 'lea rcx' sites: user decision, no Facebook (a018).
 #   U3MP_FRIENDS_HDR=0 leaves all of it out (+ ec[2] bit0); U3MP_FRIENDS_FB=1 keeps the glyph (+ ec[2] bit1).
 n03_reloc={}
 if os.environ.get('U3MP_FRIENDS_HDR','1')!='0':
  for sub,off,fn,slot,ob in ((0,0x17c00,0x61f830,0x167a7d8,'554889e541574156'),(1,0x17c80,0x61fb00,0x167a7d0,'554889e5488d054d')):
   if bytes(image[fn:fn+8])!=bytes.fromhex(ob):raise ValueError('fixer L n003 method mismatch %#x'%fn)
   at,buf,o=stub_slot(off)
   if any(buf[o:o+0x80]):raise ValueError('stub slot in use '+hex(off))
   w=bytearray(b'\x41\xbb'+struct.pack('<I',sub))                                 # mov r11d,sub (r[0] for the helper)
   w+=bytes.fromhex('554889e5 50515256574150415141524153 4883e4f0 4881ec00020000 480fae0424')
   here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
   w+=bytes.fromhex('488d75b8')+b'\xba'+struct.pack('<I',244)
   here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
   here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
   w+=bytes.fromhex('480fae0c24 488d65b8 488945b8 415b415a41594158 5f5e5a5958 5d 4d85db 7401 c3')
   here=at+len(w);w+=b'\xe9'+struct.pack('<i',fn-(here+5))                       # helper 0: the native method
   assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
   n03_reloc[slot]=(fn,at)
   wrappers.append(dict(site=hex(slot),address=hex(at),kind=244,wrapper_hex=w.hex()))
  if os.environ.get('U3MP_FRIENDS_FB','0')!='1':
   for site in (0x61f9a8,0x61fa01):
    if image[site:site+3]!=b'\x48\x8d\x0d' or site+7+struct.unpack_from('<i',image,site+3)[0]!=0x145dadb or image[0x145dae0]!=0:
     raise ValueError('fixer L n003 FB glyph site mismatch %#x'%site)
    struct.pack_into('<i',image,site+3,0x145dae0-(site+7))
 # n007: HUD world-marker colours (objective icons, capture ring, numbers). Every producer packs the entry colour as PS3 ARGB
 #   (DcNetDrawCustomObjectiveIcon 0x797c3d.. = PS3 1.19 0x37eae4..0x37eb84: a<<24 | r<<16 | g<<8 | b; team colours 0x741a10..
 #   = PS3 f_3133e0 table: 0x4080d030 green, 0x40ff4030 red, ...; constant white 0xb0f0f0f0), and PS3 1.19's marker draw f_670050
 #   reads (r, g, b) = bytes 2, 1, 0 and alpha = byte 3. PS4's marker draw 0xc28ce0 unpacks entry +0x30 at 0xc29305 into lanes
 #   (byte3, byte2, byte1, byte0) and draws lanes 0..2 as RGB (0xc29a88..0xc29ab2), so white came out cyan (0xb0, 0xf0, 0xf0), the
 #   green ring blue / lavender, red green. The 33 bytes become the PS3 order: lanes (byte2, byte1, byte0, byte3).
 #   U3MP_HUD_ARGB=0 leaves it out (+ ec[2] bit5, which also keeps t009's ring colour order).
 if os.environ.get('U3MP_HUD_ARGB','1')!='0':
  site=0xc29305
  orig=bytes.fromhex('418b4c1c4089cac1ea18be10080000c4e248f7f1bf08080000c4e240f7f90fb6c9')
  new=bytes.fromhex('418b4c1c40ba10080000c4e268f7d1be08080000c4e248f7f10fb6f9c1e9189090')
  assert len(orig)==len(new)==33
  if bytes(image[site:site+33])!=orig:raise ValueError('fixer L n007 colour unpack mismatch')
  image[site:site+33]=new
 # n008: U3MP_MM_BACK_ANY=1 (opt-in): Circle on the matchmaking page in every phase. PS4's page handler 0x74dd90 op 5 ("can back",
 #   0x74e151..0x74e16e) = PS3 1.19 f_32dc98 op 5 (0x32dec8..0x32df0c): debug[0x68] || status (net +0x40) < 2, so from "Voting
 #   Starts in" on Circle does nothing, on PS3 as well. The patch makes it status < 7 (cmp dword [rax+0x40], 2 -> 7).
 if os.environ.get('U3MP_MM_BACK_ANY','0')=='1':
  site=0x74e167
  if bytes(image[site:site+4])!=bytes.fromhex('83784002'):raise ValueError('fixer L n008 back test mismatch')
  image[site+3]=7
 # n010: ALLOCATION_SCRIPT_RELOC_HEAP (budget id 0x6c, table 0x16f8d90, entry 0x16f8f30: {id, class 5, size, align 0x80, name}),
 #   the heap Loader::Start allocates script modules from (0x11fe4e5, created at 0x11f9ff0..0x11fa282). PS4 retail 0x750800
 #   (PS3 1.19: id 44, 0x592000, from the table f_202190 builds). The restoration's always-loaded DC modules (net.bin +372 KB,
 #   modules / menu / medals), the ported ss-* modules and the 1.19 per-character vox modules (vox/characters/<x>, PS3 loads them
 #   too: PS3 dc1 vox/characters has all 45) do not fit in Co-op Adventure (v6.38: "Not enough memory to allocate module
 #   vox/characters/lt-draza"). The size grows by U3MP_SCRIPT_HEAP_ADD (default 0x200000 = 2 MB). U3MP_SCRIPT_HEAP_ADD=0 keeps
 #   PS4's size.
 # n011: the class-5 (kAllocAppCpu) entries must add up to EXACTLY the context size 0x60000000 (ValidateContext 0xd9dec0: sum of
 #   size + padding over every entry whose class == context, compared with ==; only context 6 also counts classes 0x2b/0x2e).
 #   Retail class 5 sums to 0x60000000 with ALLOCATION_APP_CPU_FREE (id 0xc, class 5, 0x53e000 = 5368 KB, align 1) as the
 #   remainder, so n010's +ADD asserted "Context(kAllocAppCpu) Size Does Not Add Up! ... 1574912 KB" before the title.
 #   The FREE entry now shrinks by the same ADD, and the builder re-checks the class-5 sum and the class-6 sum afterwards.
 _n10add=int(os.environ.get('U3MP_SCRIPT_HEAP_ADD','0x200000'),0)
 if _n10add:
  _bt=0x16f8d90
  def _n11_entries():
   out=[];o=_bt
   while True:
    idv,cl,sz,al=struct.unpack_from('<IIQQ',image,o)
    if idv==0:break
    out.append((o,idv,cl,sz,al));o+=0x20
    if len(out)>400:raise ValueError('fixer L n011: budget table has no terminator')
   return out
  _ents=_n11_entries()
  _rel=[x for x in _ents if x[1]==0x6c];_free=[x for x in _ents if x[1]==0xc]
  if len(_rel)!=1 or len(_free)!=1:raise ValueError('fixer L n011: budget ids 0x6c / 0xc not unique')
  e=_rel[0][0];ef=_free[0][0]
  if e!=0x16f8f30 or struct.unpack_from('<IIQQ',image,e)!=(0x6c,5,0x750800,0x80):raise ValueError('fixer L n010 script heap budget mismatch')
  if ef!=0x16f9b90 or struct.unpack_from('<IIQQ',image,ef)!=(0xc,5,0x53e000,1):raise ValueError('fixer L n011 ALLOCATION_APP_CPU_FREE mismatch')
  # n011: whole MB only, so every class-5 entry after RELOC keeps its 0x100000 (largest class-5) alignment padding.
  if _n10add<0 or _n10add>0x400000 or _n10add&0xfffff or _n10add>0x53e000:raise ValueError('fixer L n011: U3MP_SCRIPT_HEAP_ADD must be 0, 0x100000 .. 0x400000 in whole MB')
  _sum=lambda cls:sum(x[3] for x in _n11_entries() if x[2] in cls)
  _s5,_s6=_sum((5,)),_sum((6,0x2b,0x2e))
  if _s5!=0x60000000:raise ValueError('fixer L n011: retail class-5 sum %#x'%_s5)
  struct.pack_into('<Q',image,e+8,0x750800+_n10add)
  struct.pack_into('<Q',image,ef+8,0x53e000-_n10add)
  if _sum((5,))!=0x60000000 or _sum((6,0x2b,0x2e))!=_s6:raise ValueError('fixer L n011: budget sums changed')
 # n017: solo Co-op Hunter Arena in a SPOOFED Find Game match (offline spoof deviation, user decision 2026-10-30). The match-end
 #   check 0x721850 (= PS3 f_30d994 0x30e1c4..0x30e2d0) ends net type 18 at once when every active player is on one team (PS3
 #   behaviour, kept for Custom Game). The spoof's other players are only menu rows, so Find Game Hunter ended ~6 s after start
 #   (g7f: 48 -> 32 without 29). At 0x722011 (je 0x7220dd, the type-18 branch; v5.36's U3MP_SOLO_HUNTER=1 NOPs it for every
 #   match) the je goes to x-stub 0x13b80: type != 18 -> fall through as before; type 18 -> the branch, unless n013's spoof flag
 #   ExtState.ea[94] is set, then fall through to the co-op path (the v5.36 behaviour) and count ea[95]. Custom Game keeps PS3's
 #   end. U3MP_MM_SOLOHUNT=0 (ec[3] bit9 = rw2+0x445d bit1) keeps the branch for the spoof too; the stub is left out when
 #   U3MP_SOLO_HUNTER=1 already removed the branch.
 if os.environ.get('U3MP_SOLO_HUNTER')!='1':
  site=0x722011;tgt=0x7220dd;nxt=site+6
  if image[0x72200c:site+6]!=bytes.fromhex('837c080c120f84c6000000'):raise ValueError('fixer L n017 hunter end check site mismatch')
  at,buf,o=stub_slot(0x13b80)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x13b80')
  w=bytearray()
  def rel(n,target):return struct.pack('<i',target-(at+len(w)+n))
  w+=b'\x0f\x85'+rel(6,nxt)                                         # jne next (flags of cmp [..+0xc],0x12)
  w+=b'\x83\x3d'+rel(7,rw2+0x3c50+94*4)+b'\x00'                      # cmp dword [ea94],0
  w+=b'\x0f\x84'+rel(6,tgt)                                         # je  0x7220dd (not a spoofed match: PS3 end check)
  w+=b'\xf6\x05'+rel(7,rw2+0x445d)+b'\x02'                           # test byte [ec3 byte1],2
  w+=b'\x0f\x85'+rel(6,tgt)                                         # switch off: PS3 end check
  w+=b'\xff\x05'+rel(6,rw2+0x3c50+95*4)                              # inc dword [ea95]
  w+=b'\xe9'+rel(5,nxt)                                              # co-op path (skips the one-team check)
  assert len(w)<=0x80;buf[o:o+len(w)]=w
  image[site:site+6]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind='n017-spoof-solo-hunter',wrapper_hex=w.hex()))
 # n021 (DIAGNOSTIC, opt-in U3MP_LPROBE_LOG=1; nothing is installed by default): log every object light-probe lookup. At 0xf80050,
 #   right after the light-env builder's call of BlendLightProbe 0xf82290 (call at 0xf8004b), the 8 bytes 'mov rsi,r15; vmovaps
 #   xmm0,[rbx+0x50]' become a jump to x-stub 0x13b00: r11d = 2 (kind 244 sub), r10 = rbx (the result light env), r9 = r15 (the
 #   object); full save, helper, restore, the 8 bytes, jmp 0xf80058. r9/r10/r11 are dead there (caller-saved, just after a call).
 if os.environ.get('U3MP_LPROBE_LOG','0')=='1':
  site=0xf80050
  if bytes(image[site:site+8])!=bytes.fromhex('4c89fec5f8284350'):raise ValueError('fixer L n021 site mismatch')
  at,buf,o=stub_slot(0x13b00)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x13b00')
  w=bytearray(b'\x41\xbb'+struct.pack('<I',2)+bytes.fromhex('4989da 4d89f9'))   # mov r11d,2; mov r10,rbx; mov r9,r15
  w+=bytes.fromhex('554889e5 50515256574150415141524153 4883e4f0 4881ec00020000 480fae0424')
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))
  w+=bytes.fromhex('488d75b8')+b'\xba'+struct.pack('<I',244)
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d65b8 415b415a41594158 5f5e5a5958 5d')
  w+=bytes.fromhex('4c89fec5f8284350')
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',site+8-(here+5))
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90\x90\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind=244,wrapper_hex=w.hex()))
 # n022: Find Game -> Co-op Arena / Co-op Adventure / Co-op Shade Survival crashed at the matchmaking page open (0x74dca4 reads
 #   NULL+0x58): the playlist lookup 0x73f7d0 (= PS3 1.19 f_304080 + an unlock test) filters by co-op word (entry +0x28) == co-op
 #   byte 0x17d6065 while the LAN byte 0x17d5c60 is set. PS3 runs Find Game online only (LAN byte 0: no filter); offline mode runs
 #   the online lobby with the LAN byte set and co-op byte 0, so co-op playlists were not found. At 0x73f813 the 11 bytes
 #   'mov al,[rsi+8]; mov rdx,[rdx+8]; test al,al; je 0x73f890' become a jump to x-stub 0x13a80: replayed, then with the LAN byte
 #   set and ExtState.ea[118] (helper n22_tick: offline mode and the switch on) set it takes 0x73f890 (PS3's online path, ea[119]++),
 #   else the co-op filter at 0x73f81e as before. U3MP_MM_COOPLIST119=0 (rw2+0x405e bit2) leaves the stub out.
 if os.environ.get('U3MP_MM_COOPLIST119','1')!='0':
  site=0x73f813;nxt=0x73f81e;onl=0x73f890
  if bytes(image[site:site+11])!=bytes.fromhex('8a4608488b520884c07472'):raise ValueError('fixer L n022 playlist filter site mismatch')
  at,buf,o=stub_slot(0x13a80)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x13a80')
  w=bytearray(bytes.fromhex('8a4608488b520884c0'))                  # mov al,[rsi+8]; mov rdx,[rdx+8]; test al,al
  def rel(n,target):return struct.pack('<i',target-(at+len(w)+n))
  w+=b'\x0f\x84'+rel(6,onl)                                         # je 0x73f890 (LAN byte clear: no filter, as before)
  w+=b'\x83\x3d'+rel(7,rw2+0x3c50+118*4)+b'\x00'                     # cmp dword [ea118],0
  w+=b'\x0f\x84'+rel(6,nxt)                                         # je 0x73f81e (not offline mode: the LAN co-op filter)
  w+=b'\xff\x05'+rel(6,rw2+0x3c50+119*4)                             # inc dword [ea119]
  w+=b'\xe9'+rel(5,onl)                                              # 0x73f890: PS3 online lookup (no co-op filter)
  assert len(w)<=0x80;buf[o:o+len(w)]=w
  image[site:site+11]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*6
  wrappers.append(dict(site=hex(site),address=hex(at),kind='n022-offline-playlist-lookup',wrapper_hex=w.hex()))
 # n023: territory registry for h008 / h010 (zone floor colour). h010's whole-tree walk never reached a territory (the walker
 #   0x11a4580 stops at the first node with +0x70 == 0), so h008 never acted. Both territory state updates (0x7b5e00 / 0x7b64c0)
 #   start with 'call 0x7b3840(territory)' (0x7b5e2a / 0x7b64e7); both calls go to x-stub 0x13a00, which adds rdi to the 8-entry
 #   table ExtState ea[128..143] (skips duplicates, ea[144]++ per new entry; rax/rcx/rdx are free at a call) and jumps to 0x7b3840.
 #   U3MP_TERR_REG=0 leaves it out (h08_tick then has only the walk, as v6.50).
 if os.environ.get('U3MP_TERR_REG','1')!='0':
  sites=(0x7b5e2a,0x7b64e7);fn=0x7b3840
  for st in sites:
   if image[st]!=0xe8 or st+5+struct.unpack_from('<i',image,st+1)[0]!=fn:raise ValueError('fixer L n023 territory call site mismatch %#x'%st)
  at,buf,o=stub_slot(0x13a00)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x13a00')
  tab=rw2+0x3c50+128*4;cnt=rw2+0x3c50+144*4
  w=bytearray()
  w+=b'\x48\x8d\x05'+struct.pack('<i',tab-(at+7))                   # lea rax,[tab]
  w+=b'\x31\xc9'                                                     # xor ecx,ecx
  L=len(w)
  w+=bytes.fromhex('488b14c8 4839fa'.replace(' ',''))                # mov rdx,[rax+rcx*8]; cmp rdx,rdi
  je1=len(w);w+=b'\x74\x00'                                          # je done
  w+=bytes.fromhex('4885d2')                                         # test rdx,rdx
  jn1=len(w);w+=b'\x75\x00'                                          # jne next
  w+=bytes.fromhex('48893cc8')                                       # mov [rax+rcx*8],rdi
  w+=b'\xff\x05'+struct.pack('<i',cnt-(at+len(w)+6))                 # inc dword [ea144]
  jm1=len(w);w+=b'\xeb\x00'                                          # jmp done
  NX=len(w);w+=bytes.fromhex('ffc1 83f908'.replace(' ',''))          # next: inc ecx; cmp ecx,8
  w+=b'\x72'+struct.pack('<b',L-(len(w)+2))                          # jb L
  DN=len(w);w+=b'\xe9'+struct.pack('<i',fn-(at+len(w)+5))            # done: jmp 0x7b3840
  w[je1+1]=DN-(je1+2);w[jn1+1]=NX-(jn1+2);w[jm1+1]=DN-(jm1+2)
  assert len(w)<=0x80;buf[o:o+len(w)]=w
  for st in sites:struct.pack_into('<i',image,st+1,at-(st+5))
  wrappers.append(dict(site=hex(sites[0]),address=hex(at),kind='n023-territory-registry',wrapper_hex=w.hex()))
 # ===== end fixer L stubs =====
 # agent J j011 (c): in-match Loadout Selection gun image size. Widget type 0x21 (LOADOUT_SELECT, factory case 0x64ab7d ->
 #   ctor 0x60bbc0, vtable 0x1679940, draw 0x60bec0) draws each gun cell with 0x60d540 (long gun 0x60c85e, pistol 0x60ca7b):
 #   type-3 icon, layout 0x640ac0 with the box (w, h) = lanes 2/3 of the cell rect (0x60d7a9 / 0x60d7b3), so the gun fills the
 #   cell. PS3 1.19 f_21c1a8 (0x21c3a0..0x21c440) lays its gun item (type 33) out in a fixed 128 x 64 box ({.., .., 128.0 = lis
 #   0x4300, 64.0 = TOC r30-31708}, f_25a7bc). Measured (ps3vid2 f119 vs the user's v6.18 shot, relative to the panel): PS3 long gun
 #   and pistol are both 0.076 panel high, ours 0.113 (ratio 0.67 for both, widths 0.65-0.67): both guns are height-bound, PS3
 #   at its 64.0 box height (PS4 uses the same 1280x720 UI units; the cell is about 96 high), and PS3's 128.0 is not binding
 #   (its gun is about 149 units wide). So only the box height becomes 64.0; the width stays the cell's. The two vextractps
 #   (20 bytes) become a jump to x-stub 0x17200, which does that unless eb[2] bit3 (U3MP_LOADSEL_GUN119=0) is set (then the
 #   original stores); back at 0x60d7bd (xmm1 is cleared there).
 if os.environ.get('U3MP_LOADSEL_GUN_STUB','1')!='0':
  site=0x60d7a9
  orig=bytes.fromhex('c4e379178d30ffffff02c4e379178d34ffffff03')
  if bytes(image[site:site+20])!=orig:raise ValueError('agent J j011 loadout gun box site mismatch')
  at,buf,o=stub_slot(0x17200)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x17200')
  w=bytearray()
  here=at+len(w);w+=b'\xf6\x05'+struct.pack('<i',(rw2+0x4058)-(here+7))+b'\x08'   # test byte [eb2],8
  w+=b'\x75\x00';jnz=len(w)-1
  w+=orig[:10]                                                              # box width: the cell's, as before
  # agent U2 u001: the box height is PS3's natural gun height, 64 texels (every PS3 gun icon is 256x64 or 128x64; PS4's are
  #   twice that, 512x128 / 256x128) x the layout scale 0.8 (0x640b50; PS3 f_25ce40's global) = 51.2. PS3's in-match control is
  #   f_21e634 / gun cell f_21e288 (type 3, box = cell rect + (2,2,-4,-4), the same as PS4 0x60d540), not f_21c1a8 (that one
  #   is the HUD weapon panel); the PS3 gun is not box-bound, it shows at its natural size. j011's 64.0 stays as the fallback:
  #   byte rw2+0x405d bit0 (U3MP_LOADSEL_GUN_PS3H=0) brings it back.
  w+=bytes.fromhex('c78534ffffff')+struct.pack('<I',0x424ccccd)              # box height: mov dword [rbp-0xcc],51.2
  here=at+len(w);w+=b'\xf6\x05'+struct.pack('<i',(rw2+0x405d)-(here+7))+b'\x01'   # test byte [rw2+0x405d],1
  w+=b'\x74\x0a'                                                             # jz +10 (keep 51.2)
  w+=bytes.fromhex('c78534ffffff')+struct.pack('<I',0x42800000)              # j011: mov dword [rbp-0xcc],64.0
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',(site+20)-(here+5))
  w[jnz]=len(w)-(jnz+1)
  w+=orig
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',(site+20)-(here+5))
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+20]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*15
  wrappers.append(dict(site=hex(site),address=hex(at),kind='j011-loadsel-gun',wrapper_hex=w.hex()))
 # agent U2 u001 (x-stub slot 0x17880; switch byte rw2+0x405d): type-0x21 (weapon id) item natural size, PS3 1.19 1:1.
 #   PS3 f_25ce40 case 33 (0x25d5b8..): def = f_303a20(id) (hud-settings 0x9353ed16 weapon list), size = scale x (def+36, def+40).
 #   PS4 0x640ac0 case 0x21 (0x64102f) reads the same hud-settings entry (PS3 +12 = PS4 +0x14) but computes
 #   (e[+0x14] x e[+0x2c] / e[+0x24]) x scale, i.e. PS3's size x (HUD size / texture size) = x0.70..0.86 (x0.78 for most guns).
 #   The DC data is identical on both (61 common entries). That makes every gun that is not box-bound smaller than PS3:
 #   Competitive Loadouts cells (0x6084b0, box = 0.5*W-8 x 132 as PS3 f_22028c), the selected-weapon panel; list rows are
 #   box-bound on both and stay the same. The 45 bytes 0x64105a..0x641086 (rax = entry; result xmm0 lanes 0/1) jump to the
 #   stub: unless byte rw2+0x405d bit1 (U3MP_GUNICON119=0), xmm0 = (e[+0x2c], e[+0x30]) and on to 0x641087 (the scale
 #   multiply and the box fit, unchanged); else the original 45 bytes. Nothing branches into the range.
 if os.environ.get('U3MP_GUNICON119_STUB','1')!='0':
  site=0x64105a
  orig=bytes.fromhex('c5fa7e4014c5fa7e482cc5f859c1c5fa7e4824c5fa5ed1c4e37904c001c4e37904c901c5fa5ec1c4e36921c010')
  if len(orig)!=0x2d or bytes(image[site:site+0x2d])!=orig:raise ValueError('agent U2 u001 type-0x21 size site mismatch')
  at,buf,o=stub_slot(0x17880)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x17880')
  w=bytearray()
  here=at+len(w);w+=b'\xf6\x05'+struct.pack('<i',(rw2+0x405d)-(here+7))+b'\x02'   # test byte [rw2+0x405d],2
  w+=b'\x75\x00';jnz=len(w)-1
  w+=bytes.fromhex('c5fa7e402c')                                            # vmovq xmm0,[rax+0x2c]  (PS3 def+36/+40)
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',(site+0x2d)-(here+5))
  w[jnz]=len(w)-(jnz+1)
  w+=orig
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',(site+0x2d)-(here+5))
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+0x2d]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\xcc'*0x28
  wrappers.append(dict(site=hex(site),address=hex(at),kind='u001-gun-icon-size',wrapper_hex=w.hex()))
 # agent J j010 (1): HUD sprite colours. 0x11b8dd0 (sprite draw, PS3 f_9936e0; also reached from the hud-settings element
 #   draw 0x11b8af0) entry 'push rbp; mov rbp,rsp; push r15; push r14' (8 bytes, a function entry, nothing jumps into it) ->
 #   value stub kind 229 (slot 0x16f80): the helper may point rcx at a decoded colour copy and answers 0 (run on).
 #   U3MP_HUD_GAMMA=0 sets eb[2] bit0 (the stub stays; the helper then does nothing). U3MP_HUD_GAMMA_STUB=0 leaves it out.
 if os.environ.get('U3MP_HUD_GAMMA_STUB','1')!='0':
  site=0x11b8dd0
  if bytes(image[site:site+8])!=bytes.fromhex('554889e541574156'):raise ValueError('agent J j010 sprite entry mismatch')
  at,w=value_stub(0x16f80,229,lambda h:bytes.fromhex('554889e541574156'),site+8)
  image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*3
  wrappers.append(dict(site=hex(site),address=hex(at),kind=229,wrapper_hex=w.hex()))
 # ===== end agent J stubs / patches =====
 # ===== fixer W (menu visuals): x-stub slots 0x17d00..0x17f00, switch byte rw2+0x405f (eb[3] bits 24..31) + ec[3],
 #   asm-shared words ExtState.ea[240..247] (rw2+0x4010..0x402f; ea[200..211] hold a013's board table, ea[240..] were unused) =====
 # fixer W w001: gun / booster / kickback / mod images too dark in the menus (v6.31: Competitive Loadouts guns median 24,18,6 vs PS3
 #   105,93,48 = the sRGB decode of PS3's raw texel). These items do NOT pass a texture handle: the type-0x21 weapon image and the
 #   type-3 UI icons draw their UI material (hud-settings / net.pak material table entry) with batch {material, 0, 0, 0}, so the
 #   material's OWN textures are bound by the material apply 0xe90150 (called by End_Actual 0xe41a60 at 0xe41d54, ret 0xe41d59).
 #   j010 (2) / l002 only replace batch handles h0..h2 and never see them (l02_d[8..11] = 0 in the v6.31 run). The T# of every
 #   bound texture is copied in two places: the fast resource writer 0xe66940 (0xe66acf: word1 &= 0x3fffff3f, word3 |= 0x4000000,
 #   into a per-draw table) and the slow path 0xe671e0 (0xe67263, onto the stack before 0x12d2b80). Both copies are private, so
 #   the stubs clear num_format (word1 bits 26..29: 9 sRGB -> 0 UNORM) in the COPY when the bind comes from a menu immediate draw:
 #   the rbp chain reaches End_Actual's material apply (ret 0xe41d59) and the first return address above End_Actual outside the
 #   hud-quad helpers 0xe42310..0xe42adf lies in menu code 0x5a0000..0x67ffff (the HUD queue executor 0xe3ffd0 and the HUD code
 #   0xc3..0xc4 never qualify, so the encoding HUD pass keeps sRGB as j010 needs). Gate word ea[240] (helper w01_tick: switch on
 #   and MP active or an MP front page, as l002). Counters: ea[241] fast-path copies made raw, ea[242] slow-path ones, ea[243]
 #   sRGB binds rejected (not a menu draw), ea[244] last return address that decided (image offset).
 #   Nothing branches into 0xe66ad0..0xe66ae4 or 0xe67264..0xe67276; flags are dead at both return points.
 #   U3MP_MENU_SRGB_RAW=0 sets byte rw2+0x405f bit0 (gate off); U3MP_MENU_SRGB_RAW_STUB=0 leaves the stubs out.
 #   w007: the stubs are built by default again for the medal-only HUD mode (U3MP_HUD_MEDAL_SRGB, default on); with
 #   U3MP_HUD_MEDAL_SRGB=0 they stay out unless U3MP_MENU_SRGB_RAW_STUB=1 (the dev default before w007). The stubs walk only when
 #   ea[240] & 0x13 (w001 / w002 / w007 gates); the w003 / w006 bits no longer start a walk.
 if os.environ.get('U3MP_MENU_SRGB_RAW_STUB','1' if os.environ.get('U3MP_HUD_MEDAL_SRGB','1')!='0' else '0')!='0':
  EA=lambda k:rw2+0x3c50+4*k
  def wasm(at,parts):
   # parts: bytes | ('L',name) label | ('j',op,name) rel32 jump/call (op = b'\xe9' / b'\xe8' / b'\x0f\x8X') |
   #        ('s',op8,name) rel8 jump (short Jcc / b'\xeb'; auto-widened to rel32 when out of range) |
   #        ('r',prefix,target,suffix) rip-relative: prefix + rel32(target) + suffix
   LONG={0xeb:b'\xe9'};LONG.update({c:bytes([0x0f,c+0x10]) for c in range(0x70,0x80)})
   wide=set()
   while True:
    lab={};out=bytearray();fx=[]
    for k,p in enumerate(parts):
     if isinstance(p,(bytes,bytearray)):out+=p
     elif p[0]=='L':lab[p[1]]=len(out)
     elif p[0]=='j':out+=p[1];fx.append((len(out),p[2],4,k));out+=bytes(4)
     elif p[0]=='s':
      if k in wide:out+=LONG[p[1][0]];fx.append((len(out),p[2],4,k));out+=bytes(4)
      else:out+=p[1];fx.append((len(out),p[2],1,k));out+=bytes(1)
     elif p[0]=='r':
      out+=p[1];pos=len(out);out+=bytes(4);out+=p[3]
      struct.pack_into('<i',out,pos,p[2]-(at+len(out)))
    grow=False
    for pos,name,n,k in fx:
     t=lab[name] if isinstance(name,str) else name-at
     if n==1:
      if not -128<=t-(pos+1)<128:wide.add(k);grow=True
      else:struct.pack_into('<b',out,pos,t-(pos+1))
     else:struct.pack_into('<i',out,pos,t-(pos+4))
    if not grow:return bytes(out)
  for site,orig in ((0xe66acf,'8b51044421c281490c000000048951044883c120eb52'),(0xe67263,'8b45b4b93fffff3f21c8814dbc000000048945b4')):
   if bytes(image[site:site+len(orig)//2])!=bytes.fromhex(orig):raise ValueError('fixer W w001 T# copy site mismatch '+hex(site))
  # shared walker + action (slots 0x17e00, 0x17e80, 0x17f00; w002 rework): in rax = the bind function's frame, rdx = &T# word1 (the private
  #   copy), rsi = the texture object; keeps rcx/rdi/r8/r9; clobbers rax/rdx/rsi. Class: the rbp chain reaches End_Actual's material
  #   apply (ret 0xe41d59), then up to 16 frames above it: a return to the queue executor's End_Actual call (0xe400a6) or into HUD
  #   code 0xc00000..0xc5ffff = HUD (the encoding pass); else the first return outside the hud-quad helpers 0xe42310..0xe42adf in
  #   menu code 0x5a0000..0x67ffff = menu. Menu (ea[240] bit0): sRGB -> UNORM (w001). HUD (ea[240] bit1): a type-2 object with a
  #   UNORM T# of a format that has an sRGB view (8_8_8_8, BC1..BC3, BC7) -> sRGB (w002), so the encoding pass shows its raw texel.
  atm,bm,om=stub_slot(0x17e00)
  # fixer L n025: the medal gate moved to its own x-stub 0x13980 (it grew past the 0x48 bytes left in 0x17d80's tail)
  MG,_mgb,_mgo=stub_slot(0x13980)
  if any(_mgb[_mgo:_mgo+0x80]):raise ValueError('stub slot in use 0x13980')
  if any(bm[om:om+0x180]):raise ValueError('stub slots in use 0x17e00..0x17f00')   # the walker takes three slots
  # h022 / h022b: HUD-list menu bind (x-stub 0x12b00, called by the walker with rdx = the frame's return address - image base, r8 = &T#
  #   word1 of the private copy). When rdx is the HUD-list executor caller 0xc1aff0 / 0xc1b11f and the per-frame gate ec[1] bit15
  #   (helper h22_tick) is set, the stub itself makes an sRGB copy UNORM (word1 bits 26..29 = 9 -> 0) and returns ZF=1 (walker done).
  #   h022 handed the bind to w001's menu tail instead, which also needs ea[240] bit0 (w001's own gate, default OFF since v6.33), so
  #   nothing was converted. Else ZF=0 with rax / rdx / r8 kept (walker goes on). Counters (h022b): ec[216] HUD-list caller seen,
  #   ec[217] ... with the gate on, ec[218] copies made UNORM, ec[219] gate on but not sRGB, ec[220] last word1 seen with the gate on.
  EC=lambda k:rw2+0x4450+4*k
  H22,_hb,_ho=stub_slot(0x12b00)
  if any(_hb[_ho:_ho+0x80]):raise ValueError('stub slot in use 0x12b00')
  # v7.36 h022c: draw-time gate (x-stub 0x13580, two slots). Called by the 0x12b00 stub once the HUD-list caller matched; answers
  #   ZF=1 = gate on, keeps every register. On: ec[1] bit15 (the tick's condition A), or condition B decided here: bit17 on, MP
  #   active (0x17d5c61), a match running (0x17d1868 == 2) and a loadout page anywhere on the menu page stack (0x17bd080: depth
  #   +0xdb0, u64 pages at +0x8b0, page id +0x10) - no menu state 7/8 filter, so the page's slide-in frames already take the menu path.
  G22,_gb,_go=stub_slot(0x13580)
  if any(_gb[_go:_go+0x100]):raise ValueError('stub slots in use 0x13580..0x13600')
  _g=wasm(G22,[('r',b'\xf6\x05',rw2+0x4455,b'\x80'),('s',b'\x75','on'),('r',b'\xf6\x05',rw2+0x4456,b'\x02'),('s',b'\x74','off'),
      ('r',b'\x80\x3d',0x17d5c61,b'\x00'),('s',b'\x74','off'),('r',b'\x83\x3d',0x17d1868,b'\x02'),('s',b'\x75','off'),
      bytes.fromhex('505152'),('r',b'\x8b\x0d',0x17bd080+0xdb0,b''),('r',b'\x8b\x05',0x17bd080+8,b''),   # pause-flash: eax = menu state
      bytes.fromhex('85c0'),('s',b'\x74','pop'),bytes.fromhex('83f808'),('s',b'\x75','top'),bytes.fromhex('ffc1'),   # state 0 = menu off; state 8: outgoing page at stack[depth+1]
      ('L','top'),bytes.fromhex('81f99f000000'),('s',b'\x77','pop'),bytes.fromhex('ffc1'),
      ('r',b'\x48\x8d\x15',0x17bd080+0x8b0,b''),
      ('L','loop'),bytes.fromhex('488b02 4885c0'.replace(' ','')),('s',b'\x74','next'),bytes.fromhex('8b4010')]+
      sum([[b'\x3d'+struct.pack('<I',pg),('s',b'\x74','found')] for pg in (0xfa6a54c4,0x3192a5ea,0x6d627b29,0x35957e36,0x59d14afb,0x8da23252,0xba7ae3b4,0x554fbe06)],[])+[
      ('L','next'),bytes.fromhex('4883c208 ffc9'.replace(' ','')),('s',b'\x75','loop'),
      ('L','pop'),bytes.fromhex('5a5958'),('L','off'),bytes.fromhex('85e4c3'),
      ('L','found'),bytes.fromhex('5a5958'),('L','on'),bytes.fromhex('39c0c3')])
  assert len(_g)<=0x100,len(_g);_gb[_go:_go+len(_g)]=_g
  wrappers.append(dict(site='-',address=hex(G22),kind='h022c-hudlist-gate',wrapper_hex=_g.hex()))
  # fixer L n024: the h022b counters moved from ec[216..220] (= h008's h08_d[0..4]: both wrote them) to ExtState ea[160..164]
  #   (peek h22=20ebed0:0x14): [0] HUD-list caller seen, [1] ... with the gate on, [2] copies made UNORM, [3] gate on but not sRGB, [4] last word1.
  _h=wasm(H22,[bytes.fromhex('4881faf0afc100'),('s',b'\x74','c'),bytes.fromhex('4881fa1fb1c100'),('s',b'\x75','r'),
      ('L','c'),('r',b'\xff\x05',EA(160),b''),('j',b'\xe8',G22),('s',b'\x75','z'),('r',b'\xff\x05',EA(161),b''),   # h022c: draw-time gate
      bytes.fromhex('52 418b10'.replace(' ','')),('r',b'\x89\x15',EA(164),b''),bytes.fromhex('81e20000003c 81fa00000024 5a'.replace(' ','')),
      ('s',b'\x75','ns'),bytes.fromhex('418120ffffffc3'),('r',b'\xff\x05',EA(162),b''),bytes.fromhex('39c0c3'),
      ('L','ns'),('r',b'\xff\x05',EA(163),b''),bytes.fromhex('39c0c3'),('L','z'),bytes.fromhex('85e4'),('L','r'),b'\xc3'])
  assert len(_h)<=0x80,len(_h);_hb[_ho:_ho+len(_h)]=_h
  wrappers.append(dict(site='-',address=hex(H22),kind='h022-hudlist-menu-check',wrapper_hex=_h.hex()))
  def chk(fail):return [bytes.fromhex('4889c24829e2'),('s',b'\x76',fail),bytes.fromhex('4881fa00000400'),('s',b'\x73',fail),
                        bytes.fromhex('a807'),('s',b'\x75',fail),bytes.fromhex('488b50084829f2488b00')]
  wm=wasm(atm,[bytes.fromhex('51 57 4150 4151 4989d0 4989f1 418b10 81e20000003c 81fa00000024'.replace(' ','')),('s',b'\x74','go'),
     bytes.fromhex('85d2'),('s',b'\x75','r0'),bytes.fromhex('418b11 83e203 83fa02'.replace(' ','')),('s',b'\x75','r0'),
     ('L','go'),('r',b'\x48\x8d\x35',0,b''),bytes.fromhex('31ff b90a000000'.replace(' ','')),
     ('L','l1')]+chk('r0')+[bytes.fromhex('4881fa591de400'),('s',b'\x74','found'),bytes.fromhex('ffc9'),('s',b'\x75','l1'),('s',b'\xeb','r0'),
     ('L','found'),bytes.fromhex('b910000000'),
     ('L','l2')]+chk('end')+[   # h022: the dead 'cmp rdx,0xe400a6 / je hud' (the executor's End_Actual call returns to 0xe400a8) is replaced
     ('j',b'\xe8',H22),('s',b'\x74','r0'),   # h022b: HUD-list caller + gate: the stub converted (or rejected) the copy itself
     bytes.fromhex('4881fa0000c000'),('s',b'\x72','nh'),bytes.fromhex('4881fa0000c600'),('s',b'\x72','hud'),
     ('L','nh'),bytes.fromhex('85ff'),('s',b'\x75','nx'),bytes.fromhex('4881fa1023e400'),('s',b'\x72','dc'),bytes.fromhex('4881fae02ae400'),('s',b'\x72','nx'),
     ('L','dc'),('r',b'\x89\x15',EA(244),b''),bytes.fromhex('bf02000000 4881fa00005a00'.replace(' ','')),('s',b'\x72','nx'),
     bytes.fromhex('4881fa00006800'),('s',b'\x73','nx'),bytes.fromhex('bf01000000'),
     ('L','nx'),bytes.fromhex('ffc9'),('s',b'\x75','l2'),
     ('L','end'),bytes.fromhex('83ff01'),('s',b'\x75','r0'),('r',b'\xf6\x05',EA(240),b'\x01'),('s',b'\x74','r0'),
     bytes.fromhex('418b10 81e20000003c 81fa00000024'.replace(' ','')),('s',b'\x75','r0'),bytes.fromhex('41812000ffffc3'.replace('41812000ffffc3','418120ffffffc3')),
     ('r',b'\xff\x05',EA(241),b''),('s',b'\xeb','r0'),
     ('L','hud'),('r',b'\x89\x15',EA(245),b''),('r',b'\xf6\x05',EA(240),b'\x12'),('s',b'\x74','r0'),
     bytes.fromhex('418b10 f7c20000003c'.replace(' ','')),('s',b'\x75','r0'),
     bytes.fromhex('c1ea14 83e23f 83fa0a'.replace(' ','')),('s',b'\x74','fmt'),bytes.fromhex('83fa23'),('s',b'\x72','r0'),bytes.fromhex('83fa25'),('s',b'\x76','fmt'),
     bytes.fromhex('83fa29'),('s',b'\x75','r0'),
     ('L','fmt'),('j',b'\xe8',MG),('s',b'\x75','r0'),   # w007: ZF = take it (full w002, or a net-medals vtex object)
     ('L','set'),bytes.fromhex('41810800000024'),('r',b'\xff\x05',EA(243),b''),
     ('L','r0'),bytes.fromhex('4159 4158 5f 59 c3'.replace(' ',''))])
  assert len(wm)<=0x180,len(wm);bm[om:om+len(wm)]=wm
  wrappers.append(dict(site='-',address=hex(atm),kind='w001-menu-walker',wrapper_hex=wm.hex()))
  # fast path 0xe66acf (rcx = the T# copy, [r13] = the object, rbp = 0xe66940's frame), back at 0xe66b37
  at,buf,o=stub_slot(0x17d00)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x17d00')
  w=wasm(at,[bytes.fromhex('8b5104 4421c2 81490c00000004 895104 50 52 56'.replace(' ','')),('r',b'\xf6\x05',EA(240),b'\x13'),('s',b'\x74','out'),
     bytes.fromhex('4889e8 488d5104 498b7500'.replace(' ','')),('j',b'\xe8',atm),
     ('L','out'),bytes.fromhex('5e 5a 58 4883c120'.replace(' ','')),('j',b'\xe9',0xe66b37)])
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[0xe66acf:0xe66acf+22]=b'\xe9'+struct.pack('<i',at-(0xe66acf+5))+b'\x90'*17
  wrappers.append(dict(site='0xe66acf',address=hex(at),kind='w001-tsharp-fast',wrapper_hex=w.hex()))
  # slow path 0xe67263 (T# copy at [rbp-0x50], word1 at [rbp-0x4c], [r15] = the object; rbp = 0xe671e0's frame), back at 0xe67277
  at,buf,o=stub_slot(0x17d80)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x17d80')
  w=wasm(at,[bytes.fromhex('8b45b4 b93fffff3f 21c8 814dbc00000004 8945b4'.replace(' ','')),('r',b'\xf6\x05',EA(240),b'\x13'),('s',b'\x74','out'),
     bytes.fromhex('52 56 4889e8 488d55b4 498b37'.replace(' ','')),('j',b'\xe8',atm),bytes.fromhex('5e 5a'.replace(' ','')),
     ('L','out'),('j',b'\xe9',0xe67277)])
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[0xe67263:0xe67263+20]=b'\xe9'+struct.pack('<i',at-(0xe67263+5))+b'\x90'*15
  wrappers.append(dict(site='0xe67263',address=hex(at),kind='w001-tsharp-slow',wrapper_hex=w.hex()))
  # w007 medal gate (called by the walker's HUD branch with rsi = image base, r9 = the texture object; clobbers rax/rcx/rdi;
  #   returns ZF=1 = make the copy sRGB). ea[240] bit1 (full w002): every type-2 object. Else (bit4, medal mode): only an object
  #   held by the vtex cache (manager 0x1b31128: 32 entries of 0x140 at +0x2800, object pointer +0x10, name +0x30, filled by
  #   0x1189430 / 0x11892b0) whose name starts with "net-medals/" (the in-match medal coins: popup 0xc22f70, streaks).
  #   Emblems, ranks, playlists and map images keep the native UNORM view. ea[242] counts the medal objects seen.
  if len(w)>0x38:raise ValueError('w007: slow-path stub grew into the medal gate')
  # fixer L n025: after the medal test, ea[240] bit5 (helper n25_tick: a rank-up popup 0xc2e4ad43 is the current HUD popup of
  #   player 0, switch U3MP_HUD_RANK_SRGB on) also takes a vtex object named "ranks/..." (the popup's rank / prestige badge,
  #   0xc2316e / 0xc23248: "ranks/%i", "ranks/prestige-%i"); ea[165] counts them. Limited to the popup's lifetime so other
  #   in-match rank icons (scoreboard) keep the native view.
  mg=wasm(MG,[('r',b'\xf6\x05',EA(240),b'\x02'),('s',b'\x74','m'),bytes.fromhex('39c0c3'),
     ('L','m'),bytes.fromhex('488dbe'+struct.pack('<I',0x1b31128+0x2800).hex()+'b920000000'),
     ('L','ml'),bytes.fromhex('4c394f10'),('s',b'\x74','mf'),bytes.fromhex('4881c740010000'),('s',b'\xe2','ml'),bytes.fromhex('c3'),
     ('L','mf'),b'\x48\xb8'+b'net-meda'+bytes.fromhex('48394730'),('s',b'\x75','rk'),('r',b'\xff\x05',EA(242),b''),
     bytes.fromhex('817f37')+b'als/',b'\xc3',
     ('L','rk'),('r',b'\xf6\x05',EA(240),b'\x20'),('s',b'\x74','no'),
     bytes.fromhex('817f30')+b'rank',('s',b'\x75','R'),bytes.fromhex('66817f34')+b's/',('s',b'\x75','R'),
     ('r',b'\xff\x05',EA(165),b''),bytes.fromhex('39c0c3'),
     ('L','no'),bytes.fromhex('85e4'),('L','R'),b'\xc3'])
  if len(mg)>0x80:raise ValueError('n025 medal gate too long %d'%len(mg))
  _mgb[_mgo:_mgo+len(mg)]=mg
  wrappers.append(dict(site='-',address=hex(MG),kind='w007-medal-gate',wrapper_hex=mg.hex()))

 # fixer W w003: tab-bar boxes (TABS-MM-BRIEF A). PS4's tab strip (style 32, ctor 0x609cf0, vtable 0x1679880, draw 0x609ff0 = PS3 1.19
 #   f_21f4d8) still lays out and draws PS3's item type 0x2d (the gradient box: one per tab, colour 0x80808080 selected /
 #   0xa0000000 others, plus the 4 px rule under the bar) through 0x640ac0 + 0x63d6f0, but PS4's item draw 0x641560 has no case
 #   0x2d (its switch ends at 0x2a), so nothing shows. PS3 f_25d740 case 0x2d (0x263a7c): texture 0x6e9d9193 (class 0), quad at
 #   (pos, item w/h), colour = draw colour x item colour, uv corners rotated by the item's flags & 3. Value stub at the entry of
 #   0x63d6f0 ('push rbp; mov rbp,rsp; push r15; push r14', 8 bytes, function entry, nothing jumps inside) -> slot 0x17a00, kind 246:
 #   for a type-0x2d item the helper does 0x63d6f0's placement / shadow and the PS3 quad itself and returns; every other item
 #   runs the native. U3MP_TAB_BOX119_STUB=0 leaves the stub out.
 if os.environ.get('U3MP_TAB_BOX119_STUB','1')!='0':
  site=0x63d6f0
  if bytes(image[site:site+8])!=bytes.fromhex('554889e541574156'):raise ValueError('fixer W w003 site mismatch 0x63d6f0')
  at,w=value_stub(0x17a00,246,lambda h:bytes.fromhex('554889e541574156'),site+8)
  image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*3
  wrappers.append(dict(site=hex(site),address=hex(at),kind=246,wrapper_hex=w.hex()))

 # fixer H h009: item types PS4's item draw 0x641560 has no case for (0x24 map image, 0x2f "new", 0x30 badge + new, 0x31 check + new;
 #   see the h009 block in u3_lan_profiles.c). Value stub at the entry of the item layout 0x640ac0 ('push rbp; mov rbp,rsp; push r15;
 #   push r14', 8 bytes, function entry, nothing jumps inside) -> x-stub 0x17980, kind 245, behind an asm pre-filter: an item whose
 #   type ([rsi]) is below 0x24 or above 0x31 replays the 8 bytes at once (0x640ac0 reads [rsi] itself at 0x640b20). The helper
 #   rewrites the item in place before 0x640ac0 copies it. U3MP_NEW_ICON119_STUB=0 leaves the stub out.
 if os.environ.get('U3MP_NEW_ICON119_STUB','1')!='0':
  site=0x640ac0
  if bytes(image[site:site+8])!=bytes.fromhex('554889e541574156'):raise ValueError('fixer H h009 site mismatch 0x640ac0')
  at,buf,o=stub_slot(0x17980)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x17980')
  w=bytearray.fromhex('833e24 7200 833e31 7700')                # cmp dword [rsi],0x24; jb native; cmp dword [rsi],0x31; ja native
  w.extend(bytes.fromhex('554889e5 50515256574150415141524153 4883e4f0 4881ec00020000 480fae0424'))
  here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
  w.extend(bytes.fromhex('488d75b8'))                            # lea rsi,[rbp-0x48]
  w.extend(b'\xba'+struct.pack('<I',245))
  here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
  w.extend(bytes.fromhex('480fae0c24 488d65b8 488945b8'))
  w.extend(bytes.fromhex('415b415a41594158 5f5e5a5958 5d'))
  w.extend(bytes.fromhex('4d85db 7401 c3'))
  nat=len(w);w[4]=nat-5;w[9]=nat-10
  w.extend(bytes.fromhex('554889e541574156'))
  here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',(site+8)-(here+5)))
  assert len(w)<=0x80 and nat-5<0x80,len(w)
  buf[o:o+len(w)]=w
  image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*3
  wrappers.append(dict(site=hex(site),address=hex(at),kind=245,wrapper_hex=w.hex()))

 # fixer W w006: confirmation dialogs too big (V633 D). The dialog widget's layout 0x5e0740 (= PS3 1.19 f_20c7a4) lays the icon item
 #   (type 3, at rbp-0x120: +0 type, +0x14 texture sid) out through 0x640ac0 at texture size x 0.8 x draw scale ([r12+8]); the result
 #   w / h land at rbp-0x64 / rbp-0x60 (out rbp-0xa0 +0x3c / +0x40). PS4's hud.pak icons are 256x256 where PS3's are 128x128, so
 #   the icon (and the title x, box height) came out twice PS3's. Site 0x5e07d0 'call 0x640ac0; vmovss xmm4,[rbp-0x64]' (10 bytes,
 #   straight-line, nothing jumps inside; xmm1/xmm2/eax and the flags are dead there: rax is reloaded at 0x5e07df) -> pure asm
 #   x-stub slot 0x17f80: the call, then with ea[240] bit3 set, item type 3 and w > 120 x scale (a texture wider than 150 px), the
 #   w and h are halved (exponent - 1) and ea[246]++ / ea[247] = w before; then the vmovss and back to 0x5e07da.
 #   U3MP_DIALOG119_STUB=0 leaves the stub out; U3MP_DIALOG119=0 (byte rw2+0x405f bit4) keeps it inert.
 if os.environ.get('U3MP_DIALOG119_STUB','1')!='0':
  site=0x5e07d0
  if bytes(image[site:site+10])!=b'\xe8'+struct.pack('<i',0x640ac0-(site+5))+bytes.fromhex('c5fa10659c'):raise ValueError('fixer W w006 site mismatch 0x5e07d0')
  at,buf,o=stub_slot(0x17f80)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x17f80')
  EAW=lambda k:rw2+0x3c50+4*k
  w=bytearray()
  def w06rip(op,target,tail=b''):
   end_=at+len(w)+len(op)+4+len(tail);w.extend(op+struct.pack('<i',target-end_)+tail)
  w06rip(b'\xe8',0x640ac0)                                         # call 0x640ac0 (the displaced call)
  w06rip(b'\xf6\x05',EAW(240),b'\x08')                             # test byte [ea240],8
  j1=len(w);w.extend(b'\x74\x00')                               # jz out
  w.extend(bytes.fromhex('83bde0feffff03'))                      # cmp dword [rbp-0x120],3
  j2=len(w);w.extend(b'\x75\x00')                               # jne out
  w.extend(bytes.fromhex('b8 0000f042 c5f96ec8 c4c172594c2408'.replace(' ','')))  # mov eax,120.0; vmovd xmm1,eax; vmulss xmm1,xmm1,[r12+8]
  w.extend(bytes.fromhex('c5fa10559c c5f82ed1'.replace(' ','')))  # vmovss xmm2,[rbp-0x64]; vucomiss xmm2,xmm1
  j3=len(w);w.extend(b'\x76\x00')                               # jbe out (also NaN)
  w.extend(bytes.fromhex('8b459c'))                              # mov eax,[rbp-0x64]
  w06rip(b'\x89\x05',EAW(247))                                     # mov [ea247],eax
  w.extend(bytes.fromhex('2d00008000 89459c'.replace(' ','')))    # sub eax,0x800000; mov [rbp-0x64],eax
  w.extend(bytes.fromhex('8b45a0 85c0'.replace(' ','')))          # mov eax,[rbp-0x60]; test eax,eax
  j4=len(w);w.extend(b'\x7e\x00')                               # jle skip (0 / negative h left alone)
  w.extend(bytes.fromhex('2d00008000 8945a0'.replace(' ','')))    # sub eax,0x800000; mov [rbp-0x60],eax
  w[j4+1]=len(w)-(j4+2)
  w06rip(b'\xff\x05',EAW(246))                                     # inc dword [ea246]
  for j in (j1,j2,j3):w[j+1]=len(w)-(j+2)
  w.extend(bytes.fromhex('c5fa10659c'))                          # out: vmovss xmm4,[rbp-0x64]
  w06rip(b'\xe9',0x5e07da)
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+10]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*5
  wrappers.append(dict(site=hex(site),address=hex(at),kind='w006-dialog-icon',wrapper_hex=bytes(w).hex()))

 # fixer W w009: Co-op Adventure story heroes (PS3 1.19 1:1). The net game table (DC 0x3b9a067d, 0xd0 entries) holds for every
 #   Co-op Adventure game (type 11) three look descs at +0x70 (PS3 1.19 +0x74 of 0xe4), one per co-op slot; byte 0 = character type
 #   (Borneo drake-french / sullivan / chloe, London drake / coop-cutter / sullivan, Monastery sullivan / elena / chloe, Syria drake /
 #   sullivan / elena, Airport drake / sullivan / coop-cutter). Match setup 0x727440 (PS3 0x3158xx) gives each active net player its
 #   slot (+0x42c) and copies desc[slot] into its hero look +0x620..+0x647 (PS3 +1168); the spawn (0x787a00) and PS3's online lobby
 #   placement (0x5b6640 -> 0x784b20) use that look. Our LAN placement takes the LAN branch (custom manager look = the profile's own
 #   hero), and the LAN flow may not leave the copy in place. Two fullsave stubs -> helper kind 248 (w09_kind):
 #   (1) 0x5b69aa 'lea rsi,[rbp-0x58]; call 0x996a60' (9 bytes; only jump target is 0x5b69aa itself): arg = caller rbp | 1 -> in a
 #       type-11 game the placement desc [rbp-0x58] becomes desc[slot of the local player];
 #   (2) 0x7887df 'lea rax,[r13+0x620]; jmp 0x788805' (9 bytes, the co-op branch of the spawn look pick, no references): arg = r13
 #       (the net player) -> in a type-11 game player+0x620 = desc[player+0x42c] (what 0x727440 writes) before the spawn reads it.
 #   Arena / Hunter / Shade have no descs and keep the player's own hero, as on PS3. U3MP_COOP_HERO119=0 sets byte rw2+0x405f bit7
 #   (stubs stay, the helper does nothing); U3MP_COOP_HERO119_STUB=0 leaves the stubs out. Slots 0x12c00 / 0x12c80.
 if os.environ.get('U3MP_COOP_HERO119_STUB','1')!='0':
  site=0x5b69aa
  if bytes(image[site:site+9])!=bytes.fromhex('488d75a8e8ad003e00'):raise ValueError('fixer W w009 lobby site mismatch 0x5b69aa')
  # v7.57 lg01: helper kind 248 decides; nonzero = 'look not ready' (al = 0) without calling 0x996a60 (r11 carries the decision)
  at,buf,o=stub_slot(0x12c00);w=bytearray.fromhex('554889e5 50515256574150415141524153 4883e4f0 4881ec00020000 480fae0424')
  here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
  w.extend(bytes.fromhex('488b75004883ce01'))                                      # rsi = caller rbp | 1 (w009 / lg01)
  w.extend(b'\xba'+struct.pack('<I',248))
  here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
  w.extend(bytes.fromhex('480fae0c24 488d65b8 488945b8'))                         # fxrstor; decision -> r11 slot
  w.extend(bytes.fromhex('415b415a41594158 5f5e5a5958 5d'))
  w.extend(bytes.fromhex('4d85db 7407 31c0'))                                     # test r11,r11; jz replay; xor eax,eax
  here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',(site+9)-(here+5)))           # held: back to 0x5b69b3 with al = 0
  here=at+len(w);w.extend(bytes.fromhex('488d75a8')+b'\xe8'+struct.pack('<i',0x996a60-(here+4+5)))
  here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',(site+9)-(here+5)))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x12c00')
  buf[o:o+len(w)]=w
  image[site:site+9]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*4
  wrappers.append(dict(site=hex(site),address=hex(at),kind=248,wrapper_hex=w.hex()))
  site=0x7887df
  if bytes(image[site:site+9])!=bytes.fromhex('498d8520060000eb1d'):raise ValueError('fixer W w009 spawn site mismatch 0x7887df')
  at,w=fullsave_stub(0x12c80,bytes.fromhex('4c89ee'),248,lambda h:bytes.fromhex('498d8520060000'),0x788805)
  image[site:site+9]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*4
  wrappers.append(dict(site=hex(site),address=hex(at),kind=248,wrapper_hex=w.hex()))

 # fixer W w010: emblem editor rows at PS3 width. The value-rect function 0x5df2a0 (row value box; the measure 0x5df5f0 adds it to the
 #   label) takes the value width as frac * W in PS3 (frac 0.4 / 0.25 / 0.75 by flags, W = menu.bin +0x5a or the row width), but
 #   PS4's 0x5df304 'vxorps xmm0,xmm0,xmm0; vcvtsi2ss xmm0,xmm0,eax' (8 bytes; the only jump in is to 0x5df30e) overwrites frac with
 #   +0x5a: the emblem sliders (+0x5a 340) measure 214 + 340 = 554 and widen PAGE_EMBLEM to 554 (PS3 350). x-stub 0x12d00: for the
 #   slider class only (object vtable 0x1678e30, rbx) and unless byte rw2+0x445c bit0 (U3MP_EMBLEM_LAYOUT119=0, ec[3] bit0):
 #   'vcvtsi2ss xmm1,xmm1,eax; vmulss xmm0,xmm0,xmm1' (xmm1 and rcx are dead until 0x5df386 / 0x5df3a7); else the 8 bytes.
 #   Both back to 0x5df313. U3MP_EMBLEM_LAYOUT119_STUB=0 leaves the stub out (the helper's track part stays, see w10_track).
 if os.environ.get('U3MP_EMBLEM_LAYOUT119_STUB','1')!='0':
  site=0x5df304
  if bytes(image[site:site+10])!=bytes.fromhex('c5f857c0c5fa2ac0eb05'):raise ValueError('fixer W w010 site mismatch 0x5df304')
  at,buf,o=stub_slot(0x12d00)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x12d00')
  w=bytearray()
  here=at+len(w);w+=b'\xf6\x05'+struct.pack('<i',(rw2+0x445c)-(here+7))+b'\x01'   # test byte [ec3 byte0],1
  w+=b'\x75\x00';j_orig=len(w)-1
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',0x1678e30-(here+7))       # lea rcx,[vtable 0x1678e30]
  w+=bytes.fromhex('48390b')                                                       # cmp [rbx],rcx
  w+=b'\x75\x00';j_orig2=len(w)-1
  w+=bytes.fromhex('c5f22ac8 c5fa59c1')                                            # vcvtsi2ss xmm1,xmm1,eax; vmulss xmm0,xmm0,xmm1
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x5df313-(here+5))
  w[j_orig]=len(w)-(j_orig+1);w[j_orig2]=len(w)-(j_orig2+1)
  w+=bytes.fromhex('c5f857c0 c5fa2ac0')                                            # orig: the 8 bytes
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',0x5df313-(here+5))
  if len(w)>0x80:raise ValueError('w010 stub too long')
  buf[o:o+len(w)]=w
  image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*3
  wrappers.append(dict(site=hex(site),address=hex(at),kind='w010-emblem-row',wrapper_hex=bytes(w).hex()))

 # ===== agent L stubs (x-stub slot 0x17280; kind 230; switch byte rw2+0x405a, see the agent L l001 region of u3_lan_profiles.c) =====
 # agent L l001 (1): unlock evaluator 0x7bb5b0 (= PS3 1.19 f_39b238; c012's cond-6 stub sits at 0x7bb708 inside it). Its entry
 #   'push rbp; mov rbp,rsp; push r15' (6 bytes, a function entry, nothing jumps into it) -> slot 0x17280+0: unless byte
 #   rw2+0x405a bit0 is set, 'mov al,1; ret' (every unlock condition passes), except for the new-item marker callers
 #   (return address in 0x7bd0b0..0x7bde8f: notify 0x7bd0b0, is-new 0x7bd6f0, mark-seen 0x7bda50, has-new 0x7bdc70), which get
 #   0 (no "!" flood, no seen bits written) unless bit5 (U3MP_UNLOCK_NEW=1) is set; bit0 set: the 6 bytes and back to 0x7bb5b6.
 #   U3MP_UNLOCK_ALL=0 sets the bit and leaves the stub out.
 l01_slot_used=False
 if os.environ.get('U3MP_UNLOCK_ALL','1')!='0':
  site=0x7bb5b0
  if bytes(image[site:site+12])!=bytes.fromhex('554889e54157415641554154'):raise ValueError('agent L l001 evaluator entry mismatch')
  at,buf,o=stub_slot(0x17280)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x17280')
  l01_slot_used=True
  w=bytearray()
  here=at+len(w);w+=b'\xf6\x05'+struct.pack('<i',(rw2+0x405a)-(here+7))+b'\x01'   # test byte [eb2+2],1 (U3MP_UNLOCK_ALL=0)
  w+=b'\x75\x00';j_orig=len(w)-1
  here=at+len(w);w+=b'\xf6\x05'+struct.pack('<i',(rw2+0x405a)-(here+7))+b'\x20'   # test byte [eb2+2],0x20 (U3MP_UNLOCK_NEW=1)
  w+=b'\x75\x00';j_one=len(w)-1
  w+=bytes.fromhex('488b0424')                                                     # mov rax,[rsp] (return address)
  here=at+len(w);w+=b'\x4c\x8d\x1d'+struct.pack('<i',0x7bd0b0-(here+7))          # lea r11,[0x7bd0b0]
  w+=bytes.fromhex('4c29d8 483de00d0000')                                          # sub rax,r11; cmp rax,0xde0
  w+=b'\x72\x00';j_zero=len(w)-1
  w[j_one]=len(w)-(j_one+1);w+=bytes.fromhex('b001c3')                             # one: mov al,1; ret
  w[j_zero]=len(w)-(j_zero+1);w+=bytes.fromhex('31c0c3')                           # zero: xor eax,eax; ret
  w[j_orig]=len(w)-(j_orig+1);w+=bytes.fromhex('554889e54157')                     # orig: the 6 entry bytes
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',(site+6)-(here+5))
  assert len(w)<=0x40,len(w);buf[o:o+len(w)]=w
  image[site:site+6]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind='l001-unlock-all',wrapper_hex=w.hex()))
 # agent L l001 (2): menu rank badge layers. 0x64228e 'call 0x63dc40' (the type-2 texture icon quad in the item draw 0x641560;
 #   the only call of 0x63dc40) -> 'call' slot 0x17280+0x40, which passes {rdi, rsi, rdx, rcx, r10 (item layout), r8 (colour),
 #   rbp of 0x641560} to kind 230; the helper issues the quad call itself (plus the PS3 badge layers for tagged rank badges) and
 #   the stub returns to 0x642293. It replaces a call, so the volatile registers are free. U3MP_BADGE_LAYERS=0 sets byte
 #   rw2+0x405a bit4 and leaves the hook out.
 if os.environ.get('U3MP_BADGE_LAYERS','1')!='0':
  site=0x64228e
  if bytes(image[site:site+5])!=b'\xe8'+struct.pack('<i',0x63dc40-(site+5)):raise ValueError('agent L l001 quad call mismatch')
  at,buf,o=stub_slot(0x17280)
  if not l01_slot_used and any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x17280')
  if any(buf[o+0x40:o+0x80]):raise ValueError('stub slot in use 0x172c0')
  at+=0x40;o+=0x40
  w=bytearray.fromhex('554889e5 50 ff7500 4150 4152 51 52 56 57'.replace(' ',''))   # push rbp; mov rbp,rsp; ev[7..0] = rax, [rbp], r8, r10, rcx, rdx, rsi, rdi
  here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))           # rdi = image base
  w+=bytes.fromhex('4889e6')+b'\xba'+struct.pack('<I',230)                   # rsi = ev (rsp is 16-aligned here); edx = kind
  here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
  w+=bytes.fromhex('c9c3')                                                    # leave; ret
  assert len(w)<=0x40,len(w);buf[o:o+len(w)]=w
  image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=230,wrapper_hex=w.hex()))
 # agent L l002: UI immediate draw 0xe40f00 entry 'push rbp; mov rbp,rsp; push r15; push r14' (8 bytes, function entry, nothing
 #   jumps into it) -> value stub slot 0x17800, kind 231: menu draws get UNORM copies of sRGB type-3 handles in a copied batch
 #   (rsi), HUD / queued draws are left alone; the helper answers 0 (run on). U3MP_MENU_UNORM119=0 sets byte rw2+0x405a bit7 and
 #   leaves the stub out.
 if os.environ.get('U3MP_MENU_UNORM119','1')!='0':
  site=0xe40f00
  if bytes(image[site:site+8])!=bytes.fromhex('554889e541574156'):raise ValueError('agent L l002 0xe40f00 entry mismatch')
  at,w=value_stub(0x17800,231,lambda h:bytes.fromhex('554889e541574156'),site+8)
  image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*3
  wrappers.append(dict(site=hex(site),address=hex(at),kind=231,wrapper_hex=w.hex()))
 # ===== end agent L stubs =====
 # ===== agent S stubs (x-stub slots 0x17500..0x17780, kinds 233..239, switch byte rw2+0x405b = eb[2] bits 24..31) =====
 # agent S s014: 16-byte minimum alignment for the network heap (kAllocNetwork 0x14 / kAllocNetworkProcess 0x15).
 #   v6.19s fault 'read access to ffffffffffffffff at 0x6c0224' at match start (net state 29): 0x6c0224 is 'vmovaps [rbx+0x40]'
 #   in the NetEvent ctor 0x6c01a0 (0x90 bytes, vtable 0x16d3630), reached from the script native client-net-go 0x79bf10 ->
 #   0x6c02e0 -> local apply 0x6d0f60 -> 0x6d115d. The ctor allocates with 0xd9e990(0x90, &0x14, align 8) and rbx was
 #   0x103744baa8 (8-aligned), so vmovaps raised #GP. PS4 never registered heaps 0x14 / 0x15; s010 backs them with the
 #   wrapped-malloc heap (alloc[9], class 0x12e7310, vtable 0x16c0930), whose alloc slot vt+0x48 = 0x12e77d0 honours the
 #   requested alignment literally (0xd9c350), so about every other block is only 8-aligned. The MP code Bluepoint compiled
 #   from the PS3 source passes align 8 but its objects carry 16-byte vector members (PS3's allocators never returned less
 #   than 16). Every allocation path (0xd9e990 / 0xd9eae0 / the inlined ones) calls vt+0x48 with the alignment in edx.
 #   Site: 0x12e77d0 'push rbp; mov rbp,rsp; push r15; push r14' (8 bytes, function entry; only the two vtables 0x16c0978 /
 #   0x16b37d8 and the call at 0xdceb1b reach it, nothing jumps inside) -> slot 0x17500: edx = max(edx, 16), the 8 bytes, back to
 #   0x12e77d8. Applies to every wrapped-malloc allocation (heap 9 too); the cost is at most 8 bytes per block that asked for
 #   less than 16. U3MP_NET_HEAP_ALIGN16=0 leaves it out (and sets byte rw2+0x405b bit0).
 if os.environ.get('U3MP_NET_HEAP_ALIGN16','1')!='0':
  site=0x12e77d0;orig=bytes.fromhex('554889e541574156')
  if bytes(image[site:site+8])!=orig:raise ValueError('agent S s014 wrapped-malloc alloc entry mismatch')
  at,buf,o=stub_slot(0x17500)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x17500')
  w=bytearray()
  w+=bytes.fromhex('83fa10')                                                   # cmp edx,16
  w+=b'\x73\x05'                                                               # jae orig
  w+=b'\xba'+struct.pack('<I',16)                                              # mov edx,16
  w+=orig                                                                      # orig: the 8 entry bytes
  here=at+len(w);w+=b'\xe9'+struct.pack('<i',(site+8)-(here+5))                # jmp 0x12e77d8
  w+=b'\xcc'*3+bytes.fromhex('5333')                                           # tag (int3 x3, 'S3'), never reached
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*3
  wrappers.append(dict(site=hex(site),address=hex(at),kind='s014-net-heap-align16',wrapper_hex=w.hex()))
 # agent S s016: lobby character preview while browsing the Character / Customise lists. 0x995d50 (= PS3 f_439eac, the
 #   custom manager's preview refresh, called by the list ops 0x7564ff / 0x756fdf / 0x757954, the PAGE_BODY_TYPE page callback
 #   0x66f4e1 / 0x66f5e6 and the placement) entry 'push rbp; mov rbp,rsp; push r15; push r14' (8 bytes, a function entry;
 #   nothing jumps inside) -> value stub kind 233 (slot 0x17580): the helper stores the look into the Player preview and,
 #   for the same model, calls its vt+0x4f8 (f_439eac's vt+628, missing on PS4) and answers 1; otherwise 0 (the native's
 #   model test and kill run). U3MP_PREVIEW119=0 sets byte rw2+0x405b bit1 (the stub stays and runs the native);
 #   U3MP_PREVIEW119_STUB=0 leaves it out.
 if os.environ.get('U3MP_PREVIEW119_STUB','1')!='0':
  site=0x995d50
  if bytes(image[site:site+8])!=bytes.fromhex('554889e541574156'):raise ValueError('agent S s016 0x995d50 entry mismatch')
  at,w=value_stub(0x17580,233,lambda h:bytes.fromhex('554889e541574156'),site+8)
  image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*3
  wrappers.append(dict(site=hex(site),address=hex(at),kind=233,wrapper_hex=w.hex()))
 # agent S s018: PS4 GetDesc 0x997a40 (= PS3 f_437f20, player-custom-manager.cpp:1203) asserts 'type < 64' (0x997ae6 'cmp r13d,0x40;
 #   jl 0x997b21', assert + int 0x41 at 0x997b1f). PS3 f_437f20 has no such check, and the 1.19 character table DC 0xbd3bb3b9 our
 #   net.bin carries has 79 entries, so highlighting a character with index 64..78 in the Character list (op 10 -> 0x997230 ->
 #   0x9972e5) trapped (user run baseline-1793288658831848900, v6.25). The type is stored as a byte and only indexes that 79-entry
 #   table (entries of 0x70). The jl becomes a jmp (byte 0x997aea 7c -> eb). U3MP_CHAR_TYPE64=0 keeps the assert (byte rw2+0x405b bit3).
 if os.environ.get('U3MP_CHAR_TYPE64','1')!='0':
  if bytes(image[0x997ae6:0x997aec])!=bytes.fromhex('4183fd407c35'):raise ValueError('agent S s018 GetDesc bound mismatch')
  image[0x997aea]=0xeb
 # ===== end agent S stubs =====
 # ===== agent F4 stubs (x-stub slots 0x17600..0x17780, kinds 234..239, switch byte rw2+0x405c = eb[3] bits 0..7) =====
 # f4001 (Fort runtime visibility): diagnostics and experiments, left out unless U3MP_F4_DIAG=1 or one of the U3MP_F4_* experiments
 #   is set (each 0xea1680 / 0xe99530 call then goes through the helper; a diagnostic build only).
 #   0xea1680 (background instance draw: IsVisible 0xea1290, LOD gate, per-submesh frustum, fade, then 0xe99530 per submesh) entry
 #   'push rbp; mov rbp,rsp; push r15; push r14' (8 bytes, a function entry, nothing jumps inside) -> value stub kind 234 (slot 0x17600).
 #   0xe99530 (submesh draw) entry, the same 8 bytes -> value stub kind 235 (slot 0x17680). Both always run the native afterwards.
 F4X=('U3MP_F4_PVSOFF','U3MP_F4_FRUSTOFF','U3MP_F4_LODOFF','U3MP_F4_FADEOFF','U3MP_F4_SHADERFB','U3MP_F4_DRAWALL')
 if os.environ.get('U3MP_F4_DIAG','0')=='1' or any(os.environ.get(k,'0')=='1' for k in F4X):
  for site,slot,kind in ((0xea1680,0x17600,234),(0xe99530,0x17680,235)):
   if bytes(image[site:site+8])!=bytes.fromhex('554889e541574156'):raise ValueError('agent F4 f4001 entry mismatch '+hex(site))
   at,w=value_stub(slot,kind,lambda h:bytes.fromhex('554889e541574156'),site+8)
   image[site:site+8]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*3
   wrappers.append(dict(site=hex(site),address=hex(at),kind=kind,wrapper_hex=w.hex()))
 # f4002 (default ON): fade record safety net. The instance builder 0xe9a490 gives an instance a fade record (inst+0x108) when its
 #   prototype has flags[1] (far fade) or flags[13]; for flags[1] it reads {fadeStart, fadeEnd} from the pointer at prototype +0x48
 #   (0xe9b3ce). With that pointer NULL it uses start 0.0 / end 1.0 (0x144b1e0); 0xea1680 / 0xea2770 scale the end by the
 #   fade-distance scale (0x18c6668, 0.58 on the PC port) and skip every submesh whose nearest point is farther (pass index >= 3).
 #   Converted paks set flags[1] (PS3 header byte1 0x04) but left +0x48 NULL (agent F3), so ~95 % of the Fort stage and ~80-90 % of
 #   most converted levels vanished beyond 0.6 m. Retail always points +0x48 at a 16-byte record. Fix: at 0xe9b3d5 ('je 0xe9b4d5',
 #   the NULL case) jump to x-stub 0x17700: unless byte rw2+0x405c bit6 is set, clear the record's far-fade byte (rec+1 = 0, the
 #   state the builder gives a prototype without flags[1]) and continue at 0xe9b3f6 (rec+3 = 1, then the flags[13] part); otherwise
 #   the original NULL path. Prototypes with a real +0x48 record (retail, rebuilt paks) are untouched.
 #   U3MP_FADE48_FIX=0 sets bit6 (original behaviour at runtime); U3MP_FADE48_FIX_STUB=0 leaves the patch out.
 if os.environ.get('U3MP_FADE48_FIX_STUB','1')!='0':
  site=0xe9b3d5
  if bytes(image[site:site+6])!=bytes.fromhex('0f84fa000000'):raise ValueError('agent F4 f4002 0xe9b3d5 mismatch')
  at,buf,o=stub_slot(0x17700)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x17700')
  w=bytearray()
  here=at+len(w);w.extend(b'\xf6\x05'+struct.pack('<i',(rw2+0x405c)-(here+7))+b'\x40')   # test byte [rw2+0x405c],0x40
  w.extend(b'\x75\x09')                                                                # jnz orig
  w.extend(bytes.fromhex('c6400100'))                                                    # mov byte [rax+1],0
  here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',0xe9b3f6-(here+5)))                    # jmp 0xe9b3f6
  here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',0xe9b4d5-(here+5)))                    # orig: jmp 0xe9b4d5
  assert len(w)<=0x80
  buf[o:o+len(w)]=w
  image[site:site+6]=b'\x0f\x84'+struct.pack('<i',at-(site+6))
  wrappers.append(dict(site=hex(site),address=hex(at),kind='f4002-fade48',wrapper_hex=w.hex()))
 # ===== end agent F4 stubs =====
 # ===== end agent B =====
 # ===== TEST t009: HUD marker progress ring (PS3 1.19 f_667368) at the two traps PS4 left in the marker draw 0xc28ce0 =====
 #   site 1 0xc2a21a 'int 0x41; jmp 0xc2a4b1' (7 bytes, reached only with progress >= 0) -> x-stub 0x17000, kind 200, back 0xc2a4b1
 #   site 2 0xc2a2e3 'vucomiss xmm0,[0x14493e0]; jb 0xc2a2ef; int 0x41' (12 bytes) -> x-stub 0x17080, kind 201, back 0xc2a2ef
 #     (the helper tests progress itself; xmm0 and the flags are dead at 0xc2a2ef: xmm0 is reloaded at 0xc2a304).
 #   Nothing jumps into either range except the jb itself. The stub saves the volatile registers and all xmm (fxsave), passes
 #   rsi = {r12, r15, rbp of 0xc28ce0} and ignores the helper's answer. U3MP_HUD_RING119=0 leaves both traps in place.
 if os.environ.get('U3MP_HUD_RING119','1')!='0':
  for site,orig,off,kind,back in ((0xc2a21a,'cd41e990020000',0x17000,200,0xc2a4b1),(0xc2a2e3,'c5f82e05f5f081007202cd41',0x17080,201,0xc2a2ef)):
   if image[site:site+len(orig)//2]!=bytes.fromhex(orig):raise ValueError('t009 site mismatch '+hex(site))
   at,buf,o=stub_slot(off)
   if any(buf[o:o+0x80]):raise ValueError('stub slot in use '+hex(off))
   w=bytearray.fromhex('554889e5 50515256574150415141524153 4883e4f0 4881ec00020000 480fae0424')
   w+=bytes.fromhex('4883ec20 4c892424 4c897c2408 488b4500 4889442410')   # sub rsp,0x20; [rsp]=r12; [rsp+8]=r15; [rsp+0x10]=[rbp]
   here=at+len(w);w+=b'\x48\x8d\x3d'+struct.pack('<i',-(here+7))         # rdi = image base
   w+=bytes.fromhex('4889e6')+b'\xba'+struct.pack('<I',kind)              # rsi = rsp; edx = kind
   here=at+len(w);w+=b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7))
   here=at+len(w);w+=b'\xe8'+struct.pack('<i',helper-(here+5))
   w+=bytes.fromhex('4883c420 480fae0c24 488d65b8 415b415a41594158 5f5e5a5958 5d')
   here=at+len(w);w+=b'\xe9'+struct.pack('<i',back-(here+5))
   assert len(w)<=0x80,len(w)
   buf[o:o+len(w)]=w
   n=len(orig)//2;image[site:site+n]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*(n-5)
   wrappers.append(dict(site=hex(site),address=hex(at),kind=kind,wrapper_hex=w.hex()))
 # ===== end TEST t009 =====
 # v5.47 Advanced Settings stage 2 (1.19 game-side readers of the custom block; values in ExtState.adv at rw2+0x31f0:
 # +0 selection+1 (0 = never initialised -> original behaviour), +4 f32 respawn time, +8 f32 health multiplier).
 # U3MP_ADV_RULES=0 leaves both sites out (U3MP_ADV=0 also makes them inert: the block then stays uninitialised).
 if os.environ.get('U3MP_ADV_RULES','1')!='0':
  ADV=rw2+0x31f0
  # (1) Respawn time. 1.19 GetRespawnTime f_319ab8: debug override (g+168 > 0) first, then the custom respawn time
  # (netmgr+0x5550) when > 0, then the net game record path. PS4 0x742550 (= 1.10 f_30ee70) lacks the custom step.
  # Entry stub (push rbp; mov rbp,rsp; push r15 = 6 bytes replayed; xmm0/xmm1 are free at entry, xmm0 is the result):
  # block initialised, no override (0x17d7a50+0xa8 <= 0) and respawn > 0 -> return it; otherwise run the original.
  site=0x742550
  if image[site:site+6]!=bytes.fromhex('554889e54157'):raise ValueError('respawn time entry mismatch')
  at,buf,o=stub_slot(int(os.environ.get('U3MP_RTS','0x10800'),16))
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use respawn')
  w=bytearray()
  def rel(n,tgt):return struct.pack('<i',tgt-(at+len(w)+n))
  w+=b'\x83\x3d'+rel(7,ADV)+b'\x00'                      # cmp dword [sel1],0
  w+=b'\x74\x00';j1=len(w)-1                             # je orig
  w+=b'\xf3\x0f\x10\x05'+rel(8,0x17d7a50+0xa8)           # movss xmm0,[override]
  w+=b'\x0f\x57\xc9'                                     # xorps xmm1,xmm1
  w+=b'\x0f\x2e\xc1'                                     # ucomiss xmm0,xmm1
  w+=b'\x77\x00';j2=len(w)-1                             # ja orig (the original returns the override)
  w+=b'\xf3\x0f\x10\x05'+rel(8,ADV+4)                    # movss xmm0,[respawn]
  w+=b'\x0f\x2e\xc1'                                     # ucomiss xmm0,xmm1
  w+=b'\x76\x00';j3=len(w)-1                             # jbe orig
  w+=b'\xc3'                                             # ret xmm0
  og=len(w);w[j1]=og-(j1+1);w[j2]=og-(j2+1);w[j3]=og-(j3+1)
  w+=bytes.fromhex('554889e54157')
  w+=b'\xe9'+rel(5,site+6)
  assert len(w)<=0x80;buf[o:o+len(w)]=w
  image[site:site+6]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind='adv-respawn',wrapper_hex=w.hex()))
  # (2) Health multiplier. 1.19 f_2b0f..: after the booster health (f_5b1b8 0x9a925faf) and speed (0x7bb853bb) steps,
  # 0x2b11b0: if netmgr+0x5554 != 1.0 -> health = (int)(health * mult) (fctiwz), then the character's max-health call.
  # PS4 0x692156..0x692244 (health in r13d) lacks it. At 0x69220b (mov rax,[rbx]; mov rax,[rax] = 6 bytes, replayed; a jump
  # target of 0x692203 only; flags dead before the call) apply it inline in xmm1 (xmm0 is live, no stack use).
  site=0x69220b
  if image[site:site+6]!=bytes.fromhex('488b03488b00'):raise ValueError('health multiplier site mismatch')
  at,buf,o=stub_slot(int(os.environ.get('U3MP_HMS','0x10880'),16))
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use health')
  w=bytearray()
  w+=b'\x83\x3d'+rel(7,ADV)+b'\x00'                      # cmp dword [sel1],0
  w+=b'\x74\x00';k1=len(w)-1                             # je done
  w+=b'\x81\x3d'+rel(10,ADV+8)+struct.pack('<I',0x3f800000)   # cmp dword [health],1.0f
  w+=b'\x74\x00';k2=len(w)-1                             # je done
  w+=bytes.fromhex('f3410f2acd')                         # cvtsi2ss xmm1,r13d (xmm1 is dead here: not an argument of the call
  w+=b'\xf3\x0f\x59\x0d'+rel(8,ADV+8)                    #  below, only one of the two paths into 0x69220b writes it); mulss xmm1,[health]
  w+=bytes.fromhex('f3440f2ce9')                         # cvttss2si r13d,xmm1
  dn=len(w);w[k1]=dn-(k1+1);w[k2]=dn-(k2+1)
  w+=bytes.fromhex('488b03488b00')
  w+=b'\xe9'+rel(5,site+6)
  assert len(w)<=0x80;buf[o:o+len(w)]=w
  image[site:site+6]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'
  wrappers.append(dict(site=hex(site),address=hex(at),kind='adv-health',wrapper_hex=w.hex()))
  # v5.50 Advanced Settings stage 3: the other 1.19 readers. All inline (no helper call), all inert while ExtState.adv
  # sel1 == 0 (custom-game block never initialised, U3MP_ADV=0). G5 = PS3 g+5 (0x123ef88+5) = PS4 0x17d5c58+9.
  # U3MP_ADV_READERS=0 leaves these seven sites out.
  if os.environ.get('U3MP_ADV_READERS','1')!='0':
   G5=0x17d5c58+9
   def xslot(env,dflt,name):
    a,b2,o2=stub_slot(int(os.environ.get(env,dflt),16))
    if any(b2[o2:o2+0x80]):raise ValueError('stub slot in use '+name)
    return a,b2,o2
   def fix8(w,js,tgt):
    for j in js:
     d=tgt-(j+1);assert -128<=d<128;w[j]=d&255
   def put(site,ln,at,buf,o,w,name):
    assert len(w)<=0x80,(name,len(w));buf[o:o+len(w)]=w
    image[site:site+ln]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*(ln-5)
    wrappers.append(dict(site=hex(site),address=hex(at),kind=name,wrapper_hex=w.hex()))
   # (3) Entry stubs that make a function return 0 when its 1.19 toggle is off. In all three 1.19 functions the toggle
   # test leads to the function's 'return 0' and everything before it is a pure lookup, so testing at the entry gives
   # the same result. g5: also require PS3 g+5 as 1.19 does.
   def ret0_entry(site,orig,env,dflt,name,mask,g5):
    nonlocal at,w
    if image[site:site+len(orig)]!=orig:raise ValueError(name+' entry mismatch')
    at,buf,o=xslot(env,dflt,name);w=bytearray()
    w+=b'\x83\x3d'+rel(7,ADV)+b'\x00';js=[len(w)+1];w+=b'\x74\x00'            # cmp dword [sel1],0; je orig
    if g5:w+=b'\x80\x3d'+rel(7,G5)+b'\x00';js.append(len(w)+1);w+=b'\x74\x00'  # cmp byte [g+5],0; je orig
    w+=b'\xf6\x05'+rel(7,ADV+0x10+(mask>>8))+bytes([mask&255]);js.append(len(w)+1);w+=b'\x75\x00'   # test [f1/f2],bit; jnz orig
    w+=b'\x31\xc0\xc3'                                                        # xor eax,eax; ret
    fix8(w,js,len(w));w+=orig;w+=b'\xe9'+rel(5,site+len(orig))
    put(site,len(orig),at,buf,o,w,name)
   # Grenade rethrow (f2 bit6): 1.19 f_48ca18 (= 1.10 f_4637c8 = PS4 0xa0e6b0; grenade pick-up check, 0x48cab0) and
   # f_6f2bd0 (= 1.10 f_6e2d40 = PS4 0xd6b640, 0x6f2c3c): 'if (g+5 && !(custom +0x555f & 0x40)) return 0'.
   ret0_entry(0xa0e6b0,bytes.fromhex('554889e54157'),'U3MP_RTA','0x10900','adv-rethrow-a',0x140,True)
   ret0_entry(0xd6b640,bytes.fromhex('554889e5415653'),'U3MP_RTB','0x10980','adv-rethrow-b',0x140,True)
   # Buddy spawning (f1 bit1): 1.19 f_36bf9c (= 1.10 f_35deb0 = PS4 0x791ba0, spawn-on-buddy target) 0x36bffc:
   # 'if (!(custom +0x555e & 2)) return 0' (no g+5 test).
   ret0_entry(0x791ba0,bytes.fromhex('554889e54157'),'U3MP_BDS','0x10a00','adv-buddy',0x002,False)
   # (4) Boosters (f2 bit5) and medal kickbacks (f2 bit7). 1.10 f_29cf70 BoostersEnabled = g+5 && mgr flag (PS4
   # mgr+0x6808) && !(record+100) && f_30469c; 1.19 f_2a3eb0 adds '&& (custom & 0x20)' after the flag, and 1.19 moved
   # the 4 kickback callers (1.10 f_316a30, f_328088, f_328500, f_328eec) to the new f_2a4164 (same, with 0x80). PS4
   # inlined BoostersEnabled into 21 places; every copy ends in a call of 0x73fbc0 (= f_30469c) and its result decides.
   # So at 0x73fbc0's entry return 0 when the bit of the calling copy is off. By return address: kickback copies are
   # 0x6e7f89..0x6e814c (4 inlined getters), 0x725c8b (f_316a30), 0x74e926..0x74f2cb (3) and 0xc3d1e9; 0x68910d is
   # 1.10 f_29d204 (calls f_30469c directly, not a BoostersEnabled copy) and is left alone; all others are boosters.
   # The kickback copies still test record+0x6c (1.10 record+100) where 1.19 tests the new record+104: PS4 records
   # have no such field (rec+0x6c is 0 in the LAN playlist records, so the result is the same there).
   site=0x73fbc0;orig=bytes.fromhex('554889e5415653')
   if image[site:site+7]!=orig:raise ValueError('f_30469c entry mismatch')
   at,buf,o=xslot('U3MP_BKS','0x10a80','adv-boost');w=bytearray()
   w+=b'\x83\x3d'+rel(7,ADV)+b'\x00';jo=[len(w)+1];w+=b'\x74\x00'             # cmp dword [sel1],0; je orig
   w+=bytes.fromhex('488b0424')                                               # mov rax,[rsp]
   w+=b'\x48\x8d\x0d'+rel(7,0)                                                # lea rcx,[image base]
   w+=bytes.fromhex('4829c8')                                                 # sub rax,rcx
   w+=b'\x3d'+struct.pack('<I',0x68910d);jo.append(len(w)+1);w+=b'\x74\x00'   # cmp eax,0x68910d; je orig
   w+=b'\xb1\x80';jk=[]                                                       # mov cl,0x80
   w+=b'\x8d\x90'+struct.pack('<i',-0x6e7f89)+b'\x81\xfa'+struct.pack('<I',0x6e814c-0x6e7f89+1);jk.append(len(w)+1);w+=b'\x72\x00'
   w+=b'\x3d'+struct.pack('<I',0x725c8b);jk.append(len(w)+1);w+=b'\x74\x00'
   w+=b'\x8d\x90'+struct.pack('<i',-0x74e926)+b'\x81\xfa'+struct.pack('<I',0x74f2cb-0x74e926+1);jk.append(len(w)+1);w+=b'\x72\x00'
   w+=b'\x3d'+struct.pack('<I',0xc3d1e9);jk.append(len(w)+1);w+=b'\x74\x00'
   w+=b'\xb1\x20'                                                             # mov cl,0x20 (boosters)
   fix8(w,jk,len(w))
   w+=b'\x84\x0d'+rel(6,ADV+0x11);jo.append(len(w)+1);w+=b'\x75\x00'          # test [f2],cl; jnz orig
   w+=b'\x31\xc0\xc3'                                                         # xor eax,eax; ret
   fix8(w,jo,len(w));w+=orig;w+=b'\xe9'+rel(5,site+7)
   put(site,7,at,buf,o,w,'adv-boost')
   # (5) Sprinting (f1 bit0). 1.19 0x42b6f4 (player update, = 1.10 0x588cf4 = PS4 0xbdd5a6): after the 'can sprint'
   # flag (r28 = PS4 r13d) is computed, 'if (g+5) r28 &= custom bit8'. Hook 0xbdd5a6 (xor ebx,ebx; test r15,r15 = 5 bytes,
   # replayed last so the flags are the original ones; 0xbdd5a6 is a jump target, nothing jumps into 0xbdd5a7..0xbdd5aa).
   site=0xbdd5a6;orig=bytes.fromhex('31db4d85ff')
   if image[site:site+5]!=orig:raise ValueError('sprint site mismatch')
   at,buf,o=xslot('U3MP_SPS','0x10b00','adv-sprint');w=bytearray()
   w+=b'\x83\x3d'+rel(7,ADV)+b'\x00';js=[len(w)+1];w+=b'\x74\x00'
   w+=b'\x80\x3d'+rel(7,G5)+b'\x00';js.append(len(w)+1);w+=b'\x74\x00'
   w+=b'\xf6\x05'+rel(7,ADV+0x10)+b'\x01';js.append(len(w)+1);w+=b'\x75\x00'
   w+=b'\x45\x31\xed'                                                         # xor r13d,r13d
   fix8(w,js,len(w));w+=orig;w+=b'\xe9'+rel(5,site+5)
   put(site,5,at,buf,o,w,'adv-sprint')
   # (6) Weapon mode (+0xc, the set id of the chosen DC 0x2982116f entry; 0 = Default). 1.19 0x36ea70 (spawn loadout,
   # = PS4 0x7884f4, the join of all paths that pick the weapon set in ebx = PS3 r27): 'r26 = custom +0x5558; if (!r26)
   # r26 = r27'. Hook the join (lea rax,[0x17d5c58] = 7 bytes, rip-relative, replayed; jump target of 0x7883ce/0x7884dd/
   # 0x7884ef only): if the mode is set, ebx = mode.
   site=0x7884f4
   if image[site:site+3]!=bytes.fromhex('488d05') or site+7+struct.unpack_from('<i',image,site+3)[0]!=0x17d5c58:raise ValueError('weapon mode site mismatch')
   at,buf,o=xslot('U3MP_WMS','0x10b80','adv-weapon');w=bytearray()
   w+=b'\x83\x3d'+rel(7,ADV)+b'\x00';js=[len(w)+1];w+=b'\x74\x00'
   w+=b'\x8b\x05'+rel(6,ADV+0xc)                                              # mov eax,[weapon]
   w+=b'\x85\xc0';js.append(len(w)+1);w+=b'\x74\x00'                          # test eax,eax; je done
   w+=b'\x89\xc3'                                                             # mov ebx,eax
   fix8(w,js,len(w));w+=b'\x48\x8d\x05'+rel(7,0x17d5c58);w+=b'\xe9'+rel(5,site+7)
   put(site,7,at,buf,o,w,'adv-weapon')
   # (7) Health multiplier, second site. 1.19 0x432a4c (character spawn): after the booster health (f_5b1b8 0x9a925faf)
   # 'if (g+5 && health != 1.0) r23 = (int)(r23 * health)'. PS4 0xbd9ad2..0xbd9b20 computes it as esi = round(base * mult);
   # hook the next instruction 0xbd9b24 (mov rdi,[r14+0x310] = 7 bytes, replayed; no jump targets inside). xmm1 is dead
   # there (the vtable call that follows takes xmm0 only, set right before it).
   site=0xbd9b24;orig=bytes.fromhex('498bbe10030000')
   if image[site:site+7]!=orig:raise ValueError('health site 2 mismatch')
   at,buf,o=xslot('U3MP_HM2','0x10c00','adv-health2');w=bytearray()
   w+=b'\x83\x3d'+rel(7,ADV)+b'\x00';js=[len(w)+1];w+=b'\x74\x00'
   w+=b'\x80\x3d'+rel(7,G5)+b'\x00';js.append(len(w)+1);w+=b'\x74\x00'
   w+=b'\x81\x3d'+rel(10,ADV+8)+struct.pack('<I',0x3f800000);js.append(len(w)+1);w+=b'\x74\x00'
   w+=bytes.fromhex('f30f2ace')                                               # cvtsi2ss xmm1,esi
   w+=b'\xf3\x0f\x59\x0d'+rel(8,ADV+8)                                        # mulss xmm1,[health]
   w+=bytes.fromhex('f30f2cf1')                                               # cvttss2si esi,xmm1
   fix8(w,js,len(w));w+=orig;w+=b'\xe9'+rel(5,site+7)
   put(site,7,at,buf,o,w,'adv-health2')
 # f002: EstablishConnection Enter (vtable 0x167e7f0 slot 3) is a bare ret at 0x67fe20 (16 bytes to the next function
 # 0x67fe30, referenced only by that vtable entry 0x167e808). PS3 1.19 f_288e58 (1.10 f_286248) stamps the state start time and
 # resets the members' load flags there; the helper ports it as kind 55 (ec_enter) with rsi = the state (saved rdi at
 # [rbp-0x28] inside the full-save stub). U3MP_EC_ENTER=0 keeps the retail ret (bisect).
 if os.environ.get('U3MP_EC_ENTER','1')!='0':
  if image[0x67fe20:0x67fe30]!=bytes.fromhex('c36666666666662e0f1f840000000000'):raise ValueError('EstablishConnection Enter mismatch')
  at,w=fullsave_stub(int(os.environ.get('U3MP_ECS','0x10c80'),16),bytes.fromhex('488b75d8'),55,lambda h:b'\xc3',0x67fe21)
  image[0x67fe20:0x67fe25]=b'\xe9'+struct.pack('<i',at-(0x67fe20+5))
  wrappers.append(dict(site='0x67fe20',address=hex(at),kind=55,wrapper_hex=w.hex()))
 # f004: PS3 Player init f_580bc0 (1.19 f_421c94) fills the 8 net anim tables at Player+0x137c (PS4 +0x1c20) with the art groups
 # proto / anim-proto-multiplayer / anim-soldier-mp / anim-proto-shield (f_377adc row 0); PS4 Player init allocates the array
 # and zeroes it (0xbd7684..0xbd775c). Hook the next instruction 0xbd7764 (call 0xac4ab0, 5 bytes, replayed; also the target of
 # the non-MP branch 0xbd769a, which is fine: the helper returns at once outside MP) with r15 = the Player -> kind 57.
 if os.environ.get('U3MP_PLAYER_NET_ANIMS_HOOK','1')!='0':
  site=0xbd7764
  if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=0xac4ab0 or image[0xbd7684:0xbd768c]!=bytes.fromhex('49c787201c0000'+'00')[:8]:raise ValueError('Player init net anim site mismatch')
  at,w=fullsave_stub(int(os.environ.get('U3MP_PNS','0x10e00'),16),bytes.fromhex('4c89fe'),57,lambda h:b'\xe8'+struct.pack('<i',0xac4ab0-(h+5)),site+5)
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=57,wrapper_hex=w.hex()))
 # f003: NetEventPlayerDeath ctor PS4 0x6ba9d0 (= PS3 1.10 f_2d0aa8, anim lookup f_2d088c; 1.19 identical code). When the death
 # anim sid is not in the player's 8 anim tables, m_animIndex stays -1 and the ctor traps (PS4 int 0x41 at 0x6badd8, PS3 tw 31):
 # the desert co-op Arena crash (explosion death, type 2). Value stub at the entry (push rbp; mov rbp,rsp; push r15; push r14 =
 # 8 bytes, function entry, replayed) -> kind 56 death_anim_check: records the lookup and, unless U3MP_DEATH_GUARD=0
 # (state+0x20c1 bit1), returns early only for a sid that would trap. The co-op death path 0x99c720 calls it at 0x99ccdb with
 # r13 = damage info and r14 = the death state: that call goes through a 2-instruction pre-stub that stores both in
 # ExtState.dthr (rw2+0x3c20) for the diagnostic, then jumps to 0x6ba9d0 (the call's return address stays 0x99cce0).
 if os.environ.get('U3MP_DEATH_CHECK','1')!='0':
  if image[0x6ba9d0:0x6ba9d8]!=bytes.fromhex('554889e541574156'):raise ValueError('NetEventPlayerDeath ctor entry mismatch')
  at,w=value_stub(int(os.environ.get('U3MP_DCS','0x10d80'),16),56,lambda h:bytes.fromhex('554889e541574156'),0x6ba9d8)
  image[0x6ba9d0:0x6ba9d8]=b'\xe9'+struct.pack('<i',at-(0x6ba9d0+5))+b'\x90'*3
  wrappers.append(dict(site='0x6ba9d0',address=hex(at),kind=56,wrapper_hex=w.hex()))
  site=0x99ccdb
  if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=0x6ba9d0:raise ValueError('death send call mismatch')
  pat,pbuf,po=stub_slot(int(os.environ.get('U3MP_DPS','0x10d00'),16))
  if any(pbuf[po:po+0x80]):raise ValueError('stub slot in use death-pre')
  pw=bytearray()
  pw+=b'\x4c\x89\x2d'+struct.pack('<i',rw2+0x3c20-(pat+len(pw)+7))     # mov [rip+dthr0],r13
  pw+=b'\x4c\x89\x35'+struct.pack('<i',rw2+0x3c28-(pat+len(pw)+7))     # mov [rip+dthr1],r14
  pw+=b'\xe9'+struct.pack('<i',0x6ba9d0-(pat+len(pw)+5))                # jmp 0x6ba9d0 (now the value stub)
  pbuf[po:po+len(pw)]=pw
  image[site:site+5]=b'\xe8'+struct.pack('<i',pat-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(pat),kind='death-pre',wrapper_hex=pw.hex()))
 # v5.28 slot 3: HUD-net slot 3 0xc3b770 -> kind 52 then ret (replay = c3, the jump back is never reached).
 if os.environ.get('U3MP_SPOTTED_HUD','1')!='0':
  if image[0xc3b770:0xc3b776]!=bytes.fromhex('554889e55350'):raise ValueError('hud slot 3 entry mismatch')
  at,w=fullsave_stub(int(os.environ.get('U3MP_HSS','0x10380'),16),bytes.fromhex('4889fe'),52,lambda h:b'\xc3',0xc3b776)
  image[0xc3b770:0xc3b775]=b'\xe9'+struct.pack('<i',at-(0xc3b770+5))
  wrappers.append(dict(site='0xc3b770',address=hex(at),kind=52,wrapper_hex=w.hex()))
 # v5.26 diagnostic: watch the script-instance teardown 0x1257010 (kind 51 at its entry: push rbp; mov rbp,rsp; push r15 =
 # 6 bytes replayed; callers 0x123deb1/0x12407f1/0x125840a/0x125843a, nothing jumps into the displaced bytes). Records
 # instances whose wait count is garbage (ExtState sg/sgd/sgo). U3MP_SCRIPT_WATCH=0 leaves it out; U3MP_SCRIPT_GUARD=1
 # (RS2 bit 0x40000) skips the teardown of a bad instance. v5.31: value stub, the helper only reads (the v5.26 guard wrote
 # instance+0x644 and faulted on a released page).
 if os.environ.get('U3MP_SCRIPT_WATCH','1')!='0':
  if image[0x1257010:0x1257016]!=bytes.fromhex('554889e54157'):raise ValueError('script teardown entry mismatch')
  at,w=value_stub(int(os.environ.get('U3MP_SGS','0x10280'),16),51,lambda h:bytes.fromhex('554889e54157'),0x1257016)   # v5.31: value stub (helper may skip the teardown, never writes)
  image[0x1257010:0x1257016]=b'\xe9'+struct.pack('<i',at-(0x1257010+5))+b'\x90'
  wrappers.append(dict(site='0x1257010',address=hex(at),kind=51,wrapper_hex=w.hex()))
 # s011 (UI-BUGS M4): task status line "Task '%s-%s' is %s!" + "%02i:%02i:%02i" (0x125c770, called by every MP task update).
 # PS3 1.10 0x9e4184.. has the same condition (debug byte 0x13aef48 -> script flags / block 0x12f1f2c[1]+5 = net struct
 # 0x122d650+5, the MP-active flag that PS4 reads at 0x17d5c58+9 = 0x17d5c61) but only sprintf's the two lines (f_8a5a64 at
 # 0x9e426c / 0x9e42e4): retail PS3 draws nothing. PS4 kept the draws (0xf1d450 at 0x125c8ad / 0x125c972), so with MP
 # active the text shows on every MP page and in matches. Skip the print block like PS3: jmp 0x125c810 -> 0x125c977 (the
 # epilogue that clears 0x1b3ee48/0x1b3ee50; r12 is still 0x1b3ee48 there). U3MP_TASK_PRINT=1 keeps PS4's on-screen print.
 if os.environ.get('U3MP_TASK_PRINT','0')!='1':
  site=0x125c810
  if image[site:site+7]!=bytes.fromhex('488dbd60ffffff') or image[0x125c977:0x125c97e]!=bytes.fromhex('49c70424000000'):raise ValueError('task print site mismatch')
  image[site:site+7]=b'\xe9'+struct.pack('<i',0x125c977-(site+5))+b'\xcc\xcc'   # int3 pad: never reached, keeps the wrapper bytes unique
  wrappers.append(dict(site=hex(site),address=hex(site),kind='task-print-skip',wrapper_hex=image[site:site+7].hex()))
 # v5.21: script-module move (0x11fdf3x, "Relocate", script-module.cpp:671). When the DC heap moves a script module, every
 # bitmap-marked slot must point inside the module's old bounds, else the assert at 0x11fdf89 traps (int 0x41). Converted
 # PS3 modules keep pointer bits on script variables that scripts overwrite at runtime with pointers outside the module
 # (seen: co-op arena / Survival, *slot = image 0x14975a4 "(no debug memory)", a sid->string result). Such a pointer
 # must simply stay as it is: skip it (counter RS2[14], state+0x3db0) and continue with the next slot at 0x11fe015.
 # U3MP_RELOC_FIX=0 keeps the original assert.
 # s009: PS3 has no such check. Its Relocate (1.10 f_9b0fc0 -> f_9b0764) adds the delta to EVERY bitmap-marked slot:
 # 'if (bit) *slot += delta' (0x9b0764 loop via the bit reader f_835e60), with no bounds test and no trap; the bounds assert
 # is a PS4 addition. So by default the stub now continues at 0x11fe00b (mov rax,[r14]; add rax,[rbp-0x58]; mov [r14],rax),
 # i.e. the value PS4 itself writes after the assert, which is PS3's result. Registers there are the same as at 0x11fdf89
 # (the stub calls nothing; r9d is the 0 the assert path restores). The counter (state+0x3db0) still counts these slots.
 # U3MP_RELOC_SKIP=1 brings back the v5.21 skip (the slot keeps its value, continue at 0x11fe015).
 if os.environ.get('U3MP_RELOC_FIX','1')!='0':
  if image[0x11fdf89:0x11fdf90]!=bytes.fromhex('4c8b3d28489f00') or image[0x11fe015:0x11fe018]!=bytes.fromhex('0fb6c2'):raise ValueError('relocate assert site mismatch')
  if image[0x11fe00b:0x11fe015]!=bytes.fromhex('498b06480345a8498906'):raise ValueError('relocate add site mismatch')
  at,buf,o=stub_slot(int(os.environ.get('U3MP_RELOCS','0x10180'),16))
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use reloc')
  w=bytearray(b'\xff\x05'+bytes(4));struct.pack_into('<i',w,2,(state+0x3db0)-(at+6))
  w+=b'\xe9'+bytes(4);struct.pack_into('<i',w,7,(0x11fe015 if os.environ.get('U3MP_RELOC_SKIP','0')=='1' else 0x11fe00b)-(at+len(w)))
  buf[o:o+len(w)]=w
  image[0x11fdf89:0x11fdf8e]=b'\xe9'+struct.pack('<i',at-(0x11fdf89+5))
  wrappers.append(dict(site='0x11fdf89',address=hex(at),kind='reloc-skip',wrapper_hex=w.hex()))
 # v5.27 UI (c): font line height. PS3's .fnt loader f_979300 sets the font's line advance (font +8) to lineHeight + padding[1]
 # + spacing[1] + padding[3]; PS4's loader 0xf156a0 dropped the padding: font +0x1c = lineHeight + spacing[1] (0xf15b3f). The
 # .fnt files are identical on both (main/classic padding 3, game3/symbol3 2), so every PS4 text line is 6 (4) font units
 # shorter: menu rows 50 px apart instead of PS3's 58, page titles sitting low on the panel. At 0xf15b3f (add eax,[rsp+0xa8];
 # mov [r14+0x1c],ax = 12 bytes, no branch targets inside, flags dead) add padding[1] ([rsp+0xb8]) and padding[3]
 # ([rsp+0xb0]) as well. Font +0x20 (raw lineHeight, PS3 +0xc) is unchanged. U3MP_FONT_PAD=0 leaves it out.
 if os.environ.get('U3MP_FONT_PAD','1')!='0':
  site=0xf15b3f
  if image[site:site+12]!=bytes.fromhex('038424a800000066418946 1c'.replace(' ','')):raise ValueError('font loader line height mismatch')
  at,buf,o=stub_slot(int(os.environ.get('U3MP_FONTS','0x10300'),16))
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use font')
  w=bytearray.fromhex('038424a8000000 038424b8000000 038424b0000000 664189461c'.replace(' ',''))
  w+=b'\xe9'+struct.pack('<i',(site+12)-(at+len(w)+5))
  buf[o:o+len(w)]=w
  image[site:site+12]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*7
  wrappers.append(dict(site=hex(site),address=hex(at),kind='font-pad',wrapper_hex=w.hex()))
 # v5.30 solo-start port of the user's RPCS3 patch (U3MP_SOLO_COOP, default on; per-site table in NOTES.md). Code sites:
 # (a) match-end check PS4 0x721850 (= PS3 f_30d994): "unless debug byte 0x17d7a50+0x7c, count the
 #     players and end the match with reason 3 (not enough players)". The patch removes the byte test so the skip branch is
 #     always taken: jne 0x722504 at 0x722440 -> jmp (6 bytes, nothing jumps inside).
 # (b) booster list PS4 0x78f3b0 (= PS3 1.10 f_35e808, rewritten in 1.19): the player's booster list count
 #     (+0x84c, 16-byte entries) starts at 1 instead of 0, so entry 0 of the previous build is kept. U3MP_SOLO_COOP_NOBOOST=1
 #     leaves it out.
 # (c) custom-game Map row value (datasource 0x753850 case 5 = PS3 f_32e87c -> f_32aa20, inlined at 0x753a36..0x753abd): the index counter starts at 1 and the not-found default is 1. Three 5..10-byte stubs.
 #     U3MP_SOLO_COOP_NOMAP=1 leaves these and the helper's list preset out.
 # The start-match min-player check and the bss/static-init parts are in the helper (start_checks, solo_patch_tick).
 if os.environ.get('U3MP_SOLO_COOP','1')!='0':
  site=0x722440
  if image[0x722434:0x722446]!=bytes.fromhex('488d0515560b018a407c84c00f85be000000'):raise ValueError('solo co-op end-check site mismatch')
  image[site:site+6]=bytes.fromhex('e9bf00000090')
  wrappers.append(dict(site=hex(site),address=hex(site),kind='solo-coop-endcheck',wrapper_hex='e9bf00000090'))
  if os.environ.get('U3MP_SOLO_COOP_NOBOOST')!='1':
   site=0x78f3c1
   if image[site:site+12]!=bytes.fromhex('41c784244c08000000000000'):raise ValueError('solo co-op booster list site mismatch')
   image[site+8]=1
   wrappers.append(dict(site=hex(site),address=hex(site),kind='solo-coop-boostlist',wrapper_hex=image[site:site+12].hex()))
  if os.environ.get('U3MP_SOLO_COOP_NOMAP')!='1':
   if image[0x753a36:0x753a40]!=bytes.fromhex('31db84d20f847d000000') or image[0x753a81:0x753a86]!=bytes.fromhex('31db4c89ea') or image[0x753ab6:0x753abd]!=bytes.fromhex('31db4539c37c9c'):raise ValueError('solo co-op map index sites mismatch')
   for site,ln,off,body,back in ((0x753a36,10,int(os.environ.get('U3MP_SCM0','0x10500'),16),None,0x753a40),
                                 (0x753a81,5,int(os.environ.get('U3MP_SCM1','0x10580'),16),bytes.fromhex('4c89ea'),0x753a86),
                                 (0x753ab6,5,int(os.environ.get('U3MP_SCM2','0x10600'),16),bytes.fromhex('4539c3'),0x753abb)):
    at,buf,o=stub_slot(off)
    if any(buf[o:o+0x80]):raise ValueError('stub slot in use solo-coop map '+hex(off))
    w=bytearray(b'\xbb'+struct.pack('<I',1))                                  # mov ebx,1 (was xor ebx,ebx)
    if body is None:
     w+=bytes.fromhex('84d2')+b'\x0f\x84'+struct.pack('<i',0x753abd-(at+len(w)+2+6))  # test dl,dl; je 0x753abd
    else:w+=body
    w+=b'\xe9'+struct.pack('<i',back-(at+len(w)+5))
    buf[o:o+len(w)]=w
    image[site:site+ln]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*(ln-5)
    wrappers.append(dict(site=hex(site),address=hex(at),kind='solo-coop-mapindex',wrapper_hex=w.hex()))
 # v5.36 test option U3MP_SOLO_HUNTER=1 (default off; also keeps the v5.24 start bypass): Hunter (net type 18) ends at once
 # with one player because the match-end check PS4 0x721850 (= PS3 f_30d994 0x30e1c4..0x30e2d0) returns reason 2 when all
 # active players are on one team. PS3 runs that check for type 18 unconditionally (the user's RPCS3 patch does not touch
 # it), so this is a deviation for solo testing only: at 0x722011 the type-18 branch (je 0x7220dd) is dropped, so Hunter
 # takes the co-op path (type 18 is in the 0x72b1 co-op mask) that skips the one-team check.
 if os.environ.get('U3MP_SOLO_HUNTER')=='1':
  site=0x722011
  if image[0x72200c:site+6]!=bytes.fromhex('837c080c12 0f84c6000000'.replace(' ','')):raise ValueError('hunter one-team end check site mismatch')
  image[site:site+6]=bytes.fromhex('660f1f440000')
  wrappers.append(dict(site=hex(site),address=hex(site),kind='solo-hunter-endcheck',wrapper_hex='660f1f440000'))
 # v5.35 custom-game type list (PS3 f_32ada0 = PS4 0x7503c0, both 1.10 and 1.19): with the LAN flag set, a net game type
 # goes into the co-op list (net-game+0x3fd = PS4 0x17d5c58+0x40d set) iff its type is 8, 11 or 17, else into the
 # competitive list. PS4 dropped type 8 (Shade Survival; also PS3 f_310afc's anim-shade-mp want) from that set, so the
 # converted type-8 net games landed in the competitive list and co-op showed only Arena (17) and Adventure (11).
 # At 0x750475 (cmp esi,0x11; sete cl; cmp esi,0xb; sete dl; or dl,cl = 14 bytes, flags reset by the following xor dl,al)
 # the stub adds cmp esi,8; sete cl; or dl,cl. U3MP_COOP_TYPES=0 leaves it out.
 if os.environ.get('U3MP_COOP_TYPES','1')!='0':
  site=0x750475
  if image[site:site+14]!=bytes.fromhex('83fe110f94c183fe0b0f94c208ca'):raise ValueError('custom-game co-op type set site mismatch')
  at,buf,o=stub_slot(int(os.environ.get('U3MP_CTS','0x10680'),16))
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use coop types')
  w=bytearray.fromhex('83fe110f94c183fe0b0f94c208ca 83fe080f94c108ca'.replace(' ',''))
  w+=b'\xe9'+struct.pack('<i',(site+14)-(at+len(w)+5))
  buf[o:o+len(w)]=w
  image[site:site+14]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*9
  wrappers.append(dict(site=hex(site),address=hex(at),kind='coop-type-set',wrapper_hex=w.hex()))
 # v5.30 co-op level progress divide (PS4 0x661ca0 = PS3 f_5b2d8, 1.10 0x5b440 / 1.19 0x5bba8 `divw r5,r5,r0`): level =
 # points / [0x17c3404] + 1 for every player in a co-op match. The per-level points come from DC net.bin 0xc85e199d +0x30 (1),
 # loaded by 0x67ecd8 (PS3 f_5bf78) AFTER the start path 0x73ad00 -> 0x73a40c already called 0x661ca0, so on the first co-op
 # match after boot the divisor is 0 on PS3 as well. PPC divw does not trap (RPCS3 yields 0); x86 idiv faults (seen at
 # 0x661eea). The stub keeps the PS3 result: divisor 0 -> quotient 0 -> level 1 (edx = 1), counted at ExtState sc[3].
 # Replaces mov eax,[r13+0x30]; cdq; idiv dword [r13]; lea edx,[rax+1] (12 bytes, flags dead, no jumps inside).
 # U3MP_DIV0_GUARD=0 leaves it out.
 if os.environ.get('U3MP_DIV0_GUARD','1')!='0':
  site=0x661ee5
  if image[site:site+12]!=bytes.fromhex('418b45309941f77d008d5001'):raise ValueError('co-op level divide site mismatch')
  at,buf,o=stub_slot(int(os.environ.get('U3MP_DZS','0x10480'),16))
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use div0')
  back=site+12
  w=bytearray.fromhex('418b4530 41837d0000 740d 99 41f77d00 8d5001'.replace(' ',''))
  w+=b'\xe9'+struct.pack('<i',back-(at+len(w)+5))
  w+=b'\xba'+struct.pack('<I',1)
  w+=b'\xff\x05'+struct.pack('<i',(rw2+0x2ffc)-(at+len(w)+6))                 # inc dword [ExtState sc[3]]
  w+=b'\xe9'+struct.pack('<i',back-(at+len(w)+5))
  buf[o:o+len(w)]=w
  image[site:site+12]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'*7
  wrappers.append(dict(site=hex(site),address=hex(at),kind='coop-level-div0',wrapper_hex=w.hex()))
# v5.13: ScriptManager::Init 0x11f9d70 walks the static-ctor native list (head 0x1b3b4b0) at 0x11faa96
 # (lea r15,[0x1b3b4b0]; mov rax,[r15]). Call kind 44 first (pushes is-camera-underwater? / player-disable-zipline
 # records), replay the lea, back to 0x11faa9d. 0x11faa96 is reached by fall-through only.
 site=0x11faa96
 if image[site:site+7]!=bytes.fromhex('4c8d3d130a9400'):raise ValueError('script native list hook mismatch')
 at,w=fullsave_stub(0xd80,bytes.fromhex('4889fe'),44,lambda here:b'\x4c\x8d\x3d'+struct.pack('<i',0x1b3b4b0-(here+7)),site+7)
 image[site:site+7]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x66\x90'
 wrappers.append(dict(site=hex(site),address=hex(at),kind=44,wrapper_hex=w.hex()))
 site=0x72d28b
 if image[site:site+6]!=bytes.fromhex('4183ff037c5e'):raise ValueError('shade anim want site mismatch')
 at,w=fullsave_stub(0x180,bytes.fromhex('4c89e6'),43,lambda here:bytes.fromhex('4183ff03')+b'\x0f\x8c'+struct.pack('<i',0x72d2ef-(here+4+6)),site+6)
 image[site:site+6]=b'\xe9'+struct.pack('<i',at-(site+5))+b'\x90'
 wrappers.append(dict(site=hex(site),address=hex(at),kind=43,wrapper_hex=w.hex()))

 # Late-join check 0x7462b0 indexes the team table with the leading-team result of 0x737eb0
 # without checking for -1 (tie, e.g. 0-0 at match start). On a tie the leading score equals any
 # team's score, so map -1 to team 0 at that call site (0x74654e).
 # v5.08 check against PS3: 0x7462b0 = PS3 f_318624 (same 0.25 time / 0.75 score / type 0xf / type 0x12 tests), called by
 # f_318b08 = PS4 0x746790. PS3 also indexes without a check: *(short*)(0x12492b2 + f_318200()*0x130 + 0x5c00), and its
 # leading-team f_318200 returns -1 on a tie as PS4 does, but 32-bit address arithmetic wraps, so PS3 read the short 0x130
 # bytes before team 0 (a field of the session object, not a score) and PS4's zero-extended index faults. The guard keeps
 # the meaning (on a tie the leading score is the tied score) instead of replicating the out-of-table read. Kept.
 site=0x74654e;tgt=0x737eb0
 if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=tgt:raise ValueError('tie guard hook mismatch')
 at=rx+0x400;w=bytearray.fromhex('4883ec08')
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',tgt-(here+5)))
 w.extend(bytes.fromhex('83f8ff 7502 31c0 4883c408 c3'))
 code[0x400:0x400+len(w)]=w
 # s001: U3MP_TIE_GUARD=0 leaves the tie guard out (bisect only: a tie then indexes the team table with -1)
 if __import__('os').environ.get('U3MP_TIE_GUARD','1')!='0':image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))
 wrappers.append(dict(site=hex(site),address=hex(at),kind='tie-guard',wrapper_hex=w.hex()))

 # Guard the emblem setup call in match setup (0x72c17c call 0x66b1d0) through helper kind 10.
 site=0x72c17c
 if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=0x66b1d0:raise ValueError('emblem hook mismatch')
 at=rx+0x3c0;w=bytearray.fromhex('4883ec18 48893c24 4889fe')
 here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
 w.extend(b'\xba'+struct.pack('<I',10))
 here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
 w.extend(bytes.fromhex('4883c418c3'))
 if len(w)>0x40:raise ValueError('emblem wrapper size')
 code[0x3c0:0x3c0+len(w)]=w
 # s001: U3MP_EMBLEM_GUARD=0 leaves the original call 0x66b1d0 (no early table fill, no lookup check)
 if __import__('os').environ.get('U3MP_EMBLEM_GUARD','1')!='0':image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))
 wrappers.append(dict(site=hex(site),address=hex(at),kind=10,wrapper_hex=w.hex()))

 # Tick the restored net clock (state+0x2000) with the engine clocks: the last per-frame
 # Clock::Update call (clock 9, 0x10ff75c) goes through a stub that updates clock 9 then the net clock.
 # s002: by default the net clock is engine clock 7 again (PS3 NetInit, see startup_tick), so the second tick is only emitted
 # unless U3MP_NET_CLOCK=1 (old helper-owned clock, RS2 0x80000000). The stub still runs the per-frame helper call (kind 16).
 # f002: the PS3 clock is the default again (EstablishConnection's Enter is restored below); U3MP_NET_CLOCK=0 = old own clock.
 own_clock=__import__('os').environ.get('U3MP_NET_CLOCK','1')=='0'
 site=0x10ff75c;tick=0x1197090
 if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=tick:raise ValueError('clock tick hook mismatch')
 at=rx+0x380;w=bytearray.fromhex('4883ec08')
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',tick-(here+5)))
 if own_clock:
  here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',state+0x2000-(here+7)))
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',tick-(here+5)))
 # v4.83: per-frame helper call (kind 16), also during a match where the menu hooks are quiet
 here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
 here=at+len(w);w.extend(b'\x48\x8d\x35'+struct.pack('<i',state-(here+7)))
 w.extend(b'\xba'+struct.pack('<I',16))
 here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
 w.extend(bytes.fromhex('4883c408c3'))
 assert len(w)<0x80
 code[0x380:0x380+len(w)]=w
 image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))
 wrappers.append(dict(site=hex(site),address=hex(at),kind='net-clock-tick',wrapper_hex=w.hex()))

 # v5.15: the frozen menu shell's DC check (0x2050270) required PAGE_PROFILE 0x82d5c00b to hold exactly 10 widgets and
 # ud2'd at 0x205001d otherwise. batch31 menu.bin (PS3 1.19 layout, no BUTTON_OPTIONS row) has 9. The check only reads
 # widgets 0..2, so accept 9 or more: cmp dword [rcx],0xa / jne -> cmp dword [rcx],9 / jb.
 if image[0x2050343:0x2050348]!=bytes.fromhex('83390a7542'):raise ValueError('menu shell widget-count check mismatch')
 image[0x2050343:0x2050348]=bytes.fromhex('8339097242')
 # v5.52 (ASSETS batch42b): the same check also required exactly 149 (0x95) pages. PS3 1.19 adds PAGE_CUSTOMRULES2
 # (Advanced Settings), appended as page 150. The scan only covers the first 149 entries, so accept >= 149: jne -> jb.
 if image[0x205027b:0x2050287]!=bytes.fromhex('813f950000000f8504010000'):raise ValueError('menu shell page-count check mismatch')
 image[0x2050282]=0x82
 # v5.21 UI (b): the frozen menu shell copies PAGE 0xda46ad0 widget 4 (a "Multiplayer" button, action 0xf75b3a65) into
 # the empty PAGE_PROFILE (campaign menu) widget 2 slot, from before the PS3 main menu tiles were restored. PS3 1.19's
 # campaign menu has no Multiplayer row (multiplayer is the main-menu tile). Skip the copy, keep the success result:
 # 0x2050469 -> mov eax,1; jmp 0x205038a. U3MP_SP_MPROW=1 keeps the row.
 if os.environ.get('U3MP_SP_MPROW')!='1':
  if image[0x2050469:0x205046b]!=bytes.fromhex('31f6') or image[0x20504b6:0x20504bb]!=bytes.fromhex('e9cffeffff'):raise ValueError('menu shell copy mismatch')
  image[0x2050469:0x2050473]=b'\xb8\x01\x00\x00\x00\xe9'+struct.pack('<i',0x205038a-(0x2050469+10))
 # Wrap the already restored menu lookup before the native controls are built.
 site=0x6496ec;old_target=site+5+struct.unpack_from('<i',image,site+1)[0]
 if image[site]!=0xe8 or old_target!=0x2050000:raise ValueError('frozen menu construction hook mismatch')
 at=rx+0x500;w=bytearray.fromhex('4883ec18')
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',old_target-(here+5)))
 w.extend(bytes.fromhex('48890424 4889c6'))
 here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
 w.extend(b'\xba'+struct.pack('<I',8))
 here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
 w.extend(bytes.fromhex('488b04244883c418c3'));code[0x500:0x500+len(w)]=w
 image[site:site+5]=b'\xe8'+struct.pack('<i',at-(site+5))
 # v4.20: the datasource gate patch (net-game+9 -> +8) is no longer applied. Since v4.6 the helper raises
 # the real multiplayer-active byte (net-game+9) at LAN start, as PS3 NetInit 0x290ca0 does, so the
 # original dispatcher check passes unmodified. DS_GATES is kept above only as documentation.
 # MP profile persistence (PS3 NetPlayerData publish f_35847c, stripped on PS4). Entry hooks that call
 # the helper first and then run the original function (its position-independent prologue is replayed):
 #  kind 40 at 0x672460 = PS3 f_287770, catch-all page event handler (rdi = {event, page id}); PS3 ended it
 #          with a publish on the profile / character / missions / weapon-mod pages.
 #  kind 41 at 0x67cb40 = PS3 f_282fa4 "Save Game Stats", enter of the post-match state (vtable 0x167e570
 #          slot 3; same clock/300+10 timer and state 0x20 stores); PS3 published both local profiles there.
 # rsp is 16-aligned at the helper call (entry rsp = 8 mod 16, minus 0x18); rdi is restored (neither
 # function reads another argument register: 0x672460 takes the event only, 0x67cb40 none).
 for site,offset,kind,prologue in ((0x672460,0xf80,40,'554889e5415653'),(0x67cb40,0xfc0,41,'554889e541574156')):
  expected=bytes.fromhex(prologue)
  if image[site:site+len(expected)]!=expected:raise ValueError('mp persistence hook mismatch '+hex(site))
  at=rx+offset;w=bytearray.fromhex('4883ec18 48893c24 4889fe')
  here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
  w.extend(b'\xba'+struct.pack('<I',kind))
  here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
  here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
  w.extend(bytes.fromhex('488b3c24 4883c418'));w.extend(expected)
  here=at+len(w);w.extend(b'\xe9'+struct.pack('<i',site+len(expected)-(here+5)))
  if len(w)>0x40:raise ValueError('mp persistence wrapper overlap')
  if any(code[offset:offset+0x40]):raise ValueError('mp persistence wrapper slot in use '+hex(offset))
  code[offset:offset+len(w)]=w
  image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=kind,original=expected.hex(),wrapper_hex=w.hex()))
 # v4.93: LAN choice callback 0x66e510 (PS3 f_27fdf0; registered at 0x673a5a for 0x73273bc7 "LAN", context 0, and at
 # 0x673ae0 for 0xb7314286 PAGE_COOP_CHOICE "Splitscreen", context 1). The frozen menu shell jumps it to its own flow
 # (0x2050180, context ignored, LAN only from PAGE_MULTIPLAYER_CHOICE). Route it to helper kind 42 instead: sets the co-op
 # flag 0x17d6065 = context != 0 and opens PAGE_LAN_CHOOSE_PROFILE. rdi = event {u32 action, u32, u64 context}.
 site=0x66e510
 if image[site]!=0xe9 or site+5+struct.unpack_from('<i',image,site+1)[0]!=0x2050180 or image[site+5:site+16]!=bytes.fromhex('66662e0f1f840000000000'):raise ValueError('LAN choice callback mismatch')
 at=rx+0x460;w=bytearray.fromhex('4883ec18 48893c24 4889fe')
 here=at+len(w);w.extend(b'\x48\x8d\x3d'+struct.pack('<i',-(here+7)))
 w.extend(b'\xba'+struct.pack('<I',42))
 here=at+len(w);w.extend(b'\x48\x8d\x0d'+struct.pack('<i',state-(here+7)))
 here=at+len(w);w.extend(b'\xe8'+struct.pack('<i',helper-(here+5)))
 w.extend(bytes.fromhex('4883c418c3'))
 if len(w)>0x40 or any(code[0x460:0x4a0]):raise ValueError('LAN choice stub slot')
 code[0x460:0x460+len(w)]=w
 image[site:site+5]=b'\xe9'+struct.pack('<i',at-(site+5))
 wrappers.append(dict(site=hex(site),address=hex(at),kind=42,original='e9->0x2050180',wrapper_hex=w.hex()))
 # c001 (agent C): the v4.93 U3MP_HUD_CTX experiment (0x570da9 -> kind 19) is removed; see the c001 note at the old
 # hud_draw_ctx place in the helper (retail HUD text is flushed by DrawHud 0xc1a680, 0x18c6280+0 is 1).
 # v4.94 PS3 front-end chooser (co-op entry). PS3 Menu::OnButton f_265c94: BUTTON_TITLE_START 0x2bbbb5c5 shows
 # PAGE_ROOTPROFILE 0xd45dfcca, whose FRONT_END widget (type 0x2c; PS3 vtable 0x10e2478, input f_216a94 = PS4 0x621080,
 # intact) picks with left/right: 0 Single Player -> OnButton 0xa89bb12f (-> PAGE_MAIN 0x82d5c00b), 1 Multiplayer ->
 # PAGE_MULTIPLAYER_CHOICE 0x1e66a744, 2 Co-op -> PAGE_COOP_CHOICE 0xb0e67b0e ("Splitscreen" = LAN choice context 1).
 # PS4 TITLE_START (0x6516a2) went straight to PAGE_MAIN / 0x11fefb39 after its one-time init, so the chooser and
 # PAGE_COOP_CHOICE were unreachable. (a) 0x6517af (reached after the init on both paths): show PAGE_ROOTPROFILE as PS3,
 # via the existing 'jmp 0x6502a5' at 0x65182a (ShowPage(menu, esi, 0, 0)). (b) The kept 0xa89bb12f handler's last
 # step (0x64eab6: PS3's DAT_01392984 == -1 test) jumps to PS4's own title-start test at 0x6517b6 (rax = 0x16e8120 is set
 # at 0x64eaa8 as at 0x6517af), so Single Player lands on the same page retail PS4 opened after the title screen.
 if image[0x6517af:0x6517b6]!=bytes.fromhex('488d056a690901') or image[0x6517b6:0x6517bd]!=bytes.fromhex('83b8fc010000ff') or image[0x65182a:0x65182f]!=bytes.fromhex('e976eaffff'):raise ValueError('title start tail mismatch')
 if image[0x64eaa8:0x64eac1]!=bytes.fromhex('488d057196090183b8fc010000ff0f85732d0000e9642d0000'):raise ValueError('front-end single player tail mismatch')
 image[0x6517af:0x6517b6]=bytes.fromhex('becafc5dd4')+b'\xeb'+bytes([0x65182a-0x6517b6])
 image[0x64eab6:0x64eabc]=b'\xe9'+struct.pack('<i',0x6517b6-(0x64eab6+5))+b'\x90'
 wrappers.append(dict(site='0x6517af',note='TITLE_START -> PAGE_ROOTPROFILE (PS3 f_265c94)'))
 wrappers.append(dict(site='0x64eab6',note='0xa89bb12f -> PS4 title-start page test 0x6517b6'))
 # v5.00: MP kit/hero colour tint. Producers pack part colours as 0xAARRGGBB (0x997a40, 0x9954f0, 0x994d20,
 # picker 0x7590a0) but the three consumers (NetCharacter init 0x690100 @0x690533, preview 0xbc9e50,
 # 0xbc9fc0) built the tint vector as (A,R,G): red always full, blue lost, black shows red/pink. Reorder the
 # lane inserts to (R,G,B,A); 4 bytes per site, same lengths.
 for a in (0x69054f,0xbc9f09,0xbca86e):
  if image[a:a+22]!=bytes.fromhex('c5f96ec1c4e37922c201c4e37922c602c4e37922c003'):raise ValueError('tint site mismatch %#x'%a)
  image[a:a+22]=bytes.fromhex('c5f96ec2c4e37922c601c4e37922c002c4e37922c103')
  wrappers.append(dict(site=hex(a),kind='tint-lane-order'))
 # v5.01: same byte-order bug in the medal/award popup colour (0xc22f70 @0xc24bce; PS3 0x64f250).
 a=0xc24beb
 if image[a:a+22]!=bytes.fromhex('c5f96ec8c4e37122c901c4e37122ca02c4e37122ce03'):raise ValueError('medal tint site mismatch')
 image[a:a+22]=bytes.fromhex('c5f96ec9c4e37122ca01c4e37122ce02c4e37122c803');wrappers.append(dict(site=hex(a),kind='tint-lane-order'))
 # v5.03: remaining MP colour consumers that unpack an 0xAARRGGBB colour as (c>>24,c>>16,c>>8,c) although their producers
 # pack ARGB (PS3 order); lanes reordered to (c>>16,c>>8,c,c>>24) as above. Checked producer by producer:
 #  0x5f7c80  widget draw 0x5f7b30: colour getter [obj+0x18] vt+0xd8, read as ARGB by the sibling draws 0x5f0d5f/0x5f364d
 #  0x639b7c  0x639a60: team colour from GetTeamInfo 0x7418e0 (+0xc; table 0x1b69380 is ARGB: 0xff5f8841 / 0xff415f88 swap)
 #  0x63c77b  quad helper 0x63c730 and 0x63d13a (0x63d100): callers pass alpha<<24, alpha<<24|0xffffff, 0xff000000
 #  0x63cd49/0x63cdc9/0x63ce31/0x63ce94  gradient helper 0x63cd00 (4 corner colours): callers pass alpha<<24 or 0
 #  0xc35e4f  HudNet text colour +0x2674 (default 0x80e0e0e0 at 0xc323e8)
 # Not touched (producer packs 0xRRGGBBAA, so the current order is right): 0xc3f334, 0xc9f33f, 0xc9f637, 0xc9fd51, 0xca0109,
 # 0x642f88, 0x65e1a5, 0x11b632d, 0x11bc495, 0x11bd6f8. 0xc29326 (0xc28ce0, HUD list colour +0x30) is ARGB-shaped too
 # but its only writers are the uncalled thunks 0xc4bd00/0xc4bd80 (list never filled on PS4); left as is.
 for a,o,n in ((0x5f7c80,'c5f96ec1c4e37922c201c4e37922c602c4e37922c003','c5f96ec2c4e37922c601c4e37922c002c4e37922c103'),
               (0x639b7c,'c5f96ec1c4e37922c201c4e37922c602c4e37922c003','c5f96ec2c4e37922c601c4e37922c002c4e37922c103'),
               (0x63c77b,'c5f96ee0c4e35922e201c4e35922e702c4e35922e103','c5f96ee2c4e35922e701c4e35922e102c4e35922e003'),
               (0x63cd49,'c5f96ed0c4e36922d701c4e36922d302c4e36922d103','c5f96ed7c4e36922d301c4e36922d102c4e36922d003'),
               (0x63cdc9,'c5f96ed8c4e36122d901c4e36122df02c4e36122db03','c5f96ed9c4e36122df01c4e36122db02c4e36122d803'),
               (0x63ce31,'c5f96ed8c4e36122d901c4e36122da02c4e36122df03','c5f96ed9c4e36122da01c4e36122df02c4e36122d803'),
               (0x63ce94,'c5f96ee0c4e35922e101c4e35922e202c4e35922e703','c5f96ee1c4e35922e201c4e35922e702c4e35922e003'),
               (0xc35e4f,'c5f96ec9c4e37122ca01c4e37122cf02c4e37122c803','c5f96ecac4e37122cf01c4e37122c802c4e37122c903')):
  if image[a:a+22]!=bytes.fromhex(o):raise ValueError('colour lane site mismatch %#x'%a)
  image[a:a+22]=bytes.fromhex(n);wrappers.append(dict(site=hex(a),kind='tint-lane-order'))
 # 0x63d100: its red byte lands in r10d, which the 2-byte-VEX vmovd cannot take, so the four extractions (0x63d11c, 30 bytes)
 # are rewritten instead to leave R/G/B/A in eax/r10d/ebx/edi; the inserts at 0x63d13a stay.
 a=0x63d11c
 if image[a:a+52]!=bytes.fromhex('4489c8c1e818bf10080000c44240f7d1bb08080000c4c260f7d9410fb6f9c5f96ec0c4c37922c201c4e37922c302c4e37922c703'):raise ValueError('colour lane site mismatch 0x63d11c')
 image[a:a+30]=bytes.fromhex('bf10080000c4c240f7c1bb08080000c44260f7d1410fb6d94489cfc1ef18');wrappers.append(dict(site=hex(a),kind='tint-lane-order'))
 # v5.11 PS3 f_30867c spawner filter: PS4 0x72fbc0 kept the netgame type 0x11/0x12 tests but dropped type 8 (co-op Shade),
 # so 0x2378cd79-gated spawners never activate in type 8. One lookup now tests 0x11, 0x12 and 8 like PS3 (0x72fde0..0x72fe80).
 a=0x72fde0
 if image[a:a+0xa0]!=bytes.fromhex('bf7d069a3be8062eac004885c07435488b004885c0742d410fbf8e1c66000085c978213b087d1d4863c9488b40084869c9d00000004889c24801ca7407837c080c11745cbf7d069a3be8c22dac004885c00f8484010000488b004885c00f8478010000410fbf8e1c66000085c90f88680100003b080f8d600100004863c9488b40084869c9d00000004889c24801ca0f8446010000837c080c120f853b010000'):raise ValueError('spawn type-8 site mismatch')
 image[a:a+0xa0]=bytes.fromhex('bf7d069a3be8062eac004885c0743e488b004885c07436410fbf8e1c66000085c9782a3b087d264863c9488b40084885c0741a4869c9d00000008b44080c83f811740f83f812740a83f8087405e989010000e949000000cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc');wrappers.append(dict(site=hex(a),kind='spawner-type8'))
 # v5.10: MAX_CACHED_LEVELS 800 -> 1100 (heap 0x46 is 0x3fc00: 1100*0xd0 + 0x5020 ptr table fits, 1200 doesn't; the heap can't grow, ValidateContext) (all PS3 MP levels: ~965 level defs vs retail 681).
 # LevelMgr level-def array [mgr+0x178] = N*0xd0 (alloc 0x10eb00b), init loop 0x10e9d3e, assert 0x10ea0b3.
 # The sorted pointer table [mgr+0x180] is allocated 0x5020 bytes (2564 slots), so it already fits.
 for a,old,new in ((0x10eb00b,'be008a0200','bec07d0300'),(0x10e9d3e,'4881f920030000','4881f94c040000'),(0x10ea0b3,'3d20030000','3d4c040000')):
  if image[a:a+len(old)//2]!=bytes.fromhex(old):raise ValueError('level cache site mismatch %#x'%a)
  image[a:a+len(old)//2]=bytes.fromhex(new)
  wrappers.append(dict(site=hex(a),kind='level-cache-1100'))
 # ===== agent C: code sites (x-stub slots 0x11a00..0x11f80, helper kinds 100..109) =====
 # c002: PS3 1.19 f_317d8c (late join closed?) adds an Elimination (type 10) rule set to 1.10 f_318624 = PS4 0x7462b0. Value stub
 # (kind 100, slot 0x11a00) at the entry: the helper answers type 10 the 1.19 way, every other type runs the native (same answer
 # in 1.10 and 1.19). Displaced: push rbp; mov rbp,rsp; push r15 (6 bytes, function entry, no branch targets inside).
 if image[0x7462b0:0x7462b6]!=bytes.fromhex('554889e54157'):raise ValueError('late join check entry mismatch')
 at,w=value_stub(0x11a00,100,lambda h:bytes.fromhex('554889e54157'),0x7462b6)
 image[0x7462b0:0x7462b6]=b'\xe9'+struct.pack('<i',at-(0x7462b0+5))+b'\x90'
 wrappers.append(dict(site='0x7462b0',address=hex(at),kind=100,wrapper_hex=w.hex()))
 # c003: PS3 1.19 Elimination "Waiting for new players" (see agc_port.inc). (a) The round-end send of the inlined type-10 round
 # check, call 0x6bcfc0 at 0x7259af, goes through value stub kind 101 (slot 0x11a80): the helper holds the round end while a
 # joined-but-unspawned player exists (returns 0 without the call), else the stub tail-jumps to 0x6bcfc0 with the original
 # edi/esi (the call's return address stays on the stack, so 0x6bcfc0 returns to 0x7259b4 as before).
 if image[0x7259af]!=0xe8 or 0x7259af+5+struct.unpack_from('<i',image,0x7259b0)[0]!=0x6bcfc0:raise ValueError('elim round end call mismatch')
 at,w=value_stub(0x11a80,101,lambda h:b'',0x6bcfc0)
 image[0x7259af:0x7259b4]=b'\xe8'+struct.pack('<i',at-(0x7259af+5))
 wrappers.append(dict(site='0x7259af',address=hex(at),kind=101,wrapper_hex=w.hex()))
 # (b) net task 0x7230f0 at 0x72317a (lea r14,[0x17b6368], 7 bytes; it is a jump target itself, nothing jumps into
 # 0x72317b..0x723180; right after the type-0x12 "99" store, where 1.19 0x31e014 shows the text): kind 102, replay the lea.
 if image[0x72317a:0x723181]!=b'\x4c\x8d\x35'+struct.pack('<i',0x17b6368-(0x72317a+7)):raise ValueError('net task text site mismatch')
 at,w=fullsave_stub(0x11b00,bytes.fromhex('4c89fe'),102,lambda h:b'\x4c\x8d\x35'+struct.pack('<i',0x17b6368-(h+7)),0x723181)
 image[0x72317a:0x723181]=b'\xe9'+struct.pack('<i',at-(0x72317a+5))+b'\x66\x90'
 wrappers.append(dict(site='0x72317a',address=hex(at),kind=102,wrapper_hex=w.hex()))
 # c005: PS3 1.19 taunt package wants (agc_port.inc). (a) PS4 match setup 0x72b570 at 0x72d12d (lea rbx,[0x145f92c]
 # "anim-proto-shield", 7 bytes, right after the anim-proto-multiplayer want = PS3 1.19 0x3204a8): fullsave kind 103, replay
 # the lea. (b) taunt controller Start 0xb85010 at its store 0xb85126 (mov [r14+0x128],esi, 7 bytes; a jump target from
 # 0xb8501f): fullsave kind 104 with r14, replay the store. No branch lands inside either displaced range (scanned).
 if image[0x72d12d:0x72d134]!=b'\x48\x8d\x1d'+struct.pack('<i',0x145f92c-(0x72d12d+7)):raise ValueError('match setup taunt site mismatch')
 at,w=fullsave_stub(0x11b80,b'',103,lambda h:b'\x48\x8d\x1d'+struct.pack('<i',0x145f92c-(h+7)),0x72d134)
 image[0x72d12d:0x72d134]=b'\xe9'+struct.pack('<i',at-(0x72d12d+5))+b'\x66\x90'
 wrappers.append(dict(site='0x72d12d',address=hex(at),kind=103,wrapper_hex=w.hex()))
 if image[0xb85126:0xb8512d]!=bytes.fromhex('4189b628010000'):raise ValueError('taunt start store mismatch')
 at,w=fullsave_stub(0x11c00,bytes.fromhex('4c89f6'),104,lambda h:bytes.fromhex('4189b628010000'),0xb8512d)
 image[0xb85126:0xb8512d]=b'\xe9'+struct.pack('<i',at-(0xb85126+5))+b'\x66\x90'
 wrappers.append(dict(site='0xb85126',address=hex(at),kind=104,wrapper_hex=w.hex()))
 # c006: PS3 1.19 player info 0x3a additions, per-net-player taunt wants, hat stats (agc_port.inc).
 P=struct.pack
 def agc_site(site,orig,at,w,kind,ln=None):
  ln=ln or len(orig)
  image[site:site+ln]=b'\xe9'+P('<i',at-(site+5))+b'\x90'*(ln-5)
  wrappers.append(dict(site=hex(site),address=hex(at),kind=kind,wrapper_hex=w.hex()))
 def agc_check(site,orig,name):
  if image[site:site+len(orig)]!=orig:raise ValueError(name+' site mismatch %#x'%site)
 # ===== fixer H stubs (x-stub slots 0x17a80 / 0x17b00 / 0x17b80, no kind, no helper call; switches ec[1] bits 1..2) =====
 # h002: MP character voices (PS3 1.19 f_1b0998 = PS4 0x4af2e0, vox controller vt+0x40 'pick the vox character from the parts').
 #   Both walk the character's attached meshes, hash each mesh name and look it up in the vox character map (vox.bin symbol
 #   0x821d111, {mesh name, mesh sid, vox character name, vox character sid}, e.g. mp-hero-roman-torso -> roman), then call
 #   vt+0x38 / PS3 vt+28 (SetVoxCharacter) with the result. PS3 1.19 passes the map's character as is (0x1b0b0c..0x1b0b30),
 #   or 0 when no mesh matches (0x1b0c34..0x1b0c54). PS4 (Collection SP rule) forces 'drake' (0x3b6fa41c) for any Player
 #   (type 0xa5a63e9b) unless the map says 'young-drake' (0x4af49e..0x4af4b7), and passes 'drake' instead of 0 for a Player
 #   with no match (0x4af462..0x4af472). So in MP every hero and villain speaks with Drake's lines.
 #   With MP active (byte 0x17d5c61) both paths take PS3 1.19's value. SP keeps PS4's rule.
 #   Fallback (h003: default OFF, U3MP_VOICE_FALLBACK=1 turns it on): the PS4 speech archive (sound1/streams/speech/<lang>_ps4.psarc)
 #   only has the MP lines of drake, talbot, cutter, marlowe, salim, rameses, elena, sullivan and chloe (drk/tal/ctr/mar/sal/
 #   ram/eln/sul/chl-mp-*). For a Player whose 1.19 character is not one of those (eddie, flynn, lazarevic, guerro, ...; their
 #   lines are only in PS3 english-mp.psarc, MP3 xvag, not yet ported) the stub keeps 'drake', so nobody goes silent.
 #   The fallback also keeps 'drake' for a Player none of whose meshes is in the map (1.19 passes 0 there).
 #   Diagnostics: ec[200] 1.19 picks (a mesh matched, MP), ec[201] last picked character sid, ec[202] fallbacks to drake,
 #   ec[203] no-match picks (MP).
 if os.environ.get('U3MP_VOICE119_STUB','1')!='0':
  s1o=bytes.fromhex('817d9c9b3ea6a57507be1ca46f3beb0231f6')
  s2o=bytes.fromhex('817d9c9b3ea6a50f94c13d9026cbee0f95c284cabe1ca46f3b0f44f0')
  agc_check(0x4af462,s1o,'h002 vox pick no-match');agc_check(0x4af49e,s2o,'h002 vox pick match')
  for k_ in range(3):
   a_,b_,o_=stub_slot(0x17a80+0x80*k_)
   if any(b_[o_:o_+0x80]):raise ValueError('stub slot in use %#x'%(0x17a80+0x80*k_))
  at,buf,o3=stub_slot(0x17a80)
  ec1=rw2+0x4454;ecd=rw2+0x4450+4*200;mpb=0x17d5c61;DRK=0x3b6fa41c
  w=bytearray();lab={};fx=[]
  def RIP(pre,tgt,tail=b''):
   n=len(pre)+4+len(tail);w.extend(pre+P('<i',tgt-(at+len(w)+n))+tail)
  def J8(op,l):w.extend(bytes([op,0]));fx.append((len(w)-1,l,1))
  def J32(op2,l):w.extend(op2+b'\0\0\0\0');fx.append((len(w)-4,l,4))
  def JA(tgt):w.extend(b'\xe9'+P('<i',tgt-(at+len(w)+5)))
  # path 1: no mesh matched (rax = vt+0x38, rdi = this)
  lab['p1']=len(w)
  RIP(b'\xf6\x05',ec1,b'\x02');J32(b'\x0f\x85','o1')       # test byte [ec1],2 (U3MP_VOICE119=0) ; jnz PS4
  RIP(b'\x80\x3d',mpb,b'\x00');J32(b'\x0f\x84','o1')       # cmp byte [MP active],0 ; je PS4
  RIP(b'\xff\x05',ecd+12)                                  # ec[203]++
  w.extend(b'\x31\xf6')                                    # PS3 1.19: SetVoxCharacter(0)
  RIP(b'\xf6\x05',ec1,b'\x04');J8(0x75,'b1')              # U3MP_VOICE_FALLBACK=0: strict 1.19
  w.extend(s1o[:7]);J8(0x75,'b1')                          # fallback: a Player with no mapped mesh keeps drake
  w.extend(b'\xbe'+P('<I',DRK))
  lab['b1']=len(w);JA(0x4af474)
  lab['o1']=len(w)
  w.extend(s1o[:7]);J8(0x75,'o1x');w.extend(b'\xbe'+P('<I',DRK));JA(0x4af474)
  lab['o1x']=len(w);w.extend(b'\x31\xf6');JA(0x4af474)
  # path 2: a mesh matched, eax = the map's character sid
  lab['p2']=len(w)
  RIP(b'\xf6\x05',ec1,b'\x02');J32(b'\x0f\x85','o2')
  RIP(b'\x80\x3d',mpb,b'\x00');J32(b'\x0f\x84','o2')
  RIP(b'\xff\x05',ecd);RIP(b'\x89\x05',ecd+4)              # ec[200]++, ec[201] = sid
  w.extend(b'\x89\xc6')                                    # PS3 1.19: esi = the map's character
  RIP(b'\xf6\x05',ec1,b'\x04');J32(b'\x0f\x85','b2')       # U3MP_VOICE_FALLBACK=0: strict 1.19
  w.extend(s1o[:7]);J32(b'\x0f\x85','b2')                  # Players only
  for s_ in (DRK,0xeecb2690,0xf13ce63c,0x821885c8,0xebd873ef,0x5ab817dd,0x78b0572c,0x3f283651,0x6ce7e451,0x3f334972):
   w.extend(b'\x3d'+P('<I',s_));J32(b'\x0f\x84','b2')      # drake young-drake talbot cutter marlowe salim rameses elena sullivan chloe
  RIP(b'\xff\x05',ecd+8);w.extend(b'\xbe'+P('<I',DRK))     # ec[202]++ ; no PS4 speech for it: keep drake
  lab['b2']=len(w);JA(0x4af4ba)
  lab['o2']=len(w);w.extend(s2o);JA(0x4af4ba)
  for pos,l,sz in fx:
   d_=lab[l]-(pos+sz)
   if sz==1:
    if not -128<=d_<=127:raise ValueError('h002 rel8 '+l)
    w[pos]=d_&0xff
   else:w[pos:pos+4]=P('<i',d_)
  assert len(w)<=0x180,len(w)
  buf[o3:o3+len(w)]=w
  image[0x4af462:0x4af474]=b'\xe9'+P('<i',at+lab['p1']-(0x4af462+5))+b'\x90'*13
  image[0x4af49e:0x4af4ba]=b'\xe9'+P('<i',at+lab['p2']-(0x4af49e+5))+b'\x90'*23
  wrappers.append(dict(site='0x4af462',address=hex(at+lab['p1']),kind='h002-vox-nomatch',wrapper_hex=w.hex()))
  wrappers.append(dict(site='0x4af49e',address=hex(at+lab['p2']),kind='h002-vox-match'))
 # h020 (opt-in diagnostic, U3MP_UI_BIND_DIAG=1; nothing is built by default): which UI pass binds the dark menu icons.
 #   The material apply 0xe90150 binds a UI material's own textures through 0xe66940, called at 0xe90050 ('call 0xe66940', 5 bytes,
 #   r8 = texture object array, r9d = count; [rbp+8] = 0xe901c1, [[rbp]+8] = End_Actual's 0xe41d59 on the UI path). Site -> pure asm
 #   pre-filter at x-stub 0x12a00 (ec[1] bit13 and both return addresses match, else straight to the native call) -> value stub
 #   kind 247 at 0x12a80 (helper h20_bind: classifies the pass by the frame above End_Actual: immediate menu draw, queue executor
 #   0xe3ffd0 called from 0xeef1c4 = UI packet list +0x8a0, or from 0xc1afeb / 0xc1b11a = HUD list +0x8a8, and counts sRGB / UNORM
 #   T#s per pass into ec[177..196]); then the native call, back at 0xe90055.
 if os.environ.get('U3MP_UI_BIND_DIAG','0')=='1':
  site=0xe90050
  if bytes(image[site:site+5])!=b'\xe8'+P('<i',0xe66940-(site+5)):raise ValueError('h020 site mismatch 0xe90050')
  vat,vw=value_stub(0x12a80,247,lambda h:b'\xe8'+P('<i',0xe66940-(h+5)),0xe90055)
  at,buf,o=stub_slot(0x12a00)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use 0x12a00')
  w=bytearray();ec1=rw2+0x4454
  def RIP(pre,tgt,tail=b''):
   n=len(pre)+4+len(tail);w.extend(pre+P('<i',tgt-(at+len(w)+n))+tail)
  RIP(b'\xf6\x05',ec1+1,b'\x20');w.extend(b'\x74\x00');j0=len(w)-1            # test byte [ec1+1],0x20 ; jz native
  w.extend(bytes.fromhex('5052'));RIP(b'\x48\x8d\x05',0)                         # push rax; push rdx; lea rax,[image base]
  w.extend(bytes.fromhex('488b55084829c24881fac101e900'));w.extend(b'\x75\x00');j1=len(w)-1   # [rbp+8]-base == 0xe901c1
  w.extend(bytes.fromhex('488b5500488b52084829c24881fa591de400'));w.extend(b'\x75\x00');j2=len(w)-1  # [[rbp]+8]-base == 0xe41d59
  w.extend(bytes.fromhex('5a58'));w.extend(b'\xe9'+P('<i',vat-(at+len(w)+5)))      # pop rdx; pop rax; jmp value stub
  lp=len(w);w.extend(bytes.fromhex('5a58'))                                       # pop rdx; pop rax
  ln=len(w);w.extend(b'\xe8'+P('<i',0xe66940-(at+len(w)+5)));w.extend(b'\xe9'+P('<i',0xe90055-(at+len(w)+5)))
  w[j0]=ln-(j0+1);w[j1]=lp-(j1+1);w[j2]=lp-(j2+1)
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[site:site+5]=b'\xe9'+P('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind='h020-ui-bind-filter',wrapper_hex=w.hex()))
  wrappers.append(dict(site='-',address=hex(vat),kind=247,wrapper_hex=vw.hex()))
 # ===== end fixer H stubs =====
 pinfo=os.environ.get('U3MP_PINFO119','1')!='0'
 # (1) message size 0xc0 -> 0xc8 (taunt at +0xc0): sender 0x6b7c0e and factory case 0x3a 0x6c44e6 (mov edi,0xc0).
 for a in (0x6b7c0e,0x6c44e6):
  agc_check(a,bytes.fromhex('bfc0000000'),'pinfo size')
  if pinfo:image[a:a+5]=bytes.fromhex('bfc8000000');wrappers.append(dict(site=hex(a),kind='pinfo-size-c8'))
 # (2) sender 0x6b846c (lea rdi,[0x17d6420], 7 bytes, before the send call 0x7a2ef0): fullsave kind 105, arg r13 = message.
 o=b'\x48\x8d\x3d'+P('<i',0x17d6420-(0x6b846c+7));agc_check(0x6b846c,o,'pinfo send')
 at,w=fullsave_stub(0x11c80,bytes.fromhex('4c89ee'),105,lambda h:b'\x48\x8d\x3d'+P('<i',0x17d6420-(h+7)),0x6b8473);agc_site(0x6b846c,o,at,w,105)
 # (3) serializer slot 2 read 0x70b3c0 / slot 3 write 0x709dc0 (entries: push rbp; mov rbp,rsp; push r15): value stubs kinds
 # 106 / 107; the trampoline (replayed entry + jmp back) is called by the helper, its image offset goes to ec[40] / ec[41].
 agc_tr=[]
 for site,slot,kind in ((0x70b3c0,0x11d00,106),(0x709dc0,0x11d80,107)):
  o=bytes.fromhex('554889e54157');agc_check(site,o,'pinfo serial')
  at,w=value_stub(slot,kind,lambda h:bytes.fromhex('554889e54157'),site+6);agc_site(site,o,at,w,kind)
  agc_tr.append(at+len(w)-11)
  assert w[len(w)-11:len(w)-5]==o
 # (4) process 0x709da0 (mov qword [r12+rbx+0xb78],0, 12 bytes): fullsave kind 108, arg r15 = message.
 o=bytes.fromhex('49c7841c780b000000000000');agc_check(0x709da0,o,'pinfo process')
 at,w=fullsave_stub(0x11e00,bytes.fromhex('4c89fe'),108,lambda h:o,0x709dac);agc_site(0x709da0,o,at,w,108)
 # (5) 1.19 0x310b98 taunt want, local net player: join point 0x72c21c (mov rbx,-0x6180, 7 bytes; target of 0x72c1af /
 # 0x72c1b8): fullsave kind 109, arg = [r12+0x370] (r12 = mgr since 0x72c19d).
 o=bytes.fromhex('48c7c3809effff');agc_check(0x72c21c,o,'np taunt local')
 at,w=fullsave_stub(0x11e80,bytes.fromhex('498bb42470030000'),109,lambda h:o,0x72c223);agc_site(0x72c21c,o,at,w,109)
 # (6) the two team loops: their look-want call (call 0x742a70 at 0x72c2ba / 0x72c88a) is redirected to a thunk that makes
 # the call (rsp kept ABI-aligned), then a full-save helper call (kind 110, arg = np = r14+rbx+0x6510) and returns.
 def fullsave_pre(off,pre,argmov,kind):
  at,buf,o2=stub_slot(off);w=bytearray()
  w+=bytes.fromhex('4883ec08');w+=b'\xe8'+P('<i',pre-(at+len(w)+5));w+=bytes.fromhex('4883c408')
  w+=bytes.fromhex('554889e5 50515256574150415141524153 4883e4f0 4881ec00020000 480fae0424')
  here=at+len(w);w+=b'\x48\x8d\x3d'+P('<i',-(here+7));w+=argmov;w+=b'\xba'+P('<I',kind)
  here=at+len(w);w+=b'\x48\x8d\x0d'+P('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+P('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d65b8 415b415a41594158 5f5e5a5958 5d c3')
  assert len(w)<=0x80,len(w)
  if any(buf[o2:o2+0x80]):raise ValueError('stub slot in use '+hex(off))
  buf[o2:o2+len(w)]=w
  return at,w
 for site,slot in ((0x72c2ba,0x11f00),(0x72c88a,0x11f80)):
  if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=0x742a70:raise ValueError('np taunt loop site mismatch %#x'%site)
  at,w=fullsave_pre(slot,0x742a70,bytes.fromhex('498db41e10650000'),110)
  image[site:site+5]=b'\xe8'+P('<i',at-(site+5))
  wrappers.append(dict(site=hex(site),address=hex(at),kind=110,wrapper_hex=w.hex()))
 # (7) hat stats: modifier rebuild 0x78f3b0 exit 0x78f8b3 (add rsp,8; pop rbx = 5 bytes; jump target, nothing jumps
 # inside): fullsave kind 112, arg r12 = net player.
 o=bytes.fromhex('4883c4085b');agc_check(0x78f8b3,o,'hat mods')
 at,w=fullsave_stub(0x12000,bytes.fromhex('4c89e6'),112,lambda h:o,0x78f8b8);agc_site(0x78f8b3,o,at,w,112)
 # (8) booster skip: 0x78f589 (mov eax,[rdx+rcx+4]; test eax,eax; je 0x78f670 = 12 bytes, rax = booster record): value stub
 # kind 113 (always returns 0), replay with the je re-targeted.
 o=bytes.fromhex('8b440a0485c00f84')+P('<i',0x78f670-(0x78f589+12));agc_check(0x78f589,o,'booster skip')
 at,w=value_stub(0x12080,113,lambda h:bytes.fromhex('8b440a0485c00f84')+P('<i',0x78f670-(h+12)),0x78f595);agc_site(0x78f589,o,at,w,113)
 # c007: PS3 1.19 spotter flags per local screen (agc_port.inc). (1) spotter loop: 0x78ecc7 (lea rdi,[0x17c3b78], 7 bytes,
 # before the radius query; r13 = spotter) -> fullsave kind 114, continues at 0x78ee54 (after PS4's loop; r12/rbx/[rbp-0xc20]
 # are reloaded or unused from there).
 o=b'\x48\x8d\x3d'+P('<i',0x17c3b78-(0x78ecc7+7));agc_check(0x78ecc7,o,'spotter loop')
 at,w=fullsave_stub(0x12100,bytes.fromhex('4c89ee'),114,lambda h:b'',0x78ee54);agc_site(0x78ecc7,o,at,w,114)
 # (2) flag[1] clears, kind 115: net task 0x7262a4 (lea rax,[0x17cb060], 7 bytes, right before PS4's unrolled clears; arg 0)
 # and player reset 0x782ed4 (mov byte [rbx+0x9b1],0; arg rbx).
 o=b'\x48\x8d\x05'+P('<i',0x17cb060-(0x7262a4+7));agc_check(0x7262a4,o,'spot clear net')
 at,w=fullsave_stub(0x12180,bytes.fromhex('31f6'),115,lambda h:b'\x48\x8d\x05'+P('<i',0x17cb060-(h+7)),0x7262ab);agc_site(0x7262a4,o,at,w,115)
 o=bytes.fromhex('c683b109000000');agc_check(0x782ed4,o,'spot clear reset')
 at,w=fullsave_stub(0x12200,bytes.fromhex('4889de'),115,lambda h:o,0x782edb);agc_site(0x782ed4,o,at,w,115)
 # (3) the six native readers, kind 116 (arg = the tested player); the helper leaves the flag of the screen being drawn in
 # ec[54] and the stub replays the reader's instruction on that byte (rip-relative).
 RES=rw2+0x4450+4*54
 for site,orig,arg,slot,rep in ((0xbf2aa2,'80bbb109000000','4889de',0x12280,lambda h:b'\x80\x3d'+P('<i',RES-(h+7))+b'\x00'),
                                (0xc2cebe,'80b8b109000000','4889c6',0x12300,lambda h:b'\x80\x3d'+P('<i',RES-(h+7))+b'\x00'),
                                (0xc2d28f,'4180bfb109000000','4c89fe',0x12380,lambda h:b'\x80\x3d'+P('<i',RES-(h+7))+b'\x00'),
                                (0xc2d527,'458aafb1090000','4c89fe',0x12400,lambda h:b'\x44\x8a\x2d'+P('<i',RES-(h+7))),
                                (0xc2db20,'4180bfb109000000','4c89fe',0x12480,lambda h:b'\x80\x3d'+P('<i',RES-(h+7))+b'\x00'),
                                (0xc329a7,'80b8b109000000','4889c6',0x12500,lambda h:b'\x80\x3d'+P('<i',RES-(h+7))+b'\x00')):
  o=bytes.fromhex(orig);agc_check(site,o,'spot reader')
  at,w=fullsave_stub(slot,bytes.fromhex(arg),116,rep,site+len(o));agc_site(site,o,at,w,116)
 # c009: PS3 1.19 net anim decoder uses (agc_port.inc). regs_stub: like fullsave_stub, but it also saves rbx and r12..r15 and
 # passes rsi = the saved registers {r15,r14,r13,r12,rbx,r11,r10,r9,r8,rdi,rsi,rdx,rcx,rax} (rbp-0x70).
 def regs_stub(off,kind,replay,back):
  at,buf,o=stub_slot(off)
  w=bytearray.fromhex('554889e5 50515256574150415141524153 53 4154 4155 4156 4157 488d7590 4883e4f0 4881ec00020000 480fae0424')
  here=at+len(w);w+=b'\x48\x8d\x3d'+P('<i',-(here+7))
  w+=b'\xba'+P('<I',kind)
  here=at+len(w);w+=b'\x48\x8d\x0d'+P('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+P('<i',helper-(here+5))
  w+=bytes.fromhex('480fae0c24 488d6590 415f415e415d415c5b 415b415a41594158 5f5e5a5958 5d')
  w+=replay(at+len(w))
  here=at+len(w);w+=b'\xe9'+P('<i',back-(here+5))
  assert len(w)<=0x80,len(w)
  if any(buf[o:o+0x80]):raise ValueError('stub slot in use '+hex(off))
  buf[o:o+len(w)]=w
  return at,w
 # (1) generator init 0x682ea0 at 0x682f0b (mov rax,[r12+0x48], 5 bytes, right after PS4's 8 zero stores): fullsave kind 120,
 # arg r12 = generator (1.19 f_29f874 fills the tables with f_3868f0 there).
 o=bytes.fromhex('498b442448');agc_check(0x682f0b,o,'gen tables')
 at,w=fullsave_stub(0x12580,bytes.fromhex('4c89e6'),120,lambda h:o,0x682f10);agc_site(0x682f0b,o,at,w,120)
 # (2) apply 0x683670 at 0x683c61 (mov eax,[rbx+0xa8], 6 bytes; the target of the four handle-check jumps; rbx = generator,
 # r12 = snapshot offset): regs stub kind 117, replay.
 o=bytes.fromhex('8b83a8000000');agc_check(0x683c61,o,'apply decode')
 at,w=regs_stub(0x12600,117,lambda h:o,0x683c67);agc_site(0x683c61,o,at,w,117)
 # (3) CreateAnimCmds 0x6847d0 / (4) GetLocator 0x684cf0 entries (push rbp; mov rbp,rsp; push r15): value stubs 118 / 119.
 for site,slot,kind in ((0x6847d0,0x12680,118),(0x684cf0,0x12700,119)):
  o=bytes.fromhex('554889e54157');agc_check(site,o,'net anim gen entry')
  at,w=value_stub(slot,kind,lambda h:bytes.fromhex('554889e54157'),site+6);agc_site(site,o,at,w,kind)
 # (5) NPC sender 0x6ae2a0 at 0x6ae57f (mov dword [rbx+0x144],0, 10 bytes, right after vt+0x698; rbx = message, r15 = NPC,
 # rax = table holder): regs stub kind 121; the helper stores the bit count, nothing is replayed.
 o=bytes.fromhex('c7834401000000000000');agc_check(0x6ae57f,o,'npc bits')
 at,w=regs_stub(0x12780,121,lambda h:b'',0x6ae589);agc_site(0x6ae57f,o,at,w,121)
 # c010: PS3 1.19 hat-stat icons on the killed-by card (agc_port.inc), HudNet::Draw 0xc32ba0 (slots 0x13c00..0x13d00).
 # (1) icon count 0xc38989..0xc38999 (cmp qword [rbp-0x11e0],0; setne cl; movzx ecx,cl; add ecx,eax = 16 bytes, then je):
 # regs stub kind 122 computes PS4's sum (eax + ([rbp-0x11e0] != 0)) plus the 1.19 icon count into ec[105]; the replay is
 # mov ecx,[ec105]; test ecx,ecx (ZF for the je).
 EC105=rw2+0x4450+4*105;EC106=rw2+0x4450+4*106
 o=bytes.fromhex('4883bd20eeffff000f95c10fb6c901c1');agc_check(0xc38989,o,'hat icon count')
 at,w=regs_stub(0x13c00,122,lambda h:b'\x8b\x0d'+P('<i',EC105-(h+6))+b'\x85\xc9',0xc38999);agc_site(0xc38989,o,at,w,122)
 # x028 co-op voice diagnostic (scratch/coop-voice2/edits_v736_voxdiag.py): regs stubs on the vox controller's SayRequest
 #   0x4af5f0 (kind 170, entry: push rbp; mov rbp,rsp; push r15 replayed), its common exit 0x4af6c6 (kind 171: mov rax,[r13];
 #   cmp rax,[rbp-0x30] replayed, r14d = reject code, r12b = result) and the script say-event handler 0x4b3610 (kind 172).
 if os.environ.get('U3MP_VOXDIAG','1')!='0':
  for site,o_,slot,kind,back in ():
   ob=bytes.fromhex(o_);agc_check(site,ob,'x028 vox diag %#x'%site)
   at,w=regs_stub(slot,kind,(lambda h,ob=ob:ob),back);agc_site(site,ob,at,w,kind)
 # (2) 0xc3a945 (cmp qword [rbp-0x11e0],0, 8 bytes; xmm0 = the kickback icon's slot (x, y, w, h)): save xmm0 to ec[106..109]
 # (movups), replay, back to 0xc3a94d (the je keeps the cmp's flags).
 o=bytes.fromhex('4883bd20eeffff00');agc_check(0xc3a945,o,'hat icon rect')
 at,buf,o2=stub_slot(0x13c80)
 w=bytearray(b'\x0f\x11\x05'+P('<i',EC106-(at+7)))+o
 here=at+len(w);w+=b'\xe9'+P('<i',0xc3a94d-(here+5))
 if any(buf[o2:o2+0x80]):raise ValueError('stub slot in use 0x13c80')
 buf[o2:o2+len(w)]=w;agc_site(0xc3a945,o,at,w,'123-rect')
 # (3) 0xc3aa8d (lea rax,[0x18c6280], 7 bytes; the join after the kickback icon): regs stub kind 123 draws the icons.
 o=b'\x48\x8d\x05'+P('<i',0x18c6280-(0xc3aa8d+7));agc_check(0xc3aa8d,o,'hat icon draw')
 at,w=regs_stub(0x13d00,123,lambda h:b'\x48\x8d\x05'+P('<i',0x18c6280-(h+7)),0xc3aa94);agc_site(0xc3aa8d,o,at,w,123)
 # Hat Stats lines: Descriptor draw 0x61a2b0 (type 0x25), PS3 1.19 f_217000
 o=bytes.fromhex('4989c74d85ff');agc_check(0x61a402,o,'hs text')
 at,w=regs_stub(0x12e00,249,lambda h:o,0x61a408);agc_site(0x61a402,o,at,w,249)
 o=bytes.fromhex('498b06483b45d0');agc_check(0x61a638,o,'hs icons')
 at,w=regs_stub(0x12e80,250,lambda h:o,0x61a63f);agc_site(0x61a638,o,at,w,250)
 # UI LOD bias: pixel-stage sampler binds of End_Actual, kinds 254 / 255
 if os.environ.get('U3MP_UI_BIAS','1')!='0':
  o=bytes.fromhex('498b9424e0250000');agc_check(0xe9001c,o,'ui bias table');at,w=regs_stub(0x13480,254,lambda h:b'',0xe90024);agc_site(0xe9001c,o,at,w,254)
  o=bytes.fromhex('488b4b084c89ff');agc_check(0xe90072,o,'ui bias handles');at,w=regs_stub(0x13500,255,lambda h:bytes.fromhex('4c89ff'),0xe90079);agc_site(0xe90072,o,at,w,255)
 # x105/x106: kinds 251..253
 o=bytes.fromhex('4c8d85c0feffff')
 agc_check(0x61ad7f,o,'eu L');at,w=regs_stub(0x12f00,252,lambda h:b'',0x61ad86);agc_site(0x61ad7f,o,at,w,252)
 agc_check(0x61ae75,o,'eu R');at,w=regs_stub(0x12f80,253,lambda h:b'',0x61ae7c);agc_site(0x61ae75,o,at,w,253)
 for st_,sl_,hx_ in ((0x6a6e52,0x13300,'e8e9aafaff'),(0x6a63ed,0x13380,'e84eb5faff'),(0x6a63fe,0x13400,'e83db5faff')):
  o=bytes.fromhex(hx_);agc_check(st_,o,'mm show');at,w=regs_stub(sl_,251,lambda h:b'',st_+5);agc_site(st_,o,at,w,251)
 # v7.36 ci01: co-op buddy marker icon, kind 219 at the two HudNet+0x10e0 icon loads in the marker draw (0xc30f79 / 0xc31010:
 #   mov rsi,[rbx+0x10e0]; mov edx,[rbx+0x10e8] = 13 bytes, no jump into them; nothing replayed, the helper sets rsi / rdx).
 o=bytes.fromhex('488bb3e01000008b93e8100000')
 for st_,sl_ in ((0xc30f79,0x13680),(0xc31010,0x13700)):
  agc_check(st_,o,'coop icon');at,w=regs_stub(sl_,219,lambda h:b'',st_+13);agc_site(st_,o,at,w,219)
 # coop-score cs02 (scratch/coop-score2): co-op cash shown, not awarded. (1) 0x7270f0 entry (push rbp; mov rbp,rsp; push r15;
 # push r14 = 8 bytes, no branch into them): value stub kind 80, slot 0x14200, back 0x7270f8; the helper reads the caller's
 # return address ([rbp+8] of the stub frame) and forces 1 at four display / match-block sites only. (2) 0x7abb70 entry (same
 # 8 bytes): regs stub kind 81, slot 0x14280, back 0x7abb78 (refreshes ec[245], counts). (3) 0x7abc96 (mov rax,r12; sub rax,
 # [rax+0x370] = 10 bytes, the local-player test; no branch targets 0x7abc97..0x7abc9f in the disassembly): slot 0x14a00, no
 # helper call: test byte [ec245],1; jne -> jmp 0x7abcc2 (the remote-player path: match block only); else the two displaced
 # instructions and jmp 0x7abca0. U3MP_COOP_CASH=0 = bit 0x400 (nothing forced), U3MP_COOP_CASH_V738=1 = bit 0x800 (v7.38).
 o=bytes.fromhex('554889e541574156');agc_check(0x7270f0,o,'coop-score ranked gate')
 at,w=value_stub(0x14200,80,lambda h:o,0x7270f8);agc_site(0x7270f0,o,at,w,80)
 agc_check(0x7abb70,o,'coop-score cash add')
 at,w=regs_stub(0x14280,81,lambda h:o,0x7abb78);agc_site(0x7abb70,o,at,w,81)
 EC245=rw2+0x4450+4*245
 o=bytes.fromhex('4c89e0482b8070030000');agc_check(0x7abc96,o,'coop-score local test')
 at,buf,o2=stub_slot(0x14a00)
 if any(buf[o2:o2+0x80]):raise ValueError('stub slot in use 0x14a00')
 w=bytearray(b'\xf6\x05'+P('<i',EC245-(at+7))+b'\x01')      # test byte [rip+ec245],1
 j=len(w);w+=b'\x75\x00'                                      # jne remote
 w+=o;w+=b'\xe9'+P('<i',0x7abca0-(at+len(w)+5))               # local test as shipped, back
 w[j+1]=len(w)-(j+2)
 w+=b'\xe9'+P('<i',0x7abcc2-(at+len(w)+5))                    # remote: block only
 assert len(w)<=0x80
 buf[o2:o2+len(w)]=w;agc_site(0x7abc96,o,at,w,'cs02-remote')
 if os.environ.get('U3MP_COOP_CASH_V738','0')=='1':image[state+0x20c0]|=0x800
 # ldn01 (scratch/london/edits_ldn01.py): London draw-gate diagnostic, kinds 82..87, slots 0x14300..0x14580, report
 # /data/u3-ldn-report.txt. No branch targets inside any displaced range (checked against the full disassembly).
 # ldn01 hooks removed in v7.40 (ext code room for nl01); slots 0x14300..0x14580 and kinds 82..87 are free again.
 if os.environ.get('U3MP_COOP_CASH','1')=='0':image[state+0x20c0]|=0x400
 # c011: M3 player emblem (agc_port.inc), x-stub slots 0x13d80..0x13f00, kinds 124..127.
 # (1) NetCharacter look 0x6a5c50 (= PS3 f_2ba184, vtable slot 18; push rbp; mov rbp,rsp; push r15 = 6 bytes, function entry):
 # value stub kind 124 (the helper binds the emblem record textures and returns 1 = return; 0 = PS4's own body).
 o=bytes.fromhex('554889e54157');agc_check(0x6a5c50,o,'emblem look')
 at,w=value_stub(0x13d80,124,lambda h:o,0x6a5c56);agc_site(0x6a5c50,o,at,w,124)
 # (2) emblem editor datasources 0x76a610 (= PS3 f_33afa0; push rbp; mov rbp,rsp; push rbx; push rax) and 0x7699e0 (= PS3
 # f_33ee48; push rbp; mov rbp,rsp; push r15): value stubs 125 / 126, event 9 -> f_27d790, then the original handler.
 o=bytes.fromhex('554889e55350');agc_check(0x76a610,o,'emblem editor ds A')
 at,w=value_stub(0x13e00,125,lambda h:o,0x76a616);agc_site(0x76a610,o,at,w,125)
 o=bytes.fromhex('554889e54157');agc_check(0x7699e0,o,'emblem editor ds B')
 at,w=value_stub(0x13e80,126,lambda h:o,0x7699e6);agc_site(0x7699e0,o,at,w,126)
 # (3) match setup 0x72b570 at 0x72bdf1 (test r14b,r14b; je short 0x72be34 = 5 bytes; r14b = PS3 r23, [rbp-0x264] = PS3 r18):
 # PS3 f_31ef18 0x31fab4..0x31fad4 calls f_27d790 when r23 == 0 and r18 != 0 before the common tail (PS4 0x72be34). The stub
 # keeps PS4's branch (r23 != 0 -> 0x72bdf6), tests r18 and calls kind 127 (full save) before jumping to 0x72be34.
 o=bytes.fromhex('4584f6743e');agc_check(0x72bdf1,o,'emblem match setup')
 at,buf,o3=stub_slot(0x13f00)
 if any(buf[o3:o3+0x80]):raise ValueError('stub slot in use 0x13f00')
 w=bytearray(bytes.fromhex('4584f6'))
 here=at+len(w);w+=b'\x0f\x85'+P('<i',0x72bdf6-(here+6))
 w+=bytes.fromhex('83bd9cfdffff00')
 here=at+len(w);w+=b'\x0f\x84'+P('<i',0x72be34-(here+6))
 w+=bytes.fromhex('554889e5 50515256574150415141524153 4883e4f0 4881ec00020000 480fae0424')
 here=at+len(w);w+=b'\x48\x8d\x3d'+P('<i',-(here+7))
 w+=bytes.fromhex('31f6')+b'\xba'+P('<I',127)
 here=at+len(w);w+=b'\x48\x8d\x0d'+P('<i',state-(here+7))
 here=at+len(w);w+=b'\xe8'+P('<i',helper-(here+5))
 w+=bytes.fromhex('480fae0c24 488d65b8 415b415a41594158 5f5e5a5958 5d')
 here=at+len(w);w+=b'\xe9'+P('<i',0x72be34-(here+5))
 assert len(w)<=0x80,len(w)
 buf[o3:o3+len(w)]=w;agc_site(0x72bdf1,o,at,w,127)
 # c012: unlock condition type 6 = DLC pack installed. The case is in PS4's evaluator 0x7bb5b0 (jump table 0x7bba48,
 # entry 6 -> 0x7bb708): movsxd rax,[rbx+8]; lea rcx,0x17c3930; cmp byte [rcx+rax+0x110],0; setne al; jmp 0x7bb5f3.
 # That is a byte array of 0x100 packs (PS3 1.10 f_29c500 layout, ends at BoosterMgr 0x17c3b40). PS3 1.19 f_2a2adc reads
 # a 1024-bit array at the same mgr+0x110, and the 1.19 unlock data uses packs 278..535.
 # Stub (slot 0x13f80, kind 128, fullsave): rsi = rbx (the cond {type, sid, value}); the helper's al becomes the result;
 # then the original tail 0x7bb5f3. The 5 patched bytes (48 63 43 08 48) are only reached through the jump table.
 o=bytes.fromhex('4863430848');agc_check(0x7bb708,o,'unlock cond 6')
 at,buf,o3=stub_slot(0x13f80)
 if any(buf[o3:o3+0x80]):raise ValueError('stub slot in use 0x13f80')
 w=bytearray(bytes.fromhex('554889e5 50515256574150415141524153 4883e4f0 4881ec00020000 480fae0424'))
 here=at+len(w);w+=b'\x48\x8d\x3d'+P('<i',-(here+7))
 w+=bytes.fromhex('4889de')+b'\xba'+P('<I',128)
 here=at+len(w);w+=b'\x48\x8d\x0d'+P('<i',state-(here+7))
 here=at+len(w);w+=b'\xe8'+P('<i',helper-(here+5))
 w+=bytes.fromhex('8845f8')   # mov [rbp-8],al: the saved rax slot, so the pops hand al back
 w+=bytes.fromhex('480fae0c24 488d65b8 415b415a41594158 5f5e5a5958 5d')
 here=at+len(w);w+=b'\xe9'+P('<i',0x7bb5f3-(here+5))
 assert len(w)<=0x80,len(w)
 buf[o3:o3+len(w)]=w;agc_site(0x7bb708,o,at,w,128)
 # c013: per-instance texture tables. Each obj+0x240+8i table is 0x40 bytes, zeroed by 0x10b86f9 (alloc 0x40):
 # PS3 f_854140 / 0x85a0f8 = u8 type[16] + 48-byte GCM descriptors; PS4 = 16 bytes + 6 Ndgi texture pointers at +0x10+8*slot,
 # slot = obj+0x258 (the count 0x10c37e0 zeroes each frame). PS4's tint pass 0x10c38a0 writes slot 0 (+0x10) whatever the
 # count, because PS4's look 0x6a5c50 never added a slot. PS3 0x85a0f8 writes the tint at the current count, after the look's
 # emblem slot. The stub (slot 0x16000, no helper call) writes [table+0x10+8*count] when count <= 5 (no write past the
 # 0x40-byte table). With U3MP_EMBLEM119=0 (ec[0] bit14) it keeps PS4's own +0x10 write.
 o=bytes.fromhex('498b84dc400200004c897010');agc_check(0x10c38a0,o,'instance tint slot')
 at,buf,o3=stub_slot(0x16000)
 if any(buf[o3:o3+0x80]):raise ValueError('stub slot in use 0x16000')
 w=bytearray(bytes.fromhex('498b84dc40020000'))          # mov rax,[r12+rbx*8+0x240]
 here=at+len(w);w+=b'\xf6\x05'+P('<i',(rw2+0x4451)-(here+7))+b'\x40'   # test byte [ec0+1],0x40 (EMBLEM119 off)
 w+=b'\x75\x17'                                            # jnz orig (+0x17: over movsxd 8, cmp 3, ja 2, mov 5, jmp 5)
 w+=bytes.fromhex('49638c2458020000')                     # movsxd rcx,[r12+0x258]
 w+=bytes.fromhex('83f905')+b'\x77\x05'                  # cmp ecx,5; ja +5 (skip the store, straight to jmp back)
 w+=bytes.fromhex('4c8974c810')                           # mov [rax+rcx*8+0x10],r14
 here=at+len(w);w+=b'\xe9'+P('<i',0x10c38ac-(here+5))    # jmp back
 assert len(w)==8+7+2+0x17
 w+=bytes.fromhex('4c897010')                             # orig: mov [rax+0x10],r14
 here=at+len(w);w+=b'\xe9'+P('<i',0x10c38ac-(here+5))
 assert len(w)<=0x80,len(w)
 buf[o3:o3+len(w)]=w;agc_site(0x10c38a0,o,at,w,'c013-tint-slot')
 # c014: null texture guard + locator in PS4's GPU resource-table writer 0xe66940 (the v5.84 Profile > Emblem fault:
 # 0xe66a99 mov edi,[rsi] with rsi = 0, rsi = texture[i] at 0xe66a67). The 2D UI packet executor 0xe41c69 (packet types
 # 0x303b / 0x303c, built by 0xe428e0) fills a 3-entry texture array from the packet's handles {+8, +0x10, +0x18} and leaves
 # absent handles 0 (0xe41c60 zeroes it, 0xe41cbe / 0xe41cef skip); 0xe8f130 / 0xe66940 then read as many entries as the
 # shader has textures (+0x24), so a packet with fewer handles than its shader reads a 0 entry. The stub (slot 0x16080, no
 # helper call; up to 0x180 bytes = slots 0x16080, 0x16100, 0x16180) replaces texture[i] == 0 by the helper's permanent 4x4 transparent texture (ec[158..159], made by emb_fb119)
 # and records: ec[147] count, ec[148..151] return addresses of 4 frames (rbp chain, each checked to lie on this stack:
 # 0xe90055 / 0xe901c1 / 0xe41d59 / 0xe400a8 for the UI packet path), ec[152] handle 1, ec[153] / ec[154] its name chars
 # 0..3 / 8..11 (+0x20 / +0x28: the texture-cache entry name), ec[155] handle 2, ec[156] textures left in the loop (r12d), ec[157] the
 # packet's shader (low). Packet = the 0xe41a60 r13 that 0xe90150 saved at its rbp-0x18. Original 7 bytes:
 # mov rsi,[r13]; mov rbx,rsi.
 o=bytes.fromhex('498b75004889f3');agc_check(0xe66a67,o,'texture list read')
 at,buf,o3=stub_slot(0x16080)
 if any(buf[o3:o3+0x180]):raise ValueError("stub slot in use 0x16080")
 EC=lambda k:rw2+0x4450+4*k
 w=bytearray(bytes.fromhex('498b7500 4885f6'))           # mov rsi,[r13]; test rsi,rsi
 j_ok=len(w);w+=b'\x0f\x85'+b'\0\0\0\0'                  # jnz ok
 w+=bytes.fromhex('505251')                               # push rax; push rdx; push rcx
 def rip(op,k):
  here=at+len(w);w.extend(op+P('<i',EC(k)-(here+len(op)+4)))
 rip(b'\xff\x05',147)                                     # inc dword [ec147]
 rip(b'\x44\x89\x25',156)                                 # mov [ec156],r12d
 rip(b'\x48\x8b\x35',158)                                 # mov rsi,[ec158] (fallback texture, 0 until made)
 w+=bytes.fromhex('4889e8')                               # mov rax,rbp
 jd=[]
 for k in range(4):
  w+=bytes.fromhex('4839e0');jd.append(len(w));w+=b'\x0f\x82'+b'\0\0\0\0'   # cmp rax,rsp; jb done
  w+=bytes.fromhex('488d8c2400001000 4839c8');jd.append(len(w));w+=b'\x0f\x83'+b'\0\0\0\0'   # lea rcx,[rsp+1M]; cmp; jae done
  w+=bytes.fromhex('8b5008');rip(b'\x89\x15',148+k)       # mov edx,[rax+8]; mov [ec148+k],edx
  if k==2:
   w+=bytes.fromhex('488b48e8 4885c9')                    # mov rcx,[rax-0x18] (packet); test rcx,rcx
   jp=len(w);w+=b'\x0f\x84'+b'\0\0\0\0'                   # jz next
   w+=bytes.fromhex('8b11');rip(b'\x89\x15',157)          # mov edx,[rcx]; [ec157]
   w+=bytes.fromhex('488b5110');rip(b'\x89\x15',155)      # mov rdx,[rcx+0x10] (h2); [ec155]
   w+=bytes.fromhex('488b5108');rip(b'\x89\x15',152)      # mov rdx,[rcx+8] (h1); [ec152]
   w+=bytes.fromhex('4885d2');jz2=len(w);w+=b'\x0f\x84'+b'\0\0\0\0'   # test rdx,rdx; jz next
   w+=bytes.fromhex('8b4a20');rip(b'\x89\x0d',153)        # mov ecx,[rdx+0x20]; [ec153]
   w+=bytes.fromhex('8b4a28');rip(b'\x89\x0d',154)        # mov ecx,[rdx+0x28]; [ec154] (name chars 8..11: after "emblems/")
   nx=len(w)
   for p in (jp,jz2):w[p+2:p+6]=P('<i',nx-(p+6))
  w+=bytes.fromhex('488b00')                              # mov rax,[rax]
 done=len(w)
 for p in jd:w[p+2:p+6]=P('<i',done-(p+6))
 w+=bytes.fromhex('595a58')                               # pop rcx; pop rdx; pop rax
 ok=len(w);w[j_ok+2:j_ok+6]=P('<i',ok-(j_ok+6))
 w+=bytes.fromhex('4889f3')                               # ok: mov rbx,rsi
 here=at+len(w);w+=b'\xe9'+P('<i',0xe66a6e-(here+5))    # jmp back
 assert len(w)<=0x180,len(w)
 buf[o3:o3+len(w)]=w;agc_site(0xe66a67,o,at,w,'c014-tex-guard')
 # ===== end agent C code sites =====
 # ===== agent A a009 code sites: PS3 1.19 Legacy 4/5 (helper legacy_kind, kinds 60..65; x-stub slots 0x10f00..0x11180) =====
 if os.environ.get('U3MP_LEGACY119','1')!='0':
  PA=struct.pack
  def a9_check(site,orig,name):
   if bytes(image[site:site+len(orig)])!=orig:raise ValueError('a009 '+name+' site mismatch %#x'%site)
  # (1) caps. can-legacy 0x781da0 = 1.19 f_365674 (0x3656e0 cmplwi legacy,4): cmp [rbx+0x1860],3; setb -> 5.
  a9_check(0x781e7e,bytes.fromhex('83bb6018000003'),'can-legacy');image[0x781e84]=5
  # legacy reset 0x7814xx = 1.19 f_366b60 (legacy < 5): cmp ebx,2; jbe -> cmp ebx,4.
  a9_check(0x781512,bytes.fromhex('83fb0276'),'legacy reset');image[0x781514]=4
  # Legacy button (OnButton 0xa0bacf62, 1.19 0x26b??? < 5 at 0x269c70+): cmp [r14+rbx+0x1860],3; jb -> 5.
  a9_check(0x64e5f7,bytes.fromhex('4183bc1e601800000372'),'legacy button');image[0x64e5ff]=5
  # HUD rank art 0xc22f70 (= 1.10 f_64dfbc 0x64e0c8 / 1.19 0x660880 legacy-1 <= 4): cmp eax,4; cmovb (prestige art) -> 6.
  a9_check(0xc230fb,bytes.fromhex('83f804'),'hud prestige');image[0xc230fd]=6
  wrappers.append(dict(site='0x781e7e,0x781512,0x64e5f7,0xc230fb',kind='a009-legacy-caps'))
  # (2) function thunks: helper(base, ev, kind, state) with ev[0..2] = rdi/rsi/rdx, ev[3] = result -> rax.
  def a9_fn(at,kind):
   w=bytearray.fromhex('554889e5 4883ec20 48893c24 4889742408 4889542410 48c744241800000000')
   here=at+len(w);w+=b'\x48\x8d\x3d'+PA('<i',-(here+7))
   w+=bytes.fromhex('4889e6')+b'\xba'+PA('<I',kind)
   here=at+len(w);w+=b'\x48\x8d\x0d'+PA('<i',state-(here+7))
   here=at+len(w);w+=b'\xe8'+PA('<i',helper-(here+5))
   w+=bytes.fromhex('488b442418 c9 c3')
   return w
  def a9_slot(off):
   at,buf,o=stub_slot(off)
   if any(buf[o:o+0x80]):raise ValueError('a009 stub slot in use '+hex(off))
   return at,buf,o
  # entries (all start with push rbp; mov rbp,rsp; push r15; ... and are only entered at their first byte):
  for site,off,kind,name in ((0x662760,0x10f00,60,'cur level'),(0x662950,0x10f80,62,'tier'),(0x662ba0,0x11000,63,'xp gate'),(0x781ea0,0x11080,64,'rank value')):
   a9_check(site,bytes.fromhex('554889e54157'),name)
   at,buf,o=a9_slot(off)
   w=a9_fn(at,kind);assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
   image[site:site+6]=b'\xe9'+PA('<i',at-(site+5))+b'\x90'
   wrappers.append(dict(site=hex(site),address=hex(at),kind=kind,wrapper_hex=w.hex()))
  # (3) level-from-XP with a legacy (1.19 f_5c210): E = function thunk kind 61 (rdi mgr, esi xp, edx legacy; -1 = the menu
  # user's profile legacy, as f_20abf4 caches it at obj+0xa0), and per-site pre-thunks that load edx and jump to E.
  at,buf,o=a9_slot(0x11100)
  E=at;w=a9_fn(at,61);assert len(w)<=0x80
  tA=len(w);w+=bytes.fromhex('430fb6942c98050000')+b'\xe9'+PA('<i',E-(at+len(w)+9+5))   # movzx edx,byte [r12+r13+0x598] (1.19 np+272)
  tM=len(w);w+=b'\xba'+PA('<I',0xffffffff)+b'\xe9'+PA('<i',E-(at+len(w)+5+5))           # mov edx,-1 (menu user)
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  for site,thunk,name in ((0x7abe1e,at+tA,'award 1'),(0x7abe36,at+tA,'award 2'),(0x618d13,at+tM,'post-match init'),(0x6188df,at+tM,'post-match update')):
   if image[site]!=0xe8 or site+5+struct.unpack_from('<i',image,site+1)[0]!=0x6625a0:raise ValueError('a009 '+name+' call mismatch %#x'%site)
   image[site:site+5]=b'\xe8'+PA('<i',thunk-(site+5))
   wrappers.append(dict(site=hex(site),address=hex(thunk),kind=61,wrapper_hex=w.hex()))
  # (4) post-match draw 0x617c20 (= 1.19 f_2180b4 0x2182fc..0x218348): the two inlined thresholds of level [r14+0xbc] (ebx) at
  # 0x617ff7..0x618082 -> kind 65 with the menu user's legacy; r12d = t0, [rbp-0x290] = t1, then the vmovaps xmm1 both paths
  # run and back to 0x618083. Only callee-saved registers are live across the replaced block (it contains calls); nothing
  # outside jumps into 0x617ffc..0x618082.
  a9_check(0x617ff7,bytes.fromhex('bf9d195ec8e8'),'post-match draw');a9_check(0x618083,bytes.fromhex('4d8d6f08'),'post-match join')
  vm=bytes(image[0x61804b:0x618053]);assert vm==bytes(image[0x61807b:0x618083]) and vm[:2]==bytes.fromhex('c5f8'),vm.hex()
  at,buf,o=a9_slot(0x11180)
  w=bytearray.fromhex('4989e4 4883e4f0 4883ec20 48891c24')
  here=at+len(w);w+=b'\x48\x8d\x3d'+PA('<i',-(here+7))
  w+=bytes.fromhex('4889e6')+b'\xba'+PA('<I',65)
  here=at+len(w);w+=b'\x48\x8d\x0d'+PA('<i',state-(here+7))
  here=at+len(w);w+=b'\xe8'+PA('<i',helper-(here+5))
  w+=bytes.fromhex('8b442408 8b4c2410 4c89e4 4189c4 48898d70fdffff')+vm
  here=at+len(w);w+=b'\xe9'+PA('<i',0x618083-(here+5))
  assert len(w)<=0x80,len(w);buf[o:o+len(w)]=w
  image[0x617ff7:0x617ffc]=b'\xe9'+PA('<i',at-(0x617ff7+5))
  wrappers.append(dict(site='0x617ff7',address=hex(at),kind=65,wrapper_hex=w.hex()))
 # ===== end agent A a009 code sites =====
 # ===== agent A a013 code sites: PS3 1.19 leaderboards + post-match handler (helper lb_kind, kinds 66/67; x-stub slots 0x11200/0x11280) =====
 if os.environ.get('U3MP_LB119','1')!='0':
  PA=struct.pack
  def a13_check(site,orig,name):
   if bytes(image[site:site+len(orig)])!=orig:raise ValueError('a013 '+name+' site mismatch %#x'%site)
  def a13_fn(at,kind):   # helper(base, ev, kind, state), ev[0..2] = rdi/rsi/rdx, ev[3] -> rax (as a009)
   w=bytearray.fromhex('554889e5 4883ec20 48893c24 4889742408 4889542410 48c744241800000000')
   here=at+len(w);w+=b'\x48\x8d\x3d'+PA('<i',-(here+7))
   w+=bytes.fromhex('4889e6')+b'\xba'+PA('<I',kind)
   here=at+len(w);w+=b'\x48\x8d\x0d'+PA('<i',state-(here+7))
   here=at+len(w);w+=b'\xe8'+PA('<i',helper-(here+5))
   w+=bytes.fromhex('488b442418 c9 c3')
   return w
  def a13_slot(off):
   at,buf,o=stub_slot(off)
   if any(buf[o:o+0x80]):raise ValueError('a013 stub slot in use '+hex(off))
   return at,buf,o
  # (1) widget update 0x63a310 = f_2551c0: its tail 0x63a426 (w.idx = edx; w.colset = table[idx]; ret) -> kind 66, which also
  #     runs the query f_328a28 PS4 cut. Entered only by fall-through (0x63a423) and the branch at 0x63a40a/0x63a40c paths.
  a13_check(0x63a426,bytes.fromhex('895720488d0452488d0dfcbad6008b448104894710c3'),'widget tail')
  at,buf,o=a13_slot(0x11200);w=a13_fn(at,66);assert len(w)<=0x80;buf[o:o+len(w)]=w
  image[0x63a426:0x63a42b]=b'\xe9'+PA('<i',at-(0x63a426+5))
  wrappers.append(dict(site='0x63a426',address=hex(at),kind=66,wrapper_hex=w.hex()))
  # (2) post-match handler 0x678b00 (= 1.10 f_294798) -> kind 67 = PS3 1.19 f_29a068 (entered at its first byte only)
  a13_check(0x678b00,bytes.fromhex('554889e54157415641554154'),'post-match')
  at,buf,o=a13_slot(0x11280);w=a13_fn(at,67);assert len(w)<=0x80;buf[o:o+len(w)]=w
  image[0x678b00:0x678b06]=b'\xe9'+PA('<i',at-(0x678b00+5))+b'\x90'
  wrappers.append(dict(site='0x678b00',address=hex(at),kind=67,wrapper_hex=w.hex()))
  # (3)/(4) a015: no 6-board cycle / title-table patch any more (tournament boards removed, user decision); PS4's own 4-board
  # cycle 0x74c110 / 0x74c140 and title table 0x13a5f30 match the remaining 1.19 boards' column sets and titles.
 # ===== end agent A a013 code sites =====
 # ===== agent A a018 code sites: PS3 1.19 friends (helper fr_kind, kind 68; x-stub slots 0x11300 / 0x11380 / 0x11400) =====
 if os.environ.get('U3MP_FRIENDS119','1')!='0':
  PA=struct.pack
  def a18_check(site,orig,name):
   if bytes(image[site:site+len(orig)])!=orig:raise ValueError('a018 '+name+' site mismatch %#x'%site)
  def a18_fn(at,sub):   # helper(base, ev, 68, state), ev[0..2] = rdi/rsi/rdx, ev[3] = sub id on entry (a013 thunk otherwise)
   w=bytearray.fromhex('554889e5 4883ec20 48893c24 4889742408 4889542410')+b'\x48\xc7\x44\x24\x18'+PA('<I',sub)
   here=at+len(w);w+=b'\x48\x8d\x3d'+PA('<i',-(here+7))
   w+=bytes.fromhex('4889e6')+b'\xba'+PA('<I',68)
   here=at+len(w);w+=b'\x48\x8d\x0d'+PA('<i',state-(here+7))
   here=at+len(w);w+=b'\xe8'+PA('<i',helper-(here+5))
   w+=bytes.fromhex('488b442418 c9 c3')
   return w
  def a18_slot(off):
   at,buf,o=stub_slot(off)
   if any(buf[o:o+0x80]):raise ValueError('a018 stub slot in use '+hex(off))
   return at,buf,o
  # (1) LIST_PSN_FRIENDS row click 0x671f60 (= 1.10 f_29331c; registered for 0x7e4e92d5 by 0x673850) -> sub 1 = 1.19 f_29331c
  # (2) LIST_PLAYERSMET row click 0x671580 (= f_287e00, registered for 0x369314b2) -> sub 2 (View Profile taken out)
  #     both are entered only at their first byte (callbacks)
  for site,off,sub,name in ((0x671f60,0x11300,1,'friends click'),(0x671580,0x11380,2,'players-met click')):
   a18_check(site,bytes.fromhex('554889e5'),name)
   at,buf,o=a18_slot(off);w=a18_fn(at,sub);assert len(w)<=0x80;buf[o:o+len(w)]=w
   image[site:site+5]=b'\xe9'+PA('<i',at-(site+5))
   wrappers.append(dict(site=hex(site),address=hex(at),kind=68,sub=sub,wrapper_hex=w.hex()))
  # (3) the report popup's page handler (PS3 f_280e38): a code address for menu+0x2168 entries; ea[96] = its address
  at,buf,o=a18_slot(0x11400);w=a18_fn(at,3);assert len(w)<=0x80;buf[o:o+len(w)]=w
  a18_cb=at   # written to ea[96] in the switch-word block (the ExtState image is appended later)
  wrappers.append(dict(site='ea[96]',address=hex(at),kind=68,sub=3,wrapper_hex=w.hex()))
 # ===== end agent A a018 code sites =====
 # ===== agent A a020 code sites: PS3 1.19 party invites / joins (helper fr_kind subs 4..9, kind 68; x-stub slots 0x13000..0x13280) =====
 if os.environ.get('U3MP_FRIENDS119','1')!='0':
  # callbacks registered by 0x673850 (entered only at their first byte): invite click 0x6ca335e2 0x6716d0 (= 1.10 f_288c58)
  # -> sub 4; accept 0xd79bd5ed RET 0x6716b0 -> sub 5 (f_297e44); delete 0x39764828 RET 0x6716c0 -> sub 6 (f_281bb0); Join
  # Party 0x0ace8463 RET 0x671960 -> sub 7 (f_288414); the "Creating Party..." handler 0x671810 (registered by 0x671840 =
  # f_288514) -> sub 9 (f_294a34)
  for site,orig,off,sub,name in ((0x6716d0,'554889e5',0x13000,4,'invite click'),(0x6716b0,'c3666666666666',0x13080,5,'accept'),
                                 (0x6716c0,'c3666666666666',0x13100,6,'delete'),(0x671960,'c3666666666666',0x13180,7,'join party'),
                                 (0x671810,'833f067528',0x13280,9,'create popup')):
   a18_check(site,bytes.fromhex(orig),'a020 '+name)
   at,buf,o=a18_slot(off);w=a18_fn(at,sub);assert len(w)<=0x80;buf[o:o+len(w)]=w
   image[site:site+5]=b'\xe9'+PA('<i',at-(site+5))
   wrappers.append(dict(site=hex(site),address=hex(at),kind=68,sub=sub,wrapper_hex=w.hex()))
  # the "Joining Party..." handler (f_28c71c): a code address for fr_wait_popup; ea[126]
  at,buf,o=a18_slot(0x13200);w=a18_fn(at,8);assert len(w)<=0x80;buf[o:o+len(w)]=w
  a20_jcb=at
  wrappers.append(dict(site='ea[126]',address=hex(at),kind=68,sub=8,wrapper_hex=w.hex()))
 # ===== end agent A a020 code sites =====
 # ===== agent A a022: pickup-shimmer exposure scale B (port-compat; x-stub slot 0x14000; ea[116]/ea[117]) =====
 # The pickup shimmer colour (writer 0x10b2160, slot 0 at 0x10b2c80..0x10b2cba / 0x10b35b3..0x10b35d9) is
 #   settings colour (retail weapon-art *pickup-shimmer* = 7000/7000/10000) x A / B x 0.2,
 # A = render settings +0xc14 (live 0x1b3c0f4), B = exposure scale 0x1a502c0+0x210. PS3 1.19 does exactly the same
 # (f_852xxx: lfs f25,3028(rs) = +0xbd4; lfs f29,528(tm) = +0x210; fdivs; fmuls 0.2 at 0x852cbc..0x852cd0).
 # PS3 fills +0x210 on the CPU in the tonemap update 0x793c28..0x7945ac: mode 2 -> 1.0; manual -> 2^(rs+0x6fc)
 # (0x794360..0x794380, powf(2.0, .)); auto -> 2^EV from the measured luminance, EV limited to the settings range; then
 # clamped to [1e-5, 100] (0x79407c..0x794098). PS4 0xee0141..0xee019c instead copies it from the GPU exposure buffer
 # *(0x1a4f8b0+0x748) (created for every mode at 0xed8d16; the GPU tonemap uses that buffer directly). On the PC port that
 # GPU result never reaches CPU memory: TEST (v5.91, Syria TDM) read B = 0.0 for the whole session, so A/B = +inf and every
 # pickup weapon draws flat white (D8 bloom-off showed the white weapons; bloom spreads them into the 'white blobs').
 # Port-compat workaround (U3MP_SHIMMER_B=0 leaves the site unpatched): when the copied value is not > 1e-5, store PS3's
 # CPU value instead, from the live render settings (r12): mode +0x6f0 == 0 -> 2^(+0x6fc); == 1 -> 2^((+0x6f4 + +0x6f8)/2)
 # (the middle of the auto-exposure EV range PS4 hands the GPU at 0xee0082..0xee00a3; Syria MP 1.5..2.0 -> 3.36); else 1.0.
 # A real (nonzero) GPU value is kept untouched. exp2f = PLT 0x139cd80 (imp_exp2f); rsp is 16-aligned at the site (7 pushes).
 # Diagnostics: ea[116] substitutions, ea[117] last substituted B (float bits).
 if __import__('os').environ.get('U3MP_SHIMMER_B','1')!='0':
  site=0xee019c
  if image[site:site+13]!=bytes.fromhex('c4c17a1186100200004883c408'):raise ValueError('a022 shimmer B site mismatch')
  at,buf,o=stub_slot(0x14000)
  ea116=rw2+0x3c50+116*4;ea117=rw2+0x3c50+117*4
  w=bytearray();fix=[]
  def rel(kind,target,size=4):fix.append((len(w)-size,kind,target,size))
  w+=bytes.fromhex('c5fa100d')+bytes(4);rel('k','eps')            # vmovss xmm1,[rip+eps]
  w+=bytes.fromhex('c5f82ec1')                                    # vucomiss xmm0,xmm1
  w+=bytes.fromhex('77')+bytes(1);rel('l','ok',1)                  # ja ok (not taken on NaN)
  w+=bytes.fromhex('418b8424f0060000')                            # mov eax,[r12+0x6f0]
  w+=bytes.fromhex('85c0')                                        # test eax,eax
  w+=bytes.fromhex('75')+bytes(1);rel('l','m1',1)                  # jne m1
  w+=bytes.fromhex('c4c17a108424fc060000')                        # vmovss xmm0,[r12+0x6fc]
  w+=bytes.fromhex('eb')+bytes(1);rel('l','doexp',1)               # jmp doexp
  L={};L['m1']=len(w)
  w+=bytes.fromhex('83f801')                                      # cmp eax,1
  w+=bytes.fromhex('75')+bytes(1);rel('l','m2',1)                  # jne m2
  w+=bytes.fromhex('c4c17a108424f4060000')                        # vmovss xmm0,[r12+0x6f4]
  w+=bytes.fromhex('c4c17a588424f8060000')                        # vaddss xmm0,xmm0,[r12+0x6f8]
  w+=bytes.fromhex('c5fa5905')+bytes(4);rel('k','half')           # vmulss xmm0,xmm0,[rip+half]
  w+=bytes.fromhex('eb')+bytes(1);rel('l','doexp',1)               # jmp doexp
  L['m2']=len(w)
  w+=bytes.fromhex('c5fa1005')+bytes(4);rel('k','one')            # vmovss xmm0,[rip+one]
  w+=bytes.fromhex('eb')+bytes(1);rel('l','cnt',1)                 # jmp cnt
  L['doexp']=len(w)
  w+=bytes.fromhex('e8')+bytes(4);rel('a',0x139cd80)              # call exp2f
  L['cnt']=len(w)
  w+=bytes.fromhex('ff05')+bytes(4);rel('a',ea116)                # inc dword [rip+ea116]
  w+=bytes.fromhex('c5fa1105')+bytes(4);rel('a',ea117)            # vmovss [rip+ea117],xmm0
  L['ok']=len(w)
  w+=bytes.fromhex('c4c17a118610020000')                          # vmovss [r14+0x210],xmm0 (displaced)
  # fixer H h024: the a022 exit goes through x-stub 0x12b80: with U3MP_EXPO_TABLE=1 (byte rw2+0x4456 bit2) and the exposure
  # override set (tm+0x218, r14 = tm), B := exp2f(tm+0x214), the EV the GPU was told to use (h024 / j012); else unchanged.
  # exp2f at the same stack depth as a022's own call (rsp 16-aligned); the code after 0xee01a5 is the epilogue only.
  H24B,_hb,_ho=stub_slot(0x12b80)
  if any(_hb[_ho:_ho+0x80]):raise ValueError('stub slot in use 0x12b80')
  _x=bytearray(bytes.fromhex('f605')+bytes(4)+b'\x04');struct.pack_into('<i',_x,2,(rw2+0x4456)-(H24B+7))   # test byte [ec1.b2],4
  _x+=bytes.fromhex('7421')                                       # jz back
  _x+=bytes.fromhex('4180be1802000000')                           # cmp byte [r14+0x218],0
  _x+=bytes.fromhex('7417')                                       # je back
  _x+=bytes.fromhex('c4c17a108614020000')                         # vmovss xmm0,[r14+0x214]
  _x+=b'\xe8'+struct.pack('<i',0x139cd80-(H24B+len(_x)+5))         # call exp2f
  _x+=bytes.fromhex('c4c17a118610020000')                         # vmovss [r14+0x210],xmm0
  assert len(_x)==0x2a,hex(len(_x))
  _x+=b'\xe9'+struct.pack('<i',0xee01a5-(H24B+len(_x)+5))          # back: jmp 0xee01a5
  _hb[_ho:_ho+len(_x)]=_x
  wrappers.append(dict(site='-',address=hex(H24B),kind='h024-shimmer-B-follow',wrapper_hex=bytes(_x).hex()))
  w+=bytes.fromhex('e9')+bytes(4);rel('a',H24B)                   # jmp x-stub 0x12b80 (-> 0xee01a5)
  while len(w)%4:w.append(0xcc)
  L['eps']=len(w);w+=struct.pack('<f',1e-5)
  L['half']=len(w);w+=struct.pack('<f',0.5)
  L['one']=len(w);w+=struct.pack('<f',1.0)
  for pos,kind,target,size in fix:
   if kind=='l':
    d=L[target]-(pos+1);assert -128<=d<=127;w[pos]=d&0xff
   else:
    dst=at+L[target] if kind=='k' else target
    struct.pack_into('<i',w,pos,dst-(at+pos+4))
  assert len(w)<=0x80,len(w)
  buf[o:o+len(w)]=w
  image[site:site+9]=b'\xe9'+struct.pack('<i',at-(site+5))+bytes.fromhex('0f1f4000')
  wrappers.append(dict(site=hex(site),address=hex(at),kind='a022-shimmer-B',wrapper_hex=w.hex()))
 # ===== end agent A a022 =====
 image.extend(bytes(rx-len(image)));image.extend(code);image.extend(bytes(0x4000))
 assert len(image)==rx2
 if len(ext_code)>EXT_CODE_SIZE-XSTUB:raise ValueError('ext code overlaps the x-stub area')
 # v7.96 rf30 crash guards (A: Player::Init this+0x138 NULL in the lobby; B: render job slot re-read)
 o=bytes.fromhex('498b8738010000');agc_check(0xbd6a28,o,'player init art set')
 agc_check(0xbd6ac3,bytes.fromhex('e8b8493f00'),'player init art call')
 rf30=next((s_ for s_ in range(0x17000,0x18000,0x80) if not any(xcode[s_-0x10000:s_-0x10000+0x80])),None)
 if rf30 is None:raise ValueError('rf30: no free x-stub slot in 0x17000..0x17f80')
 at,buf,o2=stub_slot(rf30)
 w=bytearray(o)+bytes.fromhex('4885c0')+b'\x74\x05'
 here=at+len(w);w+=b'\xe9'+P('<i',0xbd6a2f-(here+5))
 here=at+len(w);w+=b'\xe9'+P('<i',0xbd6ac8-(here+5))
 buf[o2:o2+len(w)]=w;agc_site(0xbd6a28,o,at,w,'rf30-art')
 if image[0xe99042:0xe9904a]!=bytes.fromhex('488b74d7400b4604'):raise ValueError('render slot re-read site mismatch')
 image[0xe99042:0xe99047]=b'\x90'*5
 image.extend(ext_code);image.extend(bytes(EXT_CODE_SIZE-XSTUB-len(ext_code)));image.extend(xcode)
 assert len(image)==rw2
 # v5.47: with U3MP_CINEMA=1 the data segment grows by 0x27000 for the film list (20 x 0x1e90 at rw2+0x10000; PS3
 # allocates it in f_2b1870 with f_7124e0, PS4 has no allocation for stream +0x1e50).
 # agent B b001: two lists as PS3 1.19 f_2b8a3c allocates (list A local films at rw2+0x10000, list B online files at +0x36340).
 # v5.68: the helper's CIN_LIST_OFF must match EXT_DATA_SIZE (film lists right after the ext data)
 import re as _re,os as _os
 _c=open(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)),'u3_lan_profiles.c')).read() if _os.path.exists(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)),'u3_lan_profiles.c')) else ''
 _m=_re.search(r'#define CIN_LIST_OFF (0x[0-9a-fA-F]+)u',_c)
 if _m and int(_m.group(1),16)!=EXT_DATA_SIZE:raise ValueError('CIN_LIST_OFF != EXT_DATA_SIZE')
 EXT_SEG=EXT_DATA_SIZE+(0x4d000+0x40000 if CINEMA else 0)   # t002: + the two 0x20000 film bit-stream buffers (cin_film_buf)
 # agent M m004: the moved NetMenuHost table (11 x 0x308 + 8) at rw2+0xed000, right after the film buffers
 if os.environ.get('U3MP_GAMETYPES119','1')!='0':EXT_SEG=max(EXT_SEG,0xed000+0x3000)
 # Local menu fix: 96 slots per character-parts index map, beyond existing data.
 parts_forward=rw2+0xf0000;parts_reverse=rw2+0xf0200
 if parts_forward!=0x2208000:raise ValueError('character-parts helper layout mismatch')
 EXT_SEG=max(EXT_SEG,0xf4000)
 for site,opcode,old,target in (
  (0x757725,bytes.fromhex('4c8d35'),0x1b6a740,parts_reverse),
  (0x75772c,bytes.fromhex('4c8d3d'),0x1b6a6c0,parts_forward),
  (0x7577ad,bytes.fromhex('488d05'),0x1b6a6c0,parts_forward),
  (0x757ba1,bytes.fromhex('488d0d'),0x1b6a740,parts_reverse)):
  if image[site:site+3]!=opcode or site+7+struct.unpack_from('<i',image,site+3)[0]!=old:
   raise ValueError('character-parts index signature mismatch '+hex(site))
  struct.pack_into('<i',image,site+3,target-(site+7))
 image.extend(ext_data_init);image.extend(bytes(EXT_SEG-len(ext_data_init)))
 # v4.92 test switch: U3MP_DEBUG_DEATH=1 presets RS2 mode bit 2 (state+0x3d78): the restored PS3 out-of-bounds
 # kill (f_363ed4 +0x744) is forced 15 s after each spawn, so the local Player dies ~20 s after spawning.
 # The same bit can be set at runtime (byte at image 0x2067e78 when state = 0x2064100).
 if os.environ.get('U3MP_DEBUG_DEATH')=='1':image[state+0x3d78]|=4
 # v5.07: U3MP_KILLVIS=1 presets RS2 mode bit 3: start_game keeps the map entry's killvis sid (+0x40, the out-of-bounds gate)
 # for san-upper-multi-killvis instead of clearing it. Only with a converted killvis pak (vischeat.py) in the overlay: the
 # retail 244-byte stub stalls the level queue.
 # s001: default ON now (PS3 loads the killvis level from map entry +0x38/+0x40 and arms the out-of-bounds gate with its sid);
 # the converted san-upper-multi-killvis.pak (24720 bytes, md5 deabd0d4, vischeat.py) ships in overlay-port. U3MP_KILLVIS=0
 # restores the old City of Brass skip (needed only with the retail 244-byte stub).
 if os.environ.get('U3MP_KILLVIS','1')!='0':image[state+0x3d78]|=8
 # v5.08 test switch: U3MP_OOB_TEST=1 presets RS2 mode bit 5: 20 s after each spawn the out-of-bounds test point is moved into
 # the first out-of-bounds voxel of the logged-in VIS_CHEAT grid (ExtState.oob[13..15]), so the PS3 path 8 corners -> 5 s
 # timer -> death runs without walking into a pit. Needs a map with a converted killvis (and U3MP_KILLVIS=1 on City of Brass).
 if os.environ.get('U3MP_OOB_TEST')=='1':image[state+0x3d78]|=0x20
 # v5.13: U3MP_NO_COUNTDOWN=1 presets RS2 mode bit 6: Start Match skips the PS3 "Starting in [A]..." popup (old direct start)
 if os.environ.get('U3MP_NO_COUNTDOWN')=='1':image[state+0x3d78]|=0x40
 # v5.14: U3MP_SOLO_START (default 1) presets RS2 bit 7: a lone LAN host may start below the PS3 minimum player count and
 # skips the type-10 team check. U3MP_SOLO_START=0 applies the PS3 rules to a lone host too.
 if os.environ.get('U3MP_SOLO_START','1')!='0':image[state+0x3d78]|=0x80
 # v5.24: a lone host may not start co-op Hunter (net game type 18, two teams: heroes vs the AI-hunter team). PS3 needs 2
 # players for it; started solo the match ends at once and the post-match script teardown faults. U3MP_SOLO_HUNTER=1 (RS2 bit
 # 0x20000, state+0x3d7a bit1) keeps the old bypass for testing.
 if os.environ.get('U3MP_SOLO_HUNTER')=='1':image[state+0x3d7a]|=2
 # v5.32: U3MP_SCRIPT_GUARD default ON (=0 disables): a teardown of a script instance with a garbage wait count is skipped
 # (read-only check, see script_teardown_watch). Needs U3MP_SCRIPT_WATCH (default on).
 if os.environ.get('U3MP_SCRIPT_GUARD','1')!='0':image[state+0x3d7a]|=4
 # v5.25 debug: U3MP_FORCE_END=<seconds> (RS2[15], state+0x3db4) ends the match through the normal game-over path that many
 # seconds after Playing starts (host only, once per match). Default off.
 fe=int(os.environ.get('U3MP_FORCE_END','0') or 0)
 if fe>0:struct.pack_into('<I',image,state+0x3db4,fe)
 # v5.14: U3MP_TITLE_MENU=1 presets RS2 bit 8: the main/campaign menus keep Bluepoint's R1 Title Menu as a third prompt
 if os.environ.get('U3MP_TITLE_MENU','1')!='0':image[state+0x3d79]|=1  # default ON (user)
 # v5.16 kill-reward port parts (RS2 mode bits 0x400/0x800/0x1000/0x2000): history, K/D counters, stat events, medal checks
 for nm,b in (('U3MP_KR_NOHIST',4),('U3MP_KR_NOKD',8),('U3MP_KR_NOSTAT',0x10),('U3MP_KR_NOMEDAL',0x20),('U3MP_KR_NOPTS',0x40),('U3MP_KR_NOPOPUP',0x80)):
  if os.environ.get(nm)=='1':image[state+0x3d79]|=b
 if os.environ.get('U3MP_KR_NOBOOST')=='1':image[state+0x3d7a]|=1
 # v5.30 solo-start port of the user's RPCS3 patches (both default on): U3MP_SOLO_COOP (RS2 bit 0x80000, state+0x3d7a bit3:
 # helper side of the solo co-op patch: no min-player start check for any party/game type, debug byte +0x7d, custom-game
 # list preset), U3MP_UNLOCK_STORE (RS2 bit 0x100000, bit4: store "all items unlocked" flag 0x17b2a94 = 1),
 # U3MP_SOLO_COOP_NOMAP=1 (RS2 bit 0x200000, bit5: no list preset; the builder also skips the map-index stubs).
 if os.environ.get('U3MP_SOLO_COOP','1')!='0':image[state+0x3d7a]|=8
 if os.environ.get('U3MP_UNLOCK_STORE','1')!='0':image[state+0x3d7a]|=0x10
 if os.environ.get('U3MP_SOLO_COOP_NOMAP')=='1':image[state+0x3d7a]|=0x20
 # v5.31 test switch: U3MP_NO_STAGE_CLEANUP=1 (RS2 0x400000, bit6) skips the helper's in-match unwant of other maps' lobby
 # stages (match_stage_cleanup), to bisect the script-process use-after-free at co-op match start.
 if os.environ.get('U3MP_NO_STAGE_CLEANUP')=='1':image[state+0x3d7a]|=0x40
 # v5.33 diagnostic: U3MP_SS_WATCH=1 (RS2 0x800000, bit7) watches the ss-survival-display track group (module ss-survival
 # file offset 0x660) for runtime writes and module moves (ExtState ssb/ssw/ssn).
 if os.environ.get('U3MP_SS_WATCH')=='1':image[state+0x3d7a]|=0x80
 # v5.41: U3MP_RANK_TEX=0 (RS2 0x1000000, state+0x3d7b bit0) keeps rank icons as type 0x29 instead of the ranks/N texture (v5.42: and leaves the lobby profile box native).
 if os.environ.get('U3MP_RANK_TEX','1')=='0':image[state+0x3d7b]|=1
 # v5.42: U3MP_PARTY_COLOR=0 (RS2 0x2000000, state+0x3d7b bit1) leaves party op 20 (row colour) unanswered.
 if os.environ.get('U3MP_PARTY_COLOR','1')=='0':image[state+0x3d7b]|=2
 # v5.43: U3MP_R2_TEAM=0 (RS2 0x4000000, state+0x3d7b bit2) keeps the lobby's R2 Change Team widget (enum 0x30) native.
 if os.environ.get('U3MP_R2_TEAM','1')=='0':image[state+0x3d7b]|=4
 # v5.43: U3MP_KR_COOPAI=0 (RS2 0x8000000, state+0x3d7b bit3) leaves out the co-op AI victim part of the kill-reward port.
 if os.environ.get('U3MP_KR_COOPAI','1')=='0':image[state+0x3d7b]|=8
 # v5.47: FUNCTIONS switch word at ExtState +0x3a70 (rw2+0x3a70); RS2 bits >= 0x10000000 belong to SHORTCUTS.
 # bit0 U3MP_ADV=0 (1.19 Advanced Settings datasources off; was RS2 0x10000000 in v5.46), bit1 cinema port on (agent B b001: default; U3MP_CINEMA=0 clears it), bit2 U3MP_LOBBY_Q11=1 (v5.50: old lobby op 11 answer).
 if os.environ.get('U3MP_ADV','1')=='0':image[rw2+0x3a70]|=1
 if CINEMA:image[rw2+0x3a70]|=2
 if os.environ.get('U3MP_LOBBY_Q11','0')=='1':image[rw2+0x3a70]|=4   # v5.50: old lobby op 11 answer (all row queries true)
 if os.environ.get('U3MP_PLAYER_NET_ANIMS','1')=='0':image[rw2+0x3a70]|=8   # f004: Player net anim tables left empty (PS4 retail)
 # ===== agent A: switch words ExtState.ea[0..3] (rw2+0x3c50 + 4*i) =====
 # ea[0] bit0 U3MP_PROFILE_NEW=0 (a001: no Profile "!" marker, PS3 1.19 f_33e61c 0x33eabc)
 if os.environ.get('U3MP_PROFILE_NEW','1')=='0':image[rw2+0x3c50]|=1
 # ea[0] bit1 U3MP_LAN_DELETE=0 (a002: no Delete Profile on PAGE_LAN_CHOOSE_PROFILE, PS3 1.19 0x291a38/0x291a8c, G mirror 0x1b6a400)
 if os.environ.get('U3MP_LAN_DELETE','1')=='0':image[rw2+0x3c50]|=2
 # ea[0] bit3 U3MP_PAGE_SOURCES=0 (a004: PAGE_BOOSTERS / PAGE_NET_TREASURES page-level sources stay native stubs)
 if os.environ.get('U3MP_PAGE_SOURCES','1')=='0':image[rw2+0x3c50]|=8
 # ea[0] bit4 U3MP_KICKBACK_ICONS=0 (a005: no LIST_MEDAL_STREAKS row icons / check / "!", PS3 1.19 f_345d20 op 4)
 if os.environ.get('U3MP_KICKBACK_ICONS','1')=='0':image[rw2+0x3c50]|=16
 # ea[0] bit5 U3MP_BOOSTER_ICONS=0 (a006: no LIST_BOOSTERS op 4 icons, no op 12 on boosters / kickbacks / co-op weapons)
 if os.environ.get('U3MP_BOOSTER_ICONS','1')=='0':image[rw2+0x3c50]|=32
 # ea[0] bit6 U3MP_PROFILE_ROWS=0 (a007: LIST_PROFILE stays native, no Hat Stats / DLC rows)
 if os.environ.get('U3MP_PROFILE_ROWS','1')=='0':image[rw2+0x3c50]|=64
 # ea[0] bit7 U3MP_FORCE_DLC=0 (a008: keep the stored DLC word; default clears bit 0 on LAN profile load/save)
 if os.environ.get('U3MP_FORCE_DLC','1')=='0':image[rw2+0x3c50]|=128
 # ea[1] bit0 U3MP_LEGACY119=0 (a009: no Legacy 4/5; the code sites are then left unpatched as well)
 if os.environ.get('U3MP_LEGACY119','1')=='0':image[rw2+0x3c54]|=1
 # ea[1] bit1 U3MP_A010=0 (a010: MOTD OK without the 1.19 sound / history rebuild, no 1.19 quit count)
 if os.environ.get('U3MP_A010','1')=='0':image[rw2+0x3c54]|=2
 # ea[1] bit2 U3MP_HIDE_DEAD=0 (a011: UTV / YouTube / Tutorial / video-share buttons stay visible)
 if os.environ.get('U3MP_HIDE_DEAD','1')=='0':image[rw2+0x3c54]|=4
 # ea[1] bit3 U3MP_PAGEMP=0 (a012: PAGE_MULTIPLAYER handler f_298e5c off; a010 quit count still runs)
 if os.environ.get('U3MP_PAGEMP','1')=='0':image[rw2+0x3c54]|=8
 # ea[1] bit4 U3MP_LB119=0 (a013: no leaderboard manager / query / submit, PS4 0x678b00 post-match as before)
 if os.environ.get('U3MP_LB119','1')=='0':image[rw2+0x3c54]|=0x10
 else:   # a013/a015: PS3 1.19 board table 0xdb5d80 without the tournament boards (4 x {board, column set, title}) in ea[200..211]
  for i,v in enumerate((0x514,0x6dd2062d,0x70c81d2e, 0x515,0x6bb9233e,0xc0cd1b00, 0x516,0xea26aeb8,0xf67d9ad2,
                        0x517,0xba4ec309,0xc072e0a6)):
   struct.pack_into('<I',image,rw2+0x3c50+(200+i)*4,v)
 # ea[1] bit5 U3MP_FRIENDS119=0 (a018: no NetFriends port, PS4's own friends handlers)
 if os.environ.get('U3MP_FRIENDS119','1')=='0':image[rw2+0x3c54]|=0x20
 else:struct.pack_into('<I',image,rw2+0x3c50+96*4,a18_cb)   # a018: report popup handler thunk (x-stub 0x11400)
 if os.environ.get('U3MP_FRIENDS119','1')!='0':struct.pack_into('<I',image,rw2+0x3c50+126*4,a20_jcb)   # a020: "Joining Party..." handler thunk (x-stub 0x13200)
 # ea[1] bit6 U3MP_WEAPON_OP11=0 (a021: PAGE_COMP_WEAPON list stays native, no op 11 weapon id for the stats bars)
 if os.environ.get('U3MP_WEAPON_OP11','1')=='0':image[rw2+0x3c54]|=0x40
 # ===== end agent A =====
 # ===== agent C: switch words ExtState.ec[0..3] (image[rw2+0x4450+4*i]); ec[4..255] are agent C diagnostics =====
 # ec[0] bit0 U3MP_KR119=0: kill points of the 1.19 mode-type-10 branch stay as in 1.10 (c001, kr_port.inc).
 if os.environ.get('U3MP_KR119','1')=='0':image[rw2+0x4450]|=1
 # ec[0] bit1 U3MP_ELIM119=0: Elimination (type 10) late-join fades of PS3 1.19 f_363ed4 left out (1.10 behaviour) (c002).
 if os.environ.get('U3MP_ELIM119','1')=='0':image[rw2+0x4450]|=2
 # ec[0] bit2 U3MP_ELIM_WAIT=0: no 1.19 Elimination 'Waiting for new players' hold/text (c003).
 if os.environ.get('U3MP_ELIM_WAIT','1')=='0':image[rw2+0x4450]|=4
 # ec[0] bit3 U3MP_TAUNT119=0: no PS3 1.19 taunt package wants at match setup (c005, kinds 103/104).
 if os.environ.get('U3MP_TAUNT119','1')=='0':image[rw2+0x4450]|=8
 # ec[0] bit4 U3MP_TAUNT_TICK=1: keep the v4.95 taunt_tick equipped-taunt packages as well (c006 fallback, default off).
 if os.environ.get('U3MP_TAUNT_TICK','0')=='1':image[rw2+0x4450]|=16
 # ec[0] bit5 U3MP_PINFO119=0: player info message 0x3a stays 1.10 (no 1.19 fields; the size patch is left out too) (c006).
 if os.environ.get('U3MP_PINFO119','1')=='0':image[rw2+0x4450]|=32
 # ec[0] bit6 U3MP_HAT119=0: no 1.19 hat-stat modifiers / booster skip in the modifier rebuild (c006).
 if os.environ.get('U3MP_HAT119','1')=='0':image[rw2+0x4450]|=64
 # ec[0] bit7 U3MP_SPOT119=0: single spotted flag as PS4 (kind 114 runs PS4's own loop; readers see np+0x9b1) (c007).
 if os.environ.get('U3MP_SPOT119','1')=='0':image[rw2+0x4450]|=128
 # ec[0] bit8 U3MP_REPL119=0: no PS3 1.19 player state replication sender (PS4 sends none) (c008, respawn_tick -> repl_loop119).
 if os.environ.get('U3MP_REPL119','1')=='0':image[rw2+0x4451]|=1
 # c009: ec[0] bit9 U3MP_DEC119=0: no PS3 1.19 net anim decoder (apply leaves the state cleared, CreateAnimCmds 0x6847d0 /
 # GetLocator 0x684cf0 run PS4's natives); bit10 U3MP_NETANIM_TABLES119=0: the generator's 8 net anim tables stay empty (PS4);
 # bit11 U3MP_NPC_ANIM119=0: NPC state messages carry no anim bits (PS4); bit12 U3MP_REPL119_LOOP=1: loopback check (opt-in).
 if os.environ.get('U3MP_DEC119','1')=='0':image[rw2+0x4451]|=2
 if os.environ.get('U3MP_NETANIM_TABLES119','1')=='0':image[rw2+0x4451]|=4
 if os.environ.get('U3MP_NPC_ANIM119','1')=='0':image[rw2+0x4451]|=8
 if os.environ.get('U3MP_REPL119_LOOP','0')=='1':image[rw2+0x4451]|=16
 # c010: ec[0] bit13 U3MP_HATICON119=0: no 1.19 hat-stat icons on the killed-by card.
 if os.environ.get('U3MP_HATICON119','1')=='0':image[rw2+0x4451]|=32
 # c011: ec[0] bit14 U3MP_EMBLEM119=0: no emblem compositor / binding / editor hooks (NetInit back to s003's 0x66b510 call).
 if os.environ.get('U3MP_EMBLEM119','1')=='0':image[rw2+0x4451]|=0x40
 # c011: ec[0] bit15 U3MP_EMBLEM119_FLIPV=1: emblem textures stored bottom row first (orientation test aid, default off).
 if os.environ.get('U3MP_EMBLEM119_FLIPV','0')=='1':image[rw2+0x4451]|=0x80
 # c012: ec[0] bit16 U3MP_DLC119=0: unlock condition 6 back to PS4's own byte read (packs >= 0x100 read past the array).
 if os.environ.get('U3MP_DLC119','1')=='0':image[rw2+0x4452]|=0x01
 # ec[40] / ec[41]: image offsets of the msg 0x3a read / write trampolines (c006, kinds 106 / 107).
 struct.pack_into('<II',image,rw2+0x4450+4*40,agc_tr[0],agc_tr[1])
 # ===== end agent C =====
 # ===== agent B: switch words ExtState.eb[0..3] (image[rw2+0x4050+4*i]); eb[4..255] agent B imports/diagnostics =====
 # eb[0] bit0 U3MP_CINEMA_HIDE=0: PAGE_CINEMA keeps BUTTON_CINEMA_MPEG / PROTECT / UNPROTECT native (default: hidden, b002);
 # the same switch keeps the Screenshot hot key/prompt (builder site 0xc3dd79, b004).
 if os.environ.get('U3MP_CINEMA_HIDE','1')=='0':image[rw2+0x4050]|=1
 # b010: eb[0] bit1 U3MP_LAN_SOLO=0: the LAN backend runs the PS3 1.19 path only (no local Create when its room server cannot
 # start or the room state never arrives, NetInit stops when UDP 9012 cannot open, slots 11 / 14 are PS3's no-ops).
 # bit2 U3MP_LAN_REFRESH=0: no 5 s Search repeat on PAGE_LAN_PARTIES (only the Find a Party press searches, as PS3 0x290af4).
 if os.environ.get('U3MP_LAN_SOLO','1')=='0':image[rw2+0x4050]|=2
 if os.environ.get('U3MP_LAN_REFRESH','1')=='0':image[rw2+0x4050]|=4
 # b012: eb[0] bit3 U3MP_NET_TIME=0: no NetUpdate net-time block and no time sync (the net clock runs free, PS4 retail).
 if os.environ.get('U3MP_NET_TIME','1')=='0':image[rw2+0x4050]|=8
 # b014: eb[0] bit4 U3MP_ONLINE_MM=1: Play Online runs PS3's online NetInit with the ND matchmaking backend (default off).
 if os.environ.get('U3MP_ONLINE_MM','0')=='1':image[rw2+0x4050]|=0x10
 # agent B b018: eb[0] bit5 U3MP_STAGE_LOGOUT=0: stage_tick re-wants the lobby stage after the match-start logout (old behaviour).
 if os.environ.get('U3MP_STAGE_LOGOUT','1')=='0':image[rw2+0x4050]|=0x20
 # agent B b018: eb[0] bit6 U3MP_STAGE_LOGOUT_FALLBACK=0: no post-match re-want fallback (only native f_30e43c calls bring the stage back).
 if os.environ.get('U3MP_STAGE_LOGOUT_FALLBACK','1')=='0':image[rw2+0x4050]|=0x40
 # agent B b020: eb[0] bit7 U3MP_RESULTS_SLOT0=1: round screens limited to slot 0 (the Select scoreboard), as b019 shipped.
 if os.environ.get('U3MP_RESULTS_SLOT0','0')=='1':image[rw2+0x4050]|=0x80
 # agent B b023: eb[1] bit0 U3MP_CIN_HUD_SKIP=1: the film-playback HUD stub (kind 160) skips the element (no trap, no tab bar).
 if os.environ.get('U3MP_CIN_HUD_SKIP','0')=='1':image[rw2+0x4054]|=1
 # agent B b026: eb[1] bit1 U3MP_CIN_HUD_DRAW=0: the kind-160 stub runs the update only and skips the draw (f_667f0c) again.
 if os.environ.get('U3MP_CIN_HUD_DRAW','1')=='0':image[rw2+0x4054]|=2
 # agent B b033: eb[1] bit2 U3MP_SP_TAB_TRIM=0: the campaign page (Single Player) shows the Options / Credits / Title Menu prompts again.
 if os.environ.get('U3MP_SP_TAB_TRIM','1')=='0':image[rw2+0x4054]|=4
 # agent B b034: eb[1] bit3 U3MP_LOBBY_SCENE=0: no per-frame NetUpdate lobby scene block (PS3 f_292b98 0x292ce8..0x292e54), the
 # camera runs from stage_tick again and NetShutdown does not leave the scene (the old one-shot scene enter only).
 if os.environ.get('U3MP_LOBBY_SCENE','1')=='0':image[rw2+0x4054]|=8
 # agent B b038: eb[1] bit4 U3MP_PROTO_KEEP=0: anim-proto-multiplayer wanted once with flags 1 again (dropped by the match setup's
 # want-list clear, so the lobby character only returns at the next Start Game).
 if os.environ.get('U3MP_PROTO_KEEP','1')=='0':image[rw2+0x4054]|=0x10
 # agent B b039 (test only): eb[1] bit5 U3MP_CIN_PANEL_QUAD=1: the film HUD panel is drawn with the transport bar's untextured quad
 # instead of PS4's menu box path (0x63d6f0), to tell whether that path draws at all in film playback.
 if os.environ.get('U3MP_CIN_PANEL_QUAD','0')=='1':image[rw2+0x4054]|=0x20
 # agent B b040: eb[1] bit6 U3MP_UI_DEFTEX=0: the kind-165 helper leaves the batch alone (the stub is also left out of the image).
 if os.environ.get('U3MP_UI_DEFTEX','0')!='1':image[rw2+0x4054]|=0x40
 # agent B b041: eb[1] bit7 U3MP_FILM_EVENTS=0: the kind-166 helper sends no film events (the stub is also left out of the image).
 if os.environ.get('U3MP_FILM_EVENTS','1')=='0':image[rw2+0x4054]|=0x80
 # agent B b041: eb[1] bit8 U3MP_K34_SCREEN_HANDLE=1: k34's TaskManagerOnline test reads the per-screen array 0x1bb9500 again (b034..b040).
 if os.environ.get('U3MP_K34_SCREEN_HANDLE','0')=='1':image[rw2+0x4055]|=1
 # agent B b043: eb[1] bit9 U3MP_CONNECT_STEPS=0: no NetInit progress steps in net+0x3e0 (the Connecting bar stays empty);
 #   eb[1] bit10 U3MP_CONNECT_GATE=0: startup phase 2 leaves the Connecting page without waiting for the "net" level.
 if os.environ.get('U3MP_CONNECT_STEPS','1')=='0':image[rw2+0x4055]|=2
 if os.environ.get('U3MP_CONNECT_GATE','1')=='0':image[rw2+0x4055]|=4
 # agent B b044: eb[1] bit11 U3MP_ONLINE_OFFLINE=0: no offline mode for Play Online (PS3's failure dialog / the old shell jump).
 if os.environ.get('U3MP_ONLINE_OFFLINE','1')=='0':image[rw2+0x4055]|=8
 # agent B b046: eb[1] bit12 U3MP_BOOSTER_RING=0: no level tag on booster icons (and no stub).
 if os.environ.get('U3MP_BOOSTER_RING','1')=='0':image[rw2+0x4055]|=0x10
 # agent B b047: eb[1] bit13 U3MP_HUD_SCORE_TEX=0: the HUD score sprites keep PS4's (missing) texture ids.
 if os.environ.get('U3MP_HUD_SCORE_TEX','1')=='0':image[rw2+0x4055]|=0x20
 # agent B b049: eb[1] bit14 U3MP_LIST_SOUNDS=0: no select sounds in the helper's booster / kickback / co-op mod lists.
 if os.environ.get('U3MP_LIST_SOUNDS','1')=='0':image[rw2+0x4055]|=0x40
 # agent B b050 (user item 9: XP shown blue, PS3 yellow): text markup colour order. PS3 1.19 f_9a3500 '@C' (0x9a3f2c):
 #   sscanf("%x") = 0xAARRGGBB, rotlwi 8 -> 0xRRGGBBAA, channels times the current colour, stored as bytes R,G,B,A. PS4's
 #   copy 0xf1fff7 does 'movbe [rbp-0x114],eax; bswap eax' -> 0xBBGGRRAA, so the stored colour is B,G,R,A: red and blue
 #   swap (the profile box's |@Cffcece7c| XP (PS3 yellow) shows cyan, |@Cffff0000| shows blue, team-coloured clan tags
 #   |@C%08x| swap too). The 10 bytes become 'rol eax,8; mov [rbp-0x114],eax; nop' (the slot is not read again), so
 #   eax = 0xRRGGBBAA as on PS3 and the unchanged code below stores R,G,B,A (the alpha-0 branch 0xf20600 too).
 #   '@R' (PS3 0x9a4034, 24-bit colour scaled by the current colour's brightest channel) has the same swap at 0xf20619:
 #   'mov ecx,0x810' (bextr R) -> 'mov ecx,0x800' (bextr B) and 'movzx esi,sil' (B) -> 'shr esi,16; nop' (R), so R lands
 #   in the first byte as on PS3. (PS4 takes the brightest of bytes 0, 1 and 3 where PS3 takes 0, 1, 2; left as is.)
 #   U3MP_TEXT_RGB=0 leaves both as PS4 has them.
 if os.environ.get('U3MP_TEXT_RGB','1')!='0':
  if image[0xf20031:0xf2003b]!=bytes.fromhex('0f38f185ecfeffff0fc8'):raise ValueError('agent B b050 @C site mismatch')
  image[0xf20031:0xf2003b]=bytes.fromhex('c1c0088985ecfeffff90')
  if image[0xf20635:0xf2063a]!=bytes.fromhex('b910080000') or image[0xf20665:0xf20669]!=bytes.fromhex('400fb6f6'):raise ValueError('agent B b050 @R site mismatch')
  image[0xf20635:0xf2063a]=bytes.fromhex('b900080000')
  image[0xf20665:0xf20669]=bytes.fromhex('c1ee1090')
 # ===== end agent B =====
 # ===== agent E switches (eb[1] from bit31 down: byte rw2+0x4057 first, so agent B's bits from bit0 up never meet them) =====
 # agent E e001: eb[1] bit31 U3MP_HUD_WEAPON_POS=0: the in-match weapon panel keeps PS4's origin 89,54 (PS3 1.19: 96,72).
 if os.environ.get('U3MP_HUD_WEAPON_POS','1')=='0':image[rw2+0x4057]|=0x80
 # agent E e002: eb[1] bit30 U3MP_LOADOUT_BOOSTER_ICON=0: loadout-control booster cells stay type 0x2e (no icon); no trampolines.
 if os.environ.get('U3MP_LOADOUT_BOOSTER_ICON','1')=='0':image[rw2+0x4057]|=0x40
 # agent E e003: eb[1] bit29 U3MP_LOBBY_SIDE_LIVE=0: the lobby character keeps the look picked at placement (PS3 LAN path).
 # agent F f005 (user correction): default OFF. PS3 changes the look only at the new-stage placement after Start Game
 #   (f_24c05c net+1016 branch, net+1016 = 1 from state 45); U3MP_LOBBY_SIDE_LIVE=1 brings back e003's in-lobby switch.
 if os.environ.get('U3MP_LOBBY_SIDE_LIVE','0')=='0':image[rw2+0x4057]|=0x20
 # agent E e003: eb[1] bit28 U3MP_LOBBY_SIDE_INPLACE=1: PS3's online in-place re-dress (f_43a034 + f_439eac) instead of exit + entrance.
 if os.environ.get('U3MP_LOBBY_SIDE_INPLACE','0')=='1':image[rw2+0x4057]|=0x10
 # ===== end agent E switches =====
 # ===== agent G switches (eb[1] from bit27 down: byte rw2+0x4057 bits 3..0, then 0x4056 from bit7) =====
 # agent G g001: MP weapon art kept resident (PS3 NetInit multi-all). U3MP_MULTI_ALL=0: eb[1] bit27 (off);
 #   U3MP_MULTI_ALL=2: eb[1] bit26 (only the 10 gun actors + pistol-mp-base / rifle-mp-base).
 # agent I i005: default back to 1 (the full multi-all list, PS3 NetInit); i004's 2 rested on a wrong 'nothing loaded' diagnostic.
 if os.environ.get('U3MP_MULTI_ALL','1')=='0':image[rw2+0x4057]|=0x08
 if os.environ.get('U3MP_MULTI_ALL','1')=='2':image[rw2+0x4057]|=0x04
 # ===== end agent G switches =====
 # ===== agent I switches (eb[1] from bit16 up: byte rw2+0x4056 bits 0.., clear of B's bits from 0 up (now 14) and G's from 27 down) =====
 # agent I i001: PS3 1.19 party box. U3MP_PARTY119=0: eb[1] bit16 (old interim source; the click stub is not installed either);
 #   U3MP_PARTY_FOCUS=0: eb[1] bit17 (the box stays unfocusable, no focus keeper).
 if os.environ.get('U3MP_PARTY119','1')=='0':image[rw2+0x4056]|=0x01
 if os.environ.get('U3MP_PARTY_FOCUS','1')=='0':image[rw2+0x4056]|=0x02
 # agent I i003: cinema tab-bar panel and transport quads in the deferred UI pass (dc 0). U3MP_CIN_DC0=0: eb[1] bit18 (b026's dc).
 if os.environ.get('U3MP_CIN_DC0','1')=='0':image[rw2+0x4056]|=0x04
 # i005: U3MP_STATS_VIEW=0 -> eb[1] bit19: Statistics shows PAGE 0x83f24f04 without the viewed-profile object (crashes, as v6.08f)
 if os.environ.get('U3MP_STATS_VIEW','1')=='0':image[rw2+0x4056]|=0x08
 # ===== end agent I switches =====
 # ===== agent J switches (eb[1] bits 20..25: byte rw2+0x4056 bits 4..7, then 0x4057 bits 0..1; then bit15 = 0x4055 bit7) =====
 # agent J j001: U3MP_STATS_KEEP=0 -> eb[1] bit20: no per-frame re-assert of the Statistics card's load state (j01_tick)
 if os.environ.get('U3MP_STATS_KEEP','1')=='0':image[rw2+0x4056]|=0x10
 # agent J j002: U3MP_CIN_GAME_GUARD=0 -> eb[1] bit21: no net game index check before SetSelectedNetGameIdx (the film stub is left out too)
 if os.environ.get('U3MP_CIN_GAME_GUARD','1')=='0':image[rw2+0x4056]|=0x20
 # agent J j002: U3MP_BOOSTER_TITLE0=0 -> eb[1] bit22: PAGE_BOOSTERS page title answered in a021's place (after the table check)
 if os.environ.get('U3MP_BOOSTER_TITLE0','1')=='0':image[rw2+0x4056]|=0x40
 # agent J j002: U3MP_HIDE_UTV=0 -> eb[1] bit23: Options > Display keeps the Uncharted TV row
 if os.environ.get('U3MP_HIDE_UTV','1')=='0':image[rw2+0x4056]|=0x80
 # agent J j004: U3MP_EMBLEM_TEST=1/2/3 -> eb[1] bits 24/25 (byte rw2+0x4057 bits 0/1): emblem cell binding tests (see the j004 header)
 et=int(os.environ.get('U3MP_EMBLEM_TEST','3'))&3   # agent J j008/j009: 3 (the image in both the colour and the transparency slot) is the default fix
 if et:image[rw2+0x4057]|=et
 # agent J j007: U3MP_CIN_ZERO=0 -> eb[1] bit15 (byte rw2+0x4055 bit7): no zeroing of the film writer buffer at record start
 if os.environ.get('U3MP_CIN_ZERO','1')=='0':image[rw2+0x4055]|=0x80
 # agent J j010: eb[2] (byte rw2+0x4058, new agent J switch byte): bit0 U3MP_HUD_GAMMA=0 (no HUD colour decode), bit1
 #   U3MP_MENU_UNORM=0 (menus keep the pak's sRGB flag)
 if os.environ.get('U3MP_HUD_GAMMA','1')=='0':image[rw2+0x4058]|=0x01
 if os.environ.get('U3MP_MENU_UNORM','1')=='0':image[rw2+0x4058]|=0x02
 # agent J j011: U3MP_COOP_MODS119=0 -> eb[2] bit2: no co-op weapon row images / no gun refresh on mod toggles
 if os.environ.get('U3MP_COOP_MODS119','1')=='0':image[rw2+0x4058]|=0x04
 # agent J j011: U3MP_LOADSEL_GUN119=0 -> eb[2] bit3: in-match Loadout Selection gun keeps PS4's cell-size box
 if os.environ.get('U3MP_LOADSEL_GUN119','1')=='0':image[rw2+0x4058]|=0x08
 # agent J j012 experiments (opt-in, default off): eb[2] bit4 U3MP_EXPO_LOCK=1, bit5 U3MP_SKYFOG_OFF=1, bit6 U3MP_FOG_ALL_OFF=1
 if os.environ.get('U3MP_EXPO_LOCK','0')=='1':image[rw2+0x4058]|=0x10
 # x001: U3MP_LAN_WINSOCK=0 -> eb[2] bit29 (rw2+0x405b bit5): keep the port's sceNet stubs (no Winsock fallback)
 if os.environ.get('U3MP_LAN_WINSOCK','1')=='0':image[rw2+0x405b]|=0x20
 # x003: U3MP_LAN_LOOP=1 / 2 -> eb[2] bit30 / bit31: one-PC LAN test, this copy on 127.0.0.2 / 127.0.0.3
 if os.environ.get('U3MP_LAN_LOOP','0') in ('1','2'):image[rw2+0x405b]|={'1':0x40,'2':0x80}[os.environ['U3MP_LAN_LOOP']]
 if os.environ.get('U3MP_SKYFOG_OFF','0')=='1':image[rw2+0x4058]|=0x20
 if os.environ.get('U3MP_FOG_ALL_OFF','0')=='1':image[rw2+0x4058]|=0x40
 # ===== end agent J switches =====
 # ===== agent L switches (byte rw2+0x405a = eb[2] bits 16..23; all default ON, a set bit turns the part off) =====
 # l001: U3MP_UNLOCK_ALL=0 -> bit0: unlock evaluator native (stub left out) and the store bool not raised by l001
 if os.environ.get('U3MP_UNLOCK_ALL','1')=='0':image[rw2+0x405a]|=0x01
 # l001: U3MP_MAX_RANK=0 -> bit1: LAN profiles keep their own XP / legacy
 if os.environ.get('U3MP_MAX_RANK','1')=='0':image[rw2+0x405a]|=0x02
 # l001: U3MP_COOP_STAR=0 -> bit2: no co-op adventure bits set; kind 64 reads the star as a009 did
 if os.environ.get('U3MP_COOP_STAR','1')=='0':image[rw2+0x405a]|=0x04
 # l001: U3MP_ND_PAW=0 -> bit3: no paw field set; rank value bit 30 stays 0
 if os.environ.get('U3MP_ND_PAW','1')=='0':image[rw2+0x405a]|=0x08
 # l001: U3MP_UNLOCK_NEW=1 -> bit5: with unlock-all, unseen items show the new-item "!" (default: none, no seen bits written)
 if os.environ.get('U3MP_UNLOCK_NEW','0')=='1':image[rw2+0x405a]|=0x20
 # l001: U3MP_MAX_RANK_ONLINE=1 -> bit6: max rank / star / paw also for the profile Play Online loads (default LAN Party only)
 if os.environ.get('U3MP_MAX_RANK_ONLINE','0')=='1':image[rw2+0x405a]|=0x40
 # l001: U3MP_BADGE_LAYERS=0 -> bit4: menu badges as before (one texture; hook left out)
 if os.environ.get('U3MP_BADGE_LAYERS','1')=='0':image[rw2+0x405a]|=0x10
 # l002: U3MP_MENU_UNORM119=0 -> bit7: menu draws outside the type-2 icon quad sample sRGB textures decoded again (stub left out)
 if os.environ.get('U3MP_MENU_UNORM119','1')=='0':image[rw2+0x405a]|=0x80
 # fixer W w001: U3MP_MENU_SRGB_RAW=0 -> byte rw2+0x405f bit0 (eb[3] bit24): menu material textures sampled sRGB-decoded again (gate ea[240] bit0 stays 0)
 if os.environ.get('U3MP_MENU_SRGB_RAW','0')=='0':image[rw2+0x405f]|=0x01
 # fixer W w002: U3MP_HUD_TYPE2_SRGB=0 -> byte rw2+0x405f bit1 (eb[3] bit25): HUD-pass type-2 textures (medals, ranks, emblems) stay UNORM (gate ea[240] bit1 stays 0)
 if os.environ.get('U3MP_HUD_TYPE2_SRGB','0')=='0':image[rw2+0x405f]|=0x02
 # fixer W w003: U3MP_TAB_BOX119=0 -> byte rw2+0x405f bit2 (eb[3] bit26): PS3 item type 0x2d (tab boxes / tab rule) not drawn again (gate ea[240] bit2 stays 0)
 if os.environ.get('U3MP_TAB_BOX119','1')=='0':image[rw2+0x405f]|=0x04
 # fixer W w004: U3MP_MEDALS119=0 -> byte rw2+0x405f bit3 (eb[3] bit27): medal list built for the active page's tab and from the raw medals.bin order again (collapsed panel on open, 1 / 67)
 if os.environ.get('U3MP_MEDALS119','1')=='0':image[rw2+0x405f]|=0x08
 # fixer W w006: U3MP_DIALOG119=0 -> byte rw2+0x405f bit4 (eb[3] bit28): dialog icons at PS4 hud.pak size again (gate ea[240] bit3 stays 0)
 if os.environ.get('U3MP_DIALOG119','1')=='0':image[rw2+0x405f]|=0x10
 # fixer W w007: U3MP_HUD_MEDAL_SRGB=0 -> byte rw2+0x405f bit5 (eb[3] bit29): in-match medal icons (vtex net-medals/*) keep the UNORM view in the HUD pass (gate ea[240] bit4 stays 0; the walker stubs are then left out too)
 if os.environ.get('U3MP_HUD_MEDAL_SRGB','1')=='0':image[rw2+0x405f]|=0x20
 # fixer W w008: U3MP_TAB_BOX_PS3COL=0 -> byte rw2+0x405f bit6 (eb[3] bit30): h015's tab boxes keep PS3's raw item colours 0x80808080 / 0xa0000000 (flat grey / near-black boxes)
 if os.environ.get('U3MP_TAB_BOX_PS3COL','1')=='0':image[rw2+0x405f]|=0x40
 # fixer W w009: U3MP_COOP_HERO119=0 -> byte rw2+0x405f bit7 (eb[3] bit31): Co-op Adventure keeps the player's own hero (lobby and spawn)
 if os.environ.get('U3MP_COOP_HERO119','1')=='0':image[rw2+0x405f]|=0x80
 # ===== end agent L switches =====
 # ===== agent S switches (byte rw2+0x405b = eb[2] bits 24..31; a set bit means that part is off) =====
 # s014: U3MP_NET_HEAP_ALIGN16=0 -> bit0: wrapped-malloc (network heap 0x14 / 0x15 alias) keeps the requested alignment (stub left out)
 if os.environ.get('U3MP_NET_HEAP_ALIGN16','1')=='0':image[rw2+0x405b]|=0x01
 # s016: U3MP_PREVIEW119=0 -> bit1: no lobby character preview on list moves (0x995d50 as PS4 has it)
 if os.environ.get('U3MP_PREVIEW119','1')=='0':image[rw2+0x405b]|=0x02
 # s017: U3MP_PREVIEW_POLL=0 -> bit2: no per-frame Character / Customise page poll (s016's stub only)
 if os.environ.get('U3MP_PREVIEW_POLL','1')=='0':image[rw2+0x405b]|=0x04
 # s018: U3MP_CHAR_TYPE64=0 -> bit3: PS4's 'type < 64' assert in GetDesc stays (the poll then skips characters 64..78)
 if os.environ.get('U3MP_CHAR_TYPE64','1')=='0':image[rw2+0x405b]|=0x08
 # s019: U3MP_PKG_PRUNE=0 -> bit4: loadout_tick's lobby package list is add-only again (no drop of parts no look uses)
 if os.environ.get('U3MP_PKG_PRUNE','1')=='0':image[rw2+0x405b]|=0x10
 # ===== end agent S switches =====
 # ===== fixer H switches (ExtState.ec[1] = rw2+0x4454..0x4457; a set bit means that part is off) =====
 # h001: U3MP_REDRESS119=0 -> ec[1] bit0: no in-place lobby character re-dress (PS3 f_439704 / f_4394c0); s016/s017 preview path as before
 if os.environ.get('U3MP_REDRESS119','1')=='0':image[rw2+0x4454]|=0x01
 # h002: U3MP_VOICE119=0 -> ec[1] bit1: MP voices back to PS4's rule (every Player speaks as drake); U3MP_VOICE_FALLBACK=0 -> bit2:
 #   strict 1.19 voice (characters whose MP lines PS4 lacks go silent instead of keeping drake)
 if os.environ.get('U3MP_VOICE119','1')=='0':image[rw2+0x4454]|=0x02
 # h003: strict 1.19 voices are the default now that the PS3 MP speech is installed (batch b96sp, speech/<lang>/*.xvag loose
 #   in overlay-r3; FIOS's psarc dearchiver passes names missing from the archive to disk: the game's context flags = 1).
 #   U3MP_VOICE_FALLBACK=1 brings back h002's drake fallback.
 if os.environ.get('U3MP_VOICE_FALLBACK','0')=='0':image[rw2+0x4454]|=0x04
 # h004: U3MP_PARTMP119=0 -> ec[1] bit3: the helper does not want the MP particle modules part-multiplayer / part-obj-shade-char
 if os.environ.get('U3MP_PARTMP119','1')=='0':image[rw2+0x4454]|=0x08
 # x028: U3MP_VOX_VOICEFIX=1 -> ec[1] bit30: a SayRequest that arrives with voice 0 gets ec[201] (last mesh pick) or drake
 if os.environ.get('U3MP_VOX_VOICEFIX','0')=='1':image[rw2+0x4454]|=0x40000000
 # h005: U3MP_PLAYLIST_IMG119=0 -> ec[1] bit4: Find Game playlist picture left as PS4's type 0x26 (empty box)
 if os.environ.get('U3MP_PLAYLIST_IMG119','1')=='0':image[rw2+0x4454]|=0x10
 # h008: U3MP_KOTH_OWNER=0 -> ec[1] bit5: no local owner fallback / particle refresh for net-territory zones (h08_d still runs)
 if os.environ.get('U3MP_KOTH_OWNER','1')=='0':image[rw2+0x4454]|=0x20
 # h009: U3MP_NEW_ICON119=0 -> ec[1] bit7: the 0x640ac0 stub answers nothing (new marker / map image items stay undrawn)
 if os.environ.get('U3MP_NEW_ICON119','1')=='0':image[rw2+0x4454]|=0x80
 # h011: U3MP_MM_CARD119=0 -> ec[1] bit8 (rw2+0x4455 bit0): matchmaking map card picture fitted 2:1 as PS4 (texts outside it)
 if os.environ.get('U3MP_MM_CARD119','1')=='0':image[rw2+0x4455]|=0x01
 # h013: U3MP_TAB_BOX_CYCLE=1 -> ec[1] bit9: tab boxes cycle w003 quad / queued quad / native type-3 / magenta every 4 s (test only)
 if os.environ.get('U3MP_TAB_BOX_CYCLE','0')=='1':image[rw2+0x4455]|=0x02
 # h013: U3MP_TAB_BOX_NATIVE=0 -> ec[1] bit10: tab boxes drawn by w003's own quad again (v6.40, invisible)
 if os.environ.get('U3MP_TAB_BOX_NATIVE','1')=='0':image[rw2+0x4455]|=0x04
 # h014: U3MP_TAB_BOX_FLIP=1 -> ec[1] bit11: tab box drawn upside down (PS3's 180-degree gradient), opt-in
 if os.environ.get('U3MP_TAB_BOX_FLIP','0')=='1':image[rw2+0x4455]|=0x08
 # h015: U3MP_TAB_BOX_TEX=1 -> ec[1] bit12: tab box as h014's textured type-3 icon instead of PS4's box type 0x1b
 if os.environ.get('U3MP_TAB_BOX_TEX','0')=='1':image[rw2+0x4455]|=0x10
 # h020: U3MP_UI_BIND_DIAG=1 -> ec[1] bit13 (rw2+0x4455 bit5): the opt-in UI bind diagnostic (stub built only then)
 if os.environ.get('U3MP_UI_BIND_DIAG','0')=='1':image[rw2+0x4455]|=0x20
 # h022: U3MP_MENU_HUDLIST_RAW=0 -> ec[1] bit16 (rw2+0x4456 bit0): lobby menus keep sRGB in the HUD list (A off);
 #   U3MP_MENU_HUDLIST_MATCH=1 -> ec[1] bit17 (rw2+0x4456 bit1): also in a match while a loadout page is open (B on)
 if os.environ.get('U3MP_MENU_HUDLIST_RAW','1')=='0':image[rw2+0x4456]|=0x01
 if os.environ.get('U3MP_MENU_HUDLIST_MATCH','1')=='1':image[rw2+0x4456]|=0x02
 # fixer H h024: U3MP_EXPO_TABLE=1 -> ec[1] bit18 (byte rw2+0x4456 bit2): calibrated per-object auto-exposure EV (h24_tab); a022's B follows it
 if os.environ.get('U3MP_EXPO_TABLE','0')=='1':image[rw2+0x4456]|=0x04
 # h021: U3MP_OPTIONS_CREDITS=1 -> ec[1] bit14 (rw2+0x4455 bit6): Options > Credits stays visible while MP is active
 if os.environ.get('U3MP_OPTIONS_CREDITS','0')=='1':image[rw2+0x4455]|=0x40
 # h008: U3MP_HUD_RING_DECODE=0 -> ec[1] bit6: HUD progress ring colours not sRGB-decoded (pale grey-green as v6.36)
 if os.environ.get('U3MP_HUD_RING_DECODE','1')=='0':image[rw2+0x4454]|=0x40
 # ===== end fixer H switches =====
 # ===== fixer L switches (ExtState.ec[2] = rw2+0x4458..0x445b; a set bit means that part is off / changed) =====
 # n003: U3MP_FRIENDS_HDR=0 -> ec[2] bit0: no Friends header / Triangle / PAGE_FRIENDS in offline mode (stubs, retargets left out)
 if os.environ.get('U3MP_FRIENDS_HDR','1')=='0':image[rw2+0x4458]|=0x01
 # n003: U3MP_FRIENDS_FB=1 -> ec[2] bit1: the header keeps PS4's " |FB|" glyph
 if os.environ.get('U3MP_FRIENDS_FB','0')=='1':image[rw2+0x4458]|=0x02
 # n004: U3MP_MM_END119=0 -> ec[2] bit2: m005's 5 s "GAME STARTING IN" and the select at the start (no PS3 f_32c620 port)
 if os.environ.get('U3MP_MM_END119','1')=='0':image[rw2+0x4458]|=0x04
 # n004: U3MP_MM_STAGE119=0 -> ec[2] bit3: n004's 45 s and select, but no scene leave / lobby stage switch / line-up
 if os.environ.get('U3MP_MM_STAGE119','1')=='0':image[rw2+0x4458]|=0x08
 # n006: U3MP_MM_N006=0 -> ec[2] bit4: n005's matchmaking left panel (b017 VOTE gate, W[0] for the vote, type-36 map image, team-tinted rows)
 if os.environ.get('U3MP_MM_N006','1')=='0':image[rw2+0x4458]|=0x10
 # n007: U3MP_HUD_ARGB=0 -> ec[2] bit5: PS4's marker colour unpack and t009's ring colour order (byte 3 as red)
 if os.environ.get('U3MP_HUD_ARGB','1')=='0':image[rw2+0x4458]|=0x20
 # n008: U3MP_MM_VOTE119=0 -> ec[2] bit6: the VOTE list answers only PS3's query ids (v6.36: card only, no list, not focusable)
 if os.environ.get('U3MP_MM_VOTE119','1')=='0':image[rw2+0x4458]|=0x40
 # n009: U3MP_MM_COOP119=0 -> ec[2] bit7: no co-op difficulty vote after the map vote (n008's flow: 45 s GAME STARTING for every playlist)
 if os.environ.get('U3MP_MM_COOP119','1')=='0':image[rw2+0x4458]|=0x80
 # fixer W w010: U3MP_EMBLEM_LAYOUT119=0 -> ec[3] bit0 (rw2+0x445c bit0): emblem slider rows / bar track off; U3MP_BAR_TRACK_ALL=1 ->
 #   ec[3] bit1: the black bar track on every MP type-4 bar, not only PAGE_EMBLEM
 if os.environ.get('U3MP_EMBLEM_LAYOUT119','1')=='0':image[rw2+0x445c]|=0x01
 if os.environ.get('U3MP_BAR_TRACK_ALL','0')=='1':image[rw2+0x445c]|=0x02
 # n013: U3MP_MM_RETURN119=0 -> ec[3] bit8 (rw2+0x445d bit0): a spoofed Find Game match returns to PAGE_LOBBY as in v6.42
 if os.environ.get('U3MP_MM_RETURN119','1')=='0':image[rw2+0x445d]|=0x01
 # n017: U3MP_MM_SOLOHUNT=0 -> ec[3] bit9 (rw2+0x445d bit1): solo Hunter in a spoofed Find Game match ends at once (PS3 rule)
 if os.environ.get('U3MP_MM_SOLOHUNT','1')=='0':image[rw2+0x445d]|=0x02
 # ===== end fixer L switches =====
 # ===== agent F4 switches (byte rw2+0x405c = eb[3] bits 0..7; bits 0..5 opt-in f4001 experiments, a set bit means ON, MP only;
 #       bit6 f4002 fade48 fix, a set bit means OFF) =====
 # f4001: one candidate per build, each implies the f4001 stubs (see the agent F4 stubs block).
 for b_,k_ in enumerate(('U3MP_F4_PVSOFF','U3MP_F4_FRUSTOFF','U3MP_F4_LODOFF','U3MP_F4_FADEOFF','U3MP_F4_SHADERFB','U3MP_F4_DRAWALL')):
  if os.environ.get(k_,'0')=='1':image[rw2+0x405c]|=1<<b_
 # f4002: U3MP_FADE48_FIX=0 -> bit6 (a set bit means OFF here): the fade-record safety net runs the original NULL path
 if os.environ.get('U3MP_FADE48_FIX','1')=='0':image[rw2+0x405c]|=0x40
 # ===== end agent F4 switches =====
 # ===== agent U2 switches (byte rw2+0x405d = eb[3] bits 8..15; a set bit means that part is off) =====
 # u001: U3MP_LOADSEL_GUN_PS3H=0 -> bit0: in-match Loadout Selection gun box height back to j011's 64.0 (default 51.2 = PS3)
 if os.environ.get('U3MP_LOADSEL_GUN_PS3H','1')=='0':image[rw2+0x405d]|=0x01
 # u001: U3MP_GUNICON119=0 -> bit1: type-0x21 weapon items keep PS4's size formula (stub stays, runs the original bytes)
 if os.environ.get('U3MP_GUNICON119','1')=='0':image[rw2+0x405d]|=0x02
 # ===== end agent U2 switches =====
 # ===== agent K switches (byte rw2+0x4059 = eb[2] bits 8..15) =====
 # k001: U3MP_ONLINE_SKIP_PROFILE=0 -> bit0: Play Online opens the LAN profile page again
 if os.environ.get('U3MP_ONLINE_SKIP_PROFILE','1')=='0':image[rw2+0x4059]|=0x01
 # k001: U3MP_SETTINGS119=0 -> bit1: Custom Game > Settings as before (no row hiding, Friendly Fire style 13, no Time Limit number)
 if os.environ.get('U3MP_SETTINGS119','1')=='0':image[rw2+0x4059]|=0x02
 # ===== end agent K switches =====
 # ===== agent M switches (byte rw2+0x4059 bits 2..7 = eb[2] 0x400..0x8000) =====
 # m002: U3MP_WEAPON_MODE119=0 -> bit2: no 1.19 weapon-set test in the inventory init (stub also left out)
 if os.environ.get('U3MP_WEAPON_MODE119','1')=='0':image[rw2+0x4059]|=0x04
 # m002: U3MP_COMMUNITY119=0 -> bit3: Community > Cinema / Leaderboards buttons do nothing again
 if os.environ.get('U3MP_COMMUNITY119','1')=='0':image[rw2+0x4059]|=0x08
 # m002: U3MP_LB_LOCAL=0 -> bit4: with no community server the leaderboard query fails (net error) instead of the local row
 if os.environ.get('U3MP_LB_LOCAL','1')=='0':image[rw2+0x4059]|=0x10
 # m003: U3MP_MM_SPOOF=0 -> bit5: offline Find Game shows the network-failure dialog again (no spoofed matchmaking)
 if os.environ.get('U3MP_MM_SPOOF','1')=='0':image[rw2+0x4059]|=0x20
 # m003: U3MP_MM_SPOOF_START=0 -> bit6: the spoofed matchmaking stops at "Waiting for players..." (no local match)
 if os.environ.get('U3MP_MM_SPOOF_START','1')=='0':image[rw2+0x4059]|=0x40
 # m004: U3MP_GAMETYPES119=0 -> bit7: 1.10 game type list, table at 0x17d7b30, no playlist / pack-1 fixes
 if os.environ.get('U3MP_GAMETYPES119','1')=='0':image[rw2+0x4059]|=0x80
 # ===== agent M byte rw2+0x405e =====
 # m005: U3MP_MM_FILL119=0 -> bit0: the m003 spoof flow (no Close Game / vote sequence, no matchmaking-page LAN-byte wrappers)
 if os.environ.get('U3MP_MM_FILL119','1')=='0':image[rw2+0x405e]|=0x01
 # agent L n002: U3MP_MM_N002=0 -> bit1: no Profile-only LIST_MATCHMAKING and no back-out on "Leave Find Game?" yes (offline)
 if os.environ.get('U3MP_MM_N002','1')=='0':image[rw2+0x405e]|=0x02
 # fixer L n022: U3MP_MM_COOPLIST119=0 -> rw2+0x405e bit2 (eb[3] 0x40000): offline-mode playlist lookups keep the LAN co-op filter (v6.47)
 if os.environ.get('U3MP_MM_COOPLIST119','1')=='0':image[rw2+0x405e]|=0x04
 # fixer L n023: U3MP_TERR_TEST=1 -> rw2+0x405e bit3 (an unowned zone becomes the local team's after 10 s), =2 -> bit4 (the other team's): opt-in floor-colour test aid
 if os.environ.get('U3MP_TERR_TEST','0')=='1':image[rw2+0x405e]|=0x08
 if os.environ.get('U3MP_TERR_TEST','0')=='2':image[rw2+0x405e]|=0x10
 # fixer L n024: U3MP_MEDAL_TEST=1 -> rw2+0x405e bit5: opt-in medal popups in a match (game's own 0x5ae240), for w007 checks
 # fixer L n024/n025: U3MP_MEDAL_TEST=1 -> bit5 (medal popups), =2 -> bit7 (rank-up / prestige popups), =3 -> both
 if os.environ.get('U3MP_MEDAL_TEST','0') in ('1','3'):image[rw2+0x405e]|=0x20
 if os.environ.get('U3MP_MEDAL_TEST','0') in ('2','3'):image[rw2+0x405e]|=0x80
 # fixer L n025: U3MP_HUD_RANK_SRGB=0 -> bit6: rank-up popup badges keep the native (one encode too bright) view
 if os.environ.get('U3MP_HUD_RANK_SRGB','1')=='0':image[rw2+0x405e]|=0x40
 # fixer L n028: U3MP_HUD_RANK_ITEM=0 -> ExtState ea[170] bit0: type-0x29 rank badge items (HUD rank-up popup) stay undrawn
 if os.environ.get('U3MP_HUD_RANK_ITEM','1')=='0':image[rw2+0x3c50+170*4]|=0x01
 # fixer L n032: ExtState ea[122] bits: bit0 U3MP_RECORDS119=0 (Profile > Records keeps the 3 LAN rows), bit1 U3MP_CARD_TABS=0 (player card
 # tab bar / body stay native: no Competitive / Co-op tabs with the LAN byte set), bit2 U3MP_N032_OFFLINE_ONLY=1 (both only in offline mode)
 if os.environ.get('U3MP_RECORDS119','1')=='0':image[rw2+0x3c50+122*4]|=0x01
 if os.environ.get('U3MP_CARD_TABS','1')=='0':image[rw2+0x3c50+122*4]|=0x02
 if os.environ.get('U3MP_N032_OFFLINE_ONLY','0')=='1':image[rw2+0x3c50+122*4]|=0x04
 # fixer L n033: ea[122] bit3 = ring draw diagnostic on (U3MP_N033=1 or 2, which also builds the stubs), bit4 = constant fix (=2)
 if os.environ.get('U3MP_N033','0') in ('1','2'):image[rw2+0x3c50+122*4]|=0x08
 if os.environ.get('U3MP_N033','0')=='2':image[rw2+0x3c50+122*4]|=0x10
 # ===== end agent M switches =====
 # s002 (SHORTCUTS agent; RS2 bits are taken from the top down, 0x80000000 first, to stay clear of FUNCTIONS' bits):
 # U3MP_NET_CLOCK=1 (s004: opt-in; RS2 0x80000000 = state+0x3d7b bit7 stays clear) uses PS3's net clock = engine clock 7. Default:
 # the bit is preset and the helper keeps its own clock at state+0x2000 (v5.49e: with clock 7 solo co-op Arena left WaitReady).
 # f002: default flipped: clock 7 (PS3 1:1) unless U3MP_NET_CLOCK=0 (bisect: helper-owned clock, bit preset).
 if os.environ.get('U3MP_NET_CLOCK','1')=='0':image[state+0x3d7b]|=0x80
 # s002: U3MP_PS3_SPAWN=1 presets RS2 mode bit 0 (state+0x3d78 bit0): the local player spawns only the PS3 way, from the restored
 # f_363ed4 state machine (state 0 region want -> 1 spawner levels in -> 2 spawn, kinds 17/18); the host answer 0x787a00 just
 # stores the request (PS3 f_360af8 returns 0 for a local player). Without it the 0x787b73 stub also spawns at the host answer
 # (the v4.84 shortcut). f003: default ON (passed in game: TDM Highrise, co-op Arena Chateau: spawn, death, respawn, RS2[1]=0,
 # 0 faults). U3MP_PS3_SPAWN=0 brings back the v4.84 host-answer spawn (bisect only).
 if os.environ.get('U3MP_PS3_SPAWN','1')!='0':image[state+0x3d78]|=1
 # s003: U3MP_EMBLEM_NETINIT=0 (RS2 0x40000000, state+0x3d7b bit6) leaves the emblem table fill out of the NetInit point (PS3
 # f_2901e0 -> f_278b24 -> f_27887c); U3MP_START_RESET=0 (RS2 0x20000000, bit5) drops the per-Start-Game repeat of PS3's
 # NetInit-time abort-flag clear and Session Init (KEEP for the solo LAN backend, default on).
 if os.environ.get('U3MP_EMBLEM_NETINIT','1')=='0':image[state+0x3d7b]|=0x40
 if os.environ.get('U3MP_START_RESET','1')=='0':image[state+0x3d7b]|=0x20
 # s005: U3MP_KR_VICTIMFLAG=0 (RS2 0x10000000, state+0x3d7b bit4) leaves out the kill-reward rec0 +0x25 victim flag (PS3 f_381ae4).
 if os.environ.get('U3MP_KR_VICTIMFLAG','1')=='0':image[state+0x3d7b]|=0x10
 # s006: SHORTCUTS switch word at state+0x20c0 (RS2 bits 0x10000000..0x80000000 are all in use). bit0: U3MP_HUD_ALIVE=0 leaves out
 # the per-frame HUD group 0x100000 request of the restored f_363ed4 (PS3 f_972e1c = PS4 0x11b7d60).
 if os.environ.get('U3MP_HUD_ALIVE','1')=='0':image[state+0x20c0]|=1
 # s007: bit1 U3MP_KEEP_LOADOUT=1: old NOP at 0x72bda9 (above) and loadout_tick keeps re-requesting the kit list in the match.
 if os.environ.get('U3MP_KEEP_LOADOUT','0')=='1':image[state+0x20c0]|=2
 # s008: bit2 U3MP_TEAMCHK_GUESS=1: the Start Match type-10 team check counts every member as team 0 again (old guess) instead
 # of reading the member data team byte +9 like PS3 f_292e3c.
 if os.environ.get('U3MP_TEAMCHK_GUESS','0')=='1':image[state+0x20c0]|=4
 # s010: switches for kept shortcuts that had none (each stays default on; see the helper comments for why it stays).
 # bit3 U3MP_NET_HEAP_ALIAS=0: kAllocNetwork/NetworkProcess (0x14/0x15) not aliased to a general heap (bisect only).
 # bit4 U3MP_COOP_2NDUSER=1: PS3's "no second user" dialog 0x7c31f4d0 on the co-op LAN choice (blocks solo co-op).
 # bit5 U3MP_PENDING_DROP=0: failed (state 0xc) disc-missing levels stay in the respawn's pending list (PS3 had none).
 # bit6 U3MP_STAGE_GUARD=0: no lobby stage guard / san-upper-multi-stage fallback.
 # bit7 U3MP_DLC_PACKS=0: DLC pack "installed" flags (0x17c3a40[idx]) left as PS4 sets them.
 if os.environ.get('U3MP_NET_HEAP_ALIAS','1')=='0':image[state+0x20c0]|=8
 if os.environ.get('U3MP_COOP_2NDUSER','0')=='1':image[state+0x20c0]|=0x10
 if os.environ.get('U3MP_PENDING_DROP','1')=='0':image[state+0x20c0]|=0x20
 if os.environ.get('U3MP_STAGE_GUARD','1')=='0':image[state+0x20c0]|=0x40
 if os.environ.get('U3MP_DLC_PACKS','1')=='0':image[state+0x20c0]|=0x80
 # v5.60: bit8 U3MP_NET_SHUTDOWN=0: backing out of MP to the front end leaves the LAN/MP-active flags set (no PS3 NetShutdown
 # f_28cb80 point; the title then keeps the lobby stage). Leave count at state+0x20c4.
 if os.environ.get('U3MP_NET_SHUTDOWN','1')=='0':image[state+0x20c1]|=1
 # f003: bit9 (state+0x20c1 bit1) U3MP_DEATH_GUARD=0: a death anim missing from the player's tables traps in NetEventPlayerDeath
 # as on PS3/retail PS4 (default: that one send is skipped and counted in ExtState.dth[6]).
 if os.environ.get('U3MP_DEATH_GUARD','1')=='0':image[state+0x20c1]|=2
 oldnames=raw[np:np+ni*128];oldrelocs=raw[np+ni*128:ip]
 assert len(oldrelocs)==nr*32
 # agent L n001: m004 moved the NetMenuHost table to rw2+0xed000 and repointed the 11 'lea r,[rip->0x17d7b30]' sites, but PS4
 #   also reaches the table through ONE GOT slot, 0x16e3140 (image relocation {0x16e3140, type 0, target 0x17d7b30}). Its 3 users
 #   are the Custom Game Map list source 0x753330 (op 2 name at 0x753447, op 4 image at 0x753533) and the variation source
 #   0x753850 (0x753955: per-map record count). They read the OLD table, which nothing fills any more, so the Map list had its
 #   row count (op 6 goes through the repointed lea) but NULL map entries: a highlight bar and no names (user shot v6.31).
 #   Retarget that relocation to the moved table. Same gate as m004.
 if os.environ.get('U3MP_GAMETYPES119','1')!='0':
  _rl=bytearray(oldrelocs);_hit=0
  for _i in range(nr):
   _a,_t,_x,_y=struct.unpack_from('<4Q',_rl,_i*32)
   if _t==0 and 0x17d7b30<=_x<0x17d9988:
    if _a!=0x16e3140:raise ValueError('agent L n001: unexpected NetMenuHost relocation %#x'%_a)
    struct.pack_into('<Q',_rl,_i*32+16,rw2+0xed000+(_x-0x17d7b30));_hit+=1
  if _hit!=1:raise ValueError('agent L n001: NetMenuHost GOT relocation not found')   # the slot itself is 0 in the image (filled at load)
  oldrelocs=bytes(_rl)
 # fixer L n003: the Friends header vtable slots -> the kind 244 value stubs (see the fixer L stubs block)
 if n03_reloc:
  _rl=bytearray(oldrelocs);_hit=0
  for _i in range(nr):
   _a,_t,_x,_y=struct.unpack_from('<4Q',_rl,_i*32)
   if _a in n03_reloc:
    if _t!=0 or _x!=n03_reloc[_a][0]:raise ValueError('fixer L n003: unexpected vtable relocation %#x'%_a)
    struct.pack_into('<Q',_rl,_i*32+16,n03_reloc[_a][1]);_hit+=1
  if _hit!=2:raise ValueError('fixer L n003: Friends header vtable relocations not found')
  oldrelocs=bytes(_rl)
 # v5.47: kernel imports for the cinema port (only with U3MP_CINEMA=1), relocated into ExtState.kimp (rw2+0x3210).
 # f002: all four are NIDs the retail image already imports (Getdirentries replaces Getdents, which retail does not import),
 # so the PC port needs nothing new.
 KIMPORTS=[('1-LFLmRFxxM','sceKernelMkdir','libkernel'),('taRWhTJFTgE','sceKernelGetdirentries','libkernel'),('AUXVxWeJU-A','sceKernelUnlink','libkernel'),('eV9wAD2riIA','sceKernelStat','libkernel')] if CINEMA else []
 # agent B b001: libc time / localtime / gmtime (all three in the retail import table: wLlFkwG9UcQ#libc, efhK-YSUYYQ#libSceLibcInternal,
 # 1mecP7RgI2A#libc) for the film header time and the LIST_CINEMA date column, written to ExtState.eb[4..9] (rw2+0x4060..0x4077).
 if CINEMA:KIMPORTS+=[('wLlFkwG9UcQ','time','libc'),('efhK-YSUYYQ','localtime','libSceLibcInternal'),('1mecP7RgI2A','gmtime','libc')]
 KIMP_AT=[rw2+0x3210+i*8 for i in range(4)]+[rw2+0x4060,rw2+0x4068,rw2+0x4070]
 # agent B b010: sceKernelUsleep (1jfXLRVzisc#libkernel, in the retail import table) into ExtState.eb[10..11] (rw2+0x4078): the
 # LAN backend's Create / Join wait PS3's 30 x sys_timer_usleep(100000) for the room state (f_382d9c / f_382af0).
 KIMP_AT=KIMP_AT[:len(KIMPORTS)]+[rw2+0x4078];KIMPORTS=KIMPORTS+[('1jfXLRVzisc','sceKernelUsleep','libkernel')]
 names=b''.join((nid+'#'+library).encode().ljust(128,b'\0') for nid,_,library in IMPORTS+NET_IMPORTS+KIMPORTS)
 if ext_data_size<0x280:raise ValueError('helper ext data lacks the net import table (ExtState.net at +0x200)')
 native_bindings=[0x651940,0x11f2bf0,0x11f85e0,pool]
 natives=NATIVES
 relocs=b''.join(struct.pack('<4Q',state+i*8,1,ni+i,0) for i in range(len(IMPORTS)))+b''.join(struct.pack('<4Q',rw2+0x200+i*8,1,ni+len(IMPORTS)+i,0) for i in range(len(NET_IMPORTS)))+b''.join(struct.pack('<4Q',state+648+i*8,0,target,0) for i,target in enumerate(native_bindings))+b''.join(struct.pack('<4Q',state+768+i*8,0,target,0) for i,target in enumerate(natives))+b''.join(struct.pack('<4Q',KIMP_AT[i],1,ni+len(IMPORTS)+len(NET_IMPORTS)+i,0) for i in range(len(KIMPORTS)))
 added_relocs=len(IMPORTS)+len(NET_IMPORTS)+len(native_bindings)+len(natives)+len(KIMPORTS)
 header=bytearray(raw[:sp]+raw[sp:np]+struct.pack('<3Q',rx,0x10000,5)+struct.pack('<3Q',rw,0x4000,6)+struct.pack('<3Q',rx2,EXT_CODE_SIZE,5)+struct.pack('<3Q',rw2,EXT_SEG,6)+oldnames+names+oldrelocs+relocs)
 struct.pack_into('<Q',header,8,len(image));struct.pack_into('<Q',header,24,ns+4);struct.pack_into('<Q',header,32,nr+added_relocs);struct.pack_into('<Q',header,40,ni+len(IMPORTS)+len(NET_IMPORTS)+len(KIMPORTS))
 output=bytes(header+image);new=parse(output)
 assert new[0]==len(image) and new[1]==ns+4 and new[5][-4:]==[(rx,0x10000,5),(rw,0x4000,6),(rx2,EXT_CODE_SIZE,5),(rw2,EXT_SEG,6)]
 assert output[new[4]+helper:new[4]+helper+len(blob)]==blob and output[new[4]+rx2:new[4]+rx2+len(ext_code)]==ext_code
 return output,dict(status='LAN profile to resource-loading to local lobby experiment; no session backend',source_sha256=EXPECTED,sha256=hashlib.sha256(output).hexdigest(),helper_sha256=hashlib.sha256(blob).hexdigest(),code=hex(rx),state=hex(state),ext_code=hex(rx2),ext_code_used=hex(len(ext_code)),ext_data=hex(rw2),ext_data_used=hex(ext_data_size),helper_used=hex(len(blob)),native_bindings=[hex(v) for v in native_bindings],natives=[hex(v) for v in natives],pool_helper_sha256=hashlib.sha256(pool_blob).hexdigest(),imports=[dict(nid=n,name=s,library=l) for n,s,l in IMPORTS+NET_IMPORTS+KIMPORTS],wrappers=wrappers,profile_file='/data/u3-lan-profiles.v1',mp_profile_files='/data/u3-lan-mp-<slot>.v1 (coop: /data/u3-lan-mp-coop-<slot>.v1)',limitations=['Profile list adapter is restored; native session identity initialization still pending','Local PC profile persistence uses a versioned format; PS3 save data is not transplanted'])