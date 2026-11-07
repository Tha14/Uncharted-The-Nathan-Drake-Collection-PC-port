"""u2_netmap.py - function map of Uncharted 2's multiplayer FRONTEND, SESSION and NETWORK code,
PS3 1.10 against PS4 1.02 (read-only; prints and writes addresses, names, ids and short
strings only; no code bytes).

Usage:
  python u2_netmap.py <PS3 EBOOT.elf> <PS4 boot-linked.bin> <PS4 eboot.elf> tsv  <out.tsv>
  python u2_netmap.py <PS3 EBOOT.elf> <PS4 boot-linked.bin> <PS4 eboot.elf> md   <out.md>
  python u2_netmap.py <PS3 EBOOT.elf> <PS4 boot-linked.bin> <PS4 eboot.elf> show <ps3hex> ...
  python u2_netmap.py <PS3 EBOOT.elf> <PS4 boot-linked.bin> <PS4 eboot.elf> netsm

Inputs it reuses (all read-only):
  ps3_elf / ps3_map    PS3 analysis cache (work/ps3-110)
  u2_func_match.py     PS3 -> PS4 matches (work/match/match.pickle; build it first)
  ps4_cg.py            PS4 call graph, stub detection, liveness
  ps4_imports.py       PS4 import names (which NP/net APIs the PS4 executable links at all)
  work/ps3-110/menu-callbacks.txt   menu control id -> PS3/PS4 callback (u2_menu_callbacks.py)
PS4 function length comes from .eh_frame_hdr of the PS4 ELF (exact FDE bounds).

Scope = the PS3 address ranges in AREAS (frontend/menu flow, bootstrap+NetInit+teardown,
session/party/room/signaling, transport). Each in-scope PS3 function gets:
  role, PS3 address, PS4 address (or absent), PS4 state, evidence, porting note.
PS4 state:
  stub ret   matched PS4 function is a bare ret / ret 0 / ret constant
  intact     matched, has code, size and matched-callee coverage comparable to PS3
  partial    matched but much smaller, or most of the PS3 function's matched callees are not
             called any more, or the PS4 function is dead (no reference left)
  inlined    no own PS4 function; its features sit inside the PS4 counterpart of a caller
  absent     no PS4 counterpart. 'absent(api)' = the function calls a PS3 NP/net API whose PS4
             equivalent is not imported by the PS4 executable at all, so it cannot exist there;
             'absent(str)' = its unique strings are not in the PS4 image; 'absent' = not found.
netsm: every NET_SM state value written through SetNetState (0x4383cc) or the state global.
"""
import bisect, collections, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps3_elf as P
import ps3_map as M
import ps4_imports as PI
import u2_func_match as FM

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NET_STATE = 0xf11764
SET_NET_STATE = 0x4383cc

# (area, lo, hi, module) ; PS3 1.10 text ranges, object files are linked in name order
AREAS = [
    ('frontend', 0x4382f0, 0x43b698, 'flow.cpp: menu callbacks, bootstrap, MP control registration'),
    ('transport', 0x43b698, 0x43d6b0, 'flow.cpp: net clock, NpNetUpdate, NetConnectionUpdate'),
    ('frontend', 0x43d6b0, 0x447a5c, 'flow.cpp: TaskManagerOnline, NET_SM transitions, page handlers'),
    ('bootstrap', 0x447a5c, 0x449e88, 'flow.cpp: NetInit thread'),
    ('transport', 0x449e88, 0x44a400, 'flow.cpp: NdiNetUpdate (per-frame net update)'),
    ('transport', 0x464cb8, 0x4654d0, 'net-connection.cpp: reliability window, ack'),
    ('frontend', 0x4654d0, 0x466f00, 'net-custom-games.cpp: join / joining / custom game list'),
    ('transport', 0x466f00, 0x469060, 'net-connection.cpp/net-message: message layer'),
    ('session', 0x48f16c, 0x492800, 'net-friends: presence, friends, players met'),
    ('frontend', 0x4a9e80, 0x4abc00, 'net-leaderboard: score service + pages'),
    ('frontend', 0x4af108, 0x4b4244, 'net-matchmaking.cpp: matchmaking state machine and vote'),
    ('frontend', 0x4b7000, 0x4c2bc0, 'net-menu / lobby lists, roster query table, player cards'),
    ('session', 0x4c2bc0, 0x4c4aa8, 'net-party: party messages, party error'),
    ('session', 0x4cff50, 0x4d2700, 'net-session-manager.cpp: Matching2 context, callbacks'),
    ('session', 0x4d2700, 0x4d93e8, 'net-session.cpp: rooms, members, room data'),
    ('session', 0x4d987c, 0x4da608, 'slot manager'),
    ('transport', 0x4db1c0, 0x4dbe70, 'net-socket: UDP sockets'),
    ('transport', 0x4dbe70, 0x4dc6c8, 'net-stats: bandwidth'),
    ('bootstrap', 0x4e6cd0, 0x4e7a68, 'net-voice: cellVoice'),
    ('session', 0x4e8978, 0x4eae30, 'np-connection-manager: sceNpSignaling'),
    ('bootstrap', 0x4eae30, 0x4ec2a8, 'np-util: error text, NetCtl dialog, sceNp init/term'),
    ('frontend', 0x4ed000, 0x4f1d48, 'twitter page and thread'),
]

# curated roles (PS3 1.10). Everything else gets an automatic role from strings/imports/caller.
ROLES = {
    0x4383a4: 'clear net flags (f11758/f1175c)',
    0x4383cc: 'SetNetState(state) -> f11764',
    0x4383e4: 'GetNetState()',
    0x4384b0: 'IsOnline(): NetState > 1 (379 callers)',
    0x4386d0: 'MP cleanup / abort (Matching2 term, sockets, signaling, voice, commerce, score)',
    0x438a50: 'sceNpBasic handler "basicCb"',
    0x438bc8: 'MP bootstrap (NetCtl checks, PAGE_MULTIPLAYER_CONNECTING, starts NetInit thread)',
    0x438fa0: 'npManagerCallback (ticket)',
    0x43918c: 'register MP menu controls and pages',
    0x4399a0: 'store browse (sceNpCommerce2ExecuteStoreBrowse)',
    0x43a164: 'send invite (sceNpBasicSendMessageGui)',
    0x43a444: 'NET_SM: lobby status change -> START_MATCHMAKING / CLIENT_CONNECT_TO_HOST',
    0x43aab0: 'BUTTON_MULTIPLAYER_STARTMATCH',
    0x43b150: 'BUTTON_MULTIPLAYER callback',
    0x43b200: 'invite / message count',
    0x43b698: 'net clock "Net Clock = %f"',
    0x43bdd0: 'NpNetUpdate (connection management, timeouts, kick)',
    0x43cdb8: 'NetConnectionUpdate (recv loop, sendto)',
    0x43d6b0: 'TaskManagerOnline LoadLevels',
    0x43e99c: 'NET_SM -> START_MATCHMAKING',
    0x43ea8c: 'PAGE_MULTIPLAYER_LEAVING_GAME',
    0x43ec08: 'NET_SM -> RESULTS ("Save Game Stats" thread)',
    0x43edc8: 'NET_SM -> RESET',
    0x43ee8c: 'TaskManagerOnline Leave',
    0x43ef48: 'TaskManagerOnline EstablishConnection',
    0x43f0c0: 'TaskManagerOnline ShowRoundResults',
    0x43f17c: 'TaskManagerOnline ShowResults',
    0x43f238: 'TaskManagerOnline ResettingReady',
    0x43f2f4: 'TaskManagerOnline Resetting',
    0x43f3b0: 'TaskManagerOnline PreResetting',
    0x43f46c: 'TaskManagerOnline Playing',
    0x43f528: 'TaskManagerOnline WaitReady',
    0x43f5e8: 'LIST_PLAYERSMET',
    0x43f830: 'LIST_FRIENDS',
    0x43fb38: 'PAGE_MULTIPLAYER_POSTGAME',
    0x440620: 'BUTTON_MULTIPLAYER_PLAYER0..9',
    0x440948: 'BUTTON_MULTIPLAYER_INVITES',
    0x441440: 'BUTTON_CLANTAG',
    0x4417f0: 'BUTTON_MULTIPLAYER_PARTY_PLAYER0..9',
    0x442558: 'TaskManagerOnlineUpdate',
    0x4430d8: 'NET_SM -> ROUND_RESULTS',
    0x443bc8: 'PAGE_LOBBY',
    0x4443f8: 'PAGE_MULTIPLAYER_PLAYER_OPTIONS',
    0x444660: 'NET_SM -> UPDATE (starts the net game)',
    0x444bb0: 'NET_SM -> CREATE_GAME_WAIT (CreateJoinRoom)',
    0x445020: 'PAGE_MULTIPLAYER_CONNECTING',
    0x445608: 'NET_SM -> LEAVE_GAME, lobby choice SERVER/CLIENT',
    0x4465c8: 'PAGE_MULTIPLAYER (Callback_Multiplayer)',
    0x447248: 'menu-temple / MP level select helper',
    0x447a5c: 'NetInit thread (NP, tickets, NAT probes, sockets, session, signaling, voice, score)',
    0x449ae8: 'sceNpBasic event handler',
    0x449e88: 'NdiNetUpdate per frame (from FrameBegin 0x21db8)',
    0x464cb8: 'NetConnection ack processing / packet header',
    0x465138: 'NetConnection send window helper',
    0x465218: 'NetConnection read packet',
    0x4652cc: 'NetConnection write packet / window full',
    0x4654d0: 'PAGE_MULTIPLAYER_JOIN',
    0x4655b0: 'custom game list (BUTTON_MULTIPLAYER_JOIN)',
    0x465808: 'BUTTON_MULTIPLAYER_SELECT_JOIN_GAME',
    0x465b70: 'host asked to leave the room',
    0x465da0: 'custom game: host/join decision',
    0x4661e0: 'PAGE_MULTIPLAYER_JOINING (client join state machine)',
    0x467538: 'message layer: wait seq',
    0x467600: 'message layer: pack (1200 byte packet)',
    0x467d20: 'message layer: expire',
    0x48f16c: 'NetFriends set presence',
    0x48f350: 'NetFriends::RoomMemberCallback',
    0x490040: 'NetFriends friend list thread',
    0x490338: 'NetFriends presence update',
    0x490a88: 'NetFriends add players history thread',
    0x491580: 'NetFriends players met thread',
    0x4a9f78: 'leaderboard poll',
    0x4aa170: 'leaderboard rankings',
    0x4ab45c: 'leaderboard "Score" thread start',
    0x4ab66c: 'leaderboard term',
    0x4ab6d0: 'leaderboard record score',
    0x4ab854: 'leaderboard init (NetInit)',
    0x4b0670: 'LIST_MATCHMAKING_VOTE',
    0x4b0a58: 'PAGE_MULTIPLAYER_MATCHMAKING (matchmaking state machine)',
    0x4b9a88: 'mode label lookup (table 0xb41210)',
    0x4bc310: 'mode label list',
    0x4bc728: 'roster/party query callback (table 0xe15d30)',
    0x4c2bc0: 'party: send message',
    0x4c2e08: 'party: get message',
    0x4c3258: 'party: post message',
    0x4c3a18: 'party: member left',
    0x4c41f8: 'NET_SM_PARTY_ERROR handler',
    0x4cff50: 'Matching2 DeleteServerContext callback',
    0x4d0040: 'Matching2 CreateServerContext callback',
    0x4d0130: 'Matching2 request callback (default opt)',
    0x4d02c8: 'Matching2 GetServerInfo callback',
    0x4d040c: 'room event processing (session event queue drain)',
    0x4d0c88: 'room event callback (queues SESSION events)',
    0x4d0d90: 'Matching2 context callback',
    0x4d0e78: 'Matching2 GetWorldInfoList callback',
    0x4d0fc0: 'g_sessionManager.Init (Matching2 context, callbacks, server/world)',
    0x4d1a00: 'g_sessionManager.Term (Matching2)',
    0x4d1c80: 'room message callback',
    0x4d2394: 'NetSession::IsHost',
    0x4d2ed4: 'NetSession set member data',
    0x4d3848: 'NetSession ping info',
    0x4d3a10: 'NetSession grant owner',
    0x4d3af8: 'NetSession set room data external',
    0x4d3c50: 'NetSession SearchRoom',
    0x4d421c: 'NetSession CreateJoinRoom',
    0x4d4578: 'NetSession JoinRoom',
    0x4d4868: 'NetSession set host rank',
    0x4d4b28: 'NetSession set host migrate',
    0x4d5338: 'NetSession hide/unhide room',
    0x4d57b0: 'NetSession set room data internal',
    0x4d5a18: 'NetSession find member by NpId',
    0x4d5e38: 'NetSession member info (avatar url, NpId)',
    0x4d7200: 'NetSession remove member',
    0x4d76e8: 'NetSession add member',
    0x4d7e48: 'JoinRoom response',
    0x4d8220: 'CreateJoinRoom response',
    0x4d8540: 'NetSession LeaveRoom',
    0x4d8a98: 'NetSession KickoutRoomMember',
    0x4d8bf4: 'LeaveRoom response',
    0x4d987c: 'slot manager remove user',
    0x4d9be8: 'slot manager add/update user',
    0x4da278: 'slot manager room id',
    0x4db1d0: 'UDP sendto',
    0x4db600: 'socket close',
    0x4db630: 'g_udpSocketWorld.Init (bind localhost)',
    0x4db7d4: 'g_udpSocket.Init (P2P, SO_USECRYPTO)',
    0x4dba48: 'UDP recvfrom',
    0x4dbe70: 'bandwidth stats',
    0x4e6cd0: 'voice write',
    0x4e7038: 'voice end',
    0x4e71d8: 'voice init',
    0x4e7560: 'voice read',
    0x4e77e0: 'voice loopback',
    0x4e8afc: 'NpConnectionManager term (signaling ctx)',
    0x4e8c8c: 'NpConnectionManager terminate connection',
    0x4e8f1c: 'NpConnectionManager connection status',
    0x4e9060: 'NpConnectionManager activate connection',
    0x4e9338: 'NpConnectionManager connection info A',
    0x4e9400: 'NpConnectionManager connection info B',
    0x4e9618: 'signaling callback',
    0x4e9d28: 'g_npConnectionManager.Init (signaling ctx)',
    0x4e9e98: 'NpConnectionManager connect by NpId',
    0x4ea438: 'signaling extended handler / connection update',
    0x4eae30: 'NP error code to text',
    0x4ebf7c: 'NetCtl init + start dialog',
    0x4ec068: 'sceNpManagerGetStatus check',
    0x4ec0f0: 'sceNp/sceNp2 term',
    0x4ec120: 'NP communication id select',
    0x4ec184: 'sceNp/sceNp2 init',
    0x4ef8d0: 'twitter thread start',
    0x4f14d0: 'twitter NetCtl check',
    0x4f1950: 'PAGE_TWITTER',
    0x4f1c38: 'BUTTON_TWITTER_*',
}

PS3_PS4_API = {   # PS3 import -> PS4 import that would replace it (only ones the PS4 exe links)
    'sceNpSignalingActivateConnection': 'sceNpSignalingActivateConnection',
    'sceNpSignalingTerminateConnection': 'sceNpSignalingTerminateConnection',
    'sceNpSignalingGetConnectionInfo': 'sceNpSignalingGetConnectionInfo',
    'sceNpScorePollAsync': 'sceNpScorePollAsync',
    'cellNetCtlGetState': 'sceNetCtlGetState',
    'sceNpManagerGetNpId': 'sceNpGetNpId',
    'sceNpTusSetData': 'sceNpTusSetDataAsync', 'sceNpTusGetData': 'sceNpTusGetDataAsync',
    'sceNpTusInit': 'sceNpTusCreateNpTitleCtx', 'sceNpTusCreateTitleCtx': 'sceNpTusCreateNpTitleCtx',
}
# matcher results rejected after checking the PS4 code, and pairs verified by hand
REJECT = {
    0x438bc8: 'PS4 5a0620 shares only message id 0x88a082af with the bootstrap; it is the menu '
              'system-message poller (many message ids, sceNetCtlGetState), not the MP bootstrap',
    0x4bfac8: 'matcher pair 102740 rests on the generic constant 0x800000ff only',
    0x4673c0: 'matcher pair rests on a fuzzy string match only',
}
MANUAL = {
    0x4e8c8c: (0x675200, 'only out-of-line PS4 caller of sceNpSignalingTerminateConnection besides inlined copies; '
                         'neighbours 6752b0/675370/675400 hold the other wrappers'),
    0x4e9060: (0x6752b0, 'only out-of-line PS4 wrapper of sceNpSignalingActivateConnection'),
    0x4e9338: (0x675370, 'PS4 wrapper of sceNpSignalingGetConnectionInfo (first of two)'),
    0x4e9400: (0x675400, 'PS4 wrapper of sceNpSignalingGetConnectionInfo (second of two)'),
    0x4a9f78: (0x64e060, 'only PS4 caller of sceNpScorePollAsync (DeleteRequest where PS3 destroys the ctx)'),
}
SECRETISH = re.compile(r'https?:|://|www\.|\.com|\.net|\.org|\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}|amazonaws|'
                       r'secret|passw|apikey|api_key|token', re.I)


def clean(t):
    """drop strings that could be host names, URLs, credentials or key material"""
    if SECRETISH.search(t):
        return '<omitted>'
    if re.fullmatch(r'[!-~]{12,}', t) and not re.fullmatch(r'[A-Za-z0-9_./%:-]+', t):
        return '<omitted>'
    return t


NET_API = re.compile(r'^(sceNp|sceNp2|cellNetCtl|cellVoice|cellHttp|cellSsl|socket|bind|sendto|recvfrom|'
                     r'setsockopt|socketclose|inet_ntoa|gethostbyname|_sys_net|sys_net_)')


def eh_sizes(elf_path):
    d = open(elf_path, 'rb').read()
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phnum, = struct.unpack_from('<H', d, 0x38)
    ph = [struct.unpack_from('<IIQQQQQQ', d, phoff + 56 * i) for i in range(phnum)]
    hdr = [p for p in ph if p[0] == 0x6474e550][0]
    txt = ph[0]
    delta = txt[2] - txt[3]
    o, va = hdr[2], hdr[3]
    cnt, = struct.unpack_from('<I', d, o + 8)
    out = {}
    for i in range(cnt):
        loc, fde = struct.unpack_from('<ii', d, o + 12 + 8 * i)
        fo = va + fde + delta
        pcb, pcr = struct.unpack_from('<iI', d, fo + 8)
        out[va + fde + 8 + pcb] = pcr
    return out


def load_menu():
    p = os.path.join(REPO, 'work', 'ps3-110', 'menu-callbacks.txt')
    out3, out4 = {}, {}
    if not os.path.exists(p):
        return out3, out4
    for l in open(p):
        if l.startswith('#'):
            continue
        m = re.match(r'(\S+)\s+(\S*)\s+([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]+)\s*\|\s*(.*)$', l.rstrip())
        if not m:
            m = re.match(r'(\S+)\s+([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]+)\s*\|\s*(.*)$', l.rstrip())
            if not m:
                continue
            sid, name, site, cb, reg, p4 = m.group(1), '', m.group(2), m.group(3), m.group(4), m.group(5)
        else:
            sid, name, site, cb, reg, p4 = m.groups()
        p4c = None
        mm = re.match(r'([0-9a-f]+) ([0-9a-f]+) (RET|code)', p4)
        if mm:
            p4c = int(mm.group(2), 16)
        lab = name or ('id ' + sid)
        if cb != '?':
            out3.setdefault(int(cb, 16), []).append(lab)
        if p4c:
            out4.setdefault(p4c, []).append(lab)
    return out3, out4


class NetMap:
    def __init__(self, ps3, img, elf):
        self.mt = FM.load(ps3, img, elf)
        self.an, self.cg = self.mt.an, self.mt.cg
        self.size4 = eh_sizes(elf)
        imps = PI.elf_imports(elf, PI.load_names(PI.DEF_NAMES))
        self.ps4_imports = set(n for o, n, l, nid in imps)
        self.menu3, self.menu4 = load_menu()
        an = self.an
        self.stub_name = {}
        for line in M.imports(an).split('\n') if hasattr(M, 'imports') and False else []:
            pass
        self._imports3()
        self.funcs = []
        for area, lo, hi, mod in AREAS:
            i = bisect.bisect_left(an.starts, lo)
            while i < len(an.starts) and an.starts[i] < hi:
                self.funcs.append((an.starts[i], area, mod))
                i += 1
        self.inscope = set(f for f, a, m in self.funcs)

    def _imports3(self):
        self.stubs3 = {}
        for lib in M.imports(self.an):
            for fn in lib['funcs']:
                if fn['stub']:
                    self.stubs3[fn['stub']] = fn['name']

    # --------------------------------------------------------------- per function
    def calls3(self, f):
        an = self.an
        e = an.func_end(f)
        out = []
        for s, t in an.calls:
            if f <= s < e:
                out.append(t)
        for s, o in an.icalls:
            if f <= s < e and an.opd.get(o):
                out.append(an.opd[o])
        return out

    def imports3(self, f):
        return sorted(set(self.stubs3[t] for t in self.calls3(f) if t in self.stubs3))

    def strings3(self, f, n=3):
        r = M.function_strings(self.an, f, self.an.func_end(f), n)
        return [x.strip('"') for x in r.split(' | ')] if r else []

    def callers3(self, f):
        an = self.an
        return sorted(set(an.func_of(s) for s in an.callers.get(f, [])))

    def match(self, f):
        if f in MANUAL:
            return MANUAL[f][0], 'manual'
        if f in REJECT:
            return None, None
        r = self.mt.m.get(f)
        if r:
            return r[0], r[1]
        if f in self.mt.inl:
            g, c = self.mt.inl[f]
            return g, 'inlined(%.2f)' % c
        return None, None

    def classify(self, f):
        an, cg = self.an, self.cg
        g, how = self.match(f)
        size3 = an.func_end(f) - f
        imps = self.imports3(f)
        missing_api = [x for x in imps if NET_API.match(x) and PS3_PS4_API.get(x) not in self.ps4_imports]
        ev = []
        if g is None:
            # PS4 menu callback by id even when the matcher did not pair it
            if missing_api:
                return None, 'absent', 'api not linked on PS4: ' + ' '.join(missing_api[:4])
            if f in REJECT:
                return None, 'absent', 'matcher pair rejected: ' + REJECT[f]
            sts = [x for x in self.strings3(f, 6) if len(x) >= 8]
            gone = [x for x in sts if x not in self.mt.s4all]
            if sts and len(gone) == len(sts):
                return None, 'absent', 'no PS4 match; its strings are not in the PS4 image'
            return None, 'absent', 'no PS4 match (str/sid/callee/vtable/score passes)'
        if how.startswith('inlined'):
            return g, 'inlined', 'features inside PS4 %x (%s)' % (g, how)
        k = cg.body_kind(g)
        size4 = self.size4.get(g)
        if k.startswith('stub'):
            return g, 'stub ret', '%s, matched by %s' % (k, how)
        dead = g in self.mt.dead
        # callee coverage: PS3 callees with a PS4 match that the PS4 function still calls
        c3 = set(self.calls3(f))
        c4 = set(cg.callees(g))
        exp = [self.mt.m[c][0] for c in c3 if c in self.mt.m]
        kept = [x for x in exp if x in c4]
        cov = (len(kept) / len(exp)) if exp else None
        ratio = (size4 / size3) if size4 and size3 else None
        ev.append('matched by %s' % (MANUAL[f][1] if f in MANUAL else how))
        if size4:
            ev.append('size %x/%x' % (size3, size4))
        if cov is not None:
            ev.append('callees kept %d/%d' % (len(kept), len(exp)))
        if missing_api:
            ev.append('PS3 APIs not linked on PS4: ' + ' '.join(missing_api[:3]))
        if dead:
            ev.append('PS4 dead (no reference)')
        state = 'intact'
        if (ratio is not None and ratio < 0.45 and size3 > 0x40) or (cov is not None and len(exp) >= 2 and cov < 0.6) \
                or missing_api:
            state = 'partial'
        if dead:
            state += ' (dead)'
        return g, state, '; '.join(ev)

    def role(self, f):
        if f in ROLES:
            return ROLES[f]
        if f in self.menu3:
            return 'menu callback ' + '/'.join(self.menu3[f][:3])
        imps = [x for x in self.imports3(f) if NET_API.match(x)]
        sts = self.strings3(f, 2)
        if sts:
            t = clean(sts[0][:60].replace('\t', ' ').replace('\n', ' ').replace('|', '/'))
            return 'str "%s"' % t
        if imps:
            return 'calls ' + ' '.join(imps[:3])
        cs = [c for c in self.callers3(f) if c in ROLES]
        if cs:
            return 'helper of %x (%s)' % (cs[0], ROLES[cs[0]][:40])
        return 'helper'

    def note(self, f, state, g):
        imps = [x for x in self.imports3(f) if NET_API.match(x)]
        if state == 'stub ret':
            return 'reimplement from PS3; PS4 keeps only the slot'
        if state.startswith('absent'):
            if imps:
                return 'reimplement on PC transport (replaces ' + ', '.join(imps[:3]) + ')'
            return 'port from PS3 code'
        if state.startswith('partial'):
            return 'compare with PS3; restore stripped calls'
        if state == 'inlined':
            return 'logic lives in PS4 %x' % g
        return 'reuse PS4 code'

    def rows(self):
        an = self.an
        out = []
        for f, area, mod in self.funcs:
            g, state, ev = self.classify(f)
            cs = [c for c in self.callers3(f)]
            ins = [c for c in cs if c in self.inscope]
            callees = sorted(set(c for c in self.calls3(f) if c in self.inscope))
            opd = an.opd_of.get(f, [])
            slots = [s for o in opd for s in an.ptrs_to(o)][:3]
            ev = clean_ev(ev)
            out.append({
                'area': area, 'module': mod, 'role': self.role(f), 'ps3': f,
                'size3': an.func_end(f) - f, 'ps4': g, 'size4': self.size4.get(g) if g else None,
                'state': state, 'evidence': ev, 'note': self.note(f, state, g),
                'callers': ins[:6], 'ncallers': len(cs), 'callees': callees[:8],
                'imports': [x for x in self.imports3(f) if NET_API.match(x)],
                'slots': slots, 'menu': self.menu3.get(f, []),
            })
        return out


def clean_ev(ev):
    def rep(m):
        return m.group(1) + clean(m.group(2))
    ev = re.sub(r'(str(?:fuzzy|set)?:)([^;]*)', rep, ev)
    return ev.replace('\n', ' ').replace('\t', ' ').replace('|', '/')


def fmt_row(r):
    return [r['role'], '%x' % r['ps3'], ('%x' % r['ps4']) if r['ps4'] else 'absent', r['state'], r['evidence'],
            r['note'], r['area'], r['module'], '%x' % r['size3'], ('%x' % r['size4']) if r['size4'] else '',
            ' '.join('%x' % c for c in r['callers']), str(r['ncallers']),
            ' '.join('%x' % c for c in r['callees']), ' '.join(r['imports'][:6]),
            ' '.join('%x' % s for s in r['slots'])]


COLS = ['role', 'ps3_110', 'ps4_102', 'ps4_state', 'evidence', 'porting_note', 'area', 'module',
        'ps3_size', 'ps4_size', 'mp_callers', 'n_callers', 'mp_callees', 'ps3_net_imports', 'ps3_data_slots']


def netsm(nm):
    an = nm.an
    e = an.elf
    ss = {}
    for s, v, k in an.refs:
        if k == 'a' and 0xb00000 < v < 0xd60000:
            t = e.cstr(v, 90)
            if t and 'NET_SM_' in t:
                ss[s] = t
    out = []
    sites = [(cs, 3) for cs in an.callers.get(SET_NET_STATE, [])]
    for s, k in an.byval.get(NET_STATE, []):
        if k == 'm' and e.u32(s) >> 26 == 36:
            sites.append((s, (e.u32(s) >> 21) & 31))
    for cs, reg in sorted(sites):
        a, val = cs - 4, None
        f = an.func_of(cs)
        while a > max(f, cs - 200) - 4:
            w = e.u32(a)
            if w >> 26 == 14 and ((w >> 16) & 31) == 0 and ((w >> 21) & 31) == reg:
                val = P.sx16(w & 0xffff)
                break
            if w >> 26 == 18 and a != cs:
                break
            a -= 4
        near = sorted((abs(x - cs), ss[x]) for x in ss if an.func_of(x) == f and abs(x - cs) < 0x80)
        out.append((val, cs, f, near[0][1] if near else ''))
    return out


AREA_TITLES = [
    ('frontend', 'Frontend: menu callbacks, page handlers, NET_SM transitions, lobby UI'),
    ('bootstrap', 'Bootstrap, NetInit, NP init/term, voice, teardown'),
    ('session', 'Session, room, party, friends, signaling'),
    ('transport', 'Transport: sockets, connection, reliability, per-frame update'),
]


def md_table(rows):
    out = ['| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |', '|---|---|---|---|---|---|']
    for r in rows:
        f = fmt_row(r)
        out.append('| %s | `%s` | %s | %s | %s | %s |' % (
            f[0].replace('|', '/'), f[1], ('`%s`' % f[2]) if f[2] != 'absent' else 'absent', f[3],
            f[4].replace('|', '/'), f[5]))
    return out


def menu_rows(nm):
    p = os.path.join(REPO, 'work', 'ps3-110', 'menu-callbacks.txt')
    names = {}
    np_ = os.path.join(REPO, 'work', 'ps3-110', 'names.txt')
    if os.path.exists(np_):
        for n in open(np_).read().split():
            names[M.sid(n)] = n
    names.update({M.sid(n): n for n in EXTRA_CONTROL_NAMES})
    out = []
    for l in open(p):
        if l.startswith('#'):
            continue
        t = l.split('|')
        left, right = t[0].split(), t[1].strip()
        if len(left) < 4 or not (0x4320 <= int(left[-3], 16) >> 8 <= 0x4f10):
            continue
        sid = left[0]
        cb = left[-2]
        nm4 = right.split(';')[0].split()
        out.append((sid, names.get(int(sid, 16), '') if sid != '?' else '', left[-3], cb,
                    nm4[1] if len(nm4) > 1 else '-', nm4[2] if len(nm4) > 2 else '-'))
    return out


EXTRA_CONTROL_NAMES = ['BUTTON_MULTIPLAYER_LEAVE_GAME', 'BUTTON_MULTIPLAYER_JOIN', 'BUTTON_MULTIPLAYER_START',
                       'BUTTON_MULTIPLAYER_LEAVEGAME']


def write_md(nm, path):
    rows = nm.rows()
    L = []
    c = collections.Counter(r['state'].split(' (')[0] for r in rows)
    L.append('Generated by `u2mp/tools/u2_netmap.py` (%d PS3 functions in scope): %s.' % (
        len(rows), ', '.join('%s %d' % kv for kv in sorted(c.items()))))
    L.append('')
    L.append('### Menu controls and pages (registration table, PS3 `0x43918c` / PS4 `0x5ed8f0`)')
    L.append('')
    L.append('| control id | name | PS3 site | PS3 callback | PS4 callback | PS4 state |')
    L.append('|---|---|---|---|---|---|')
    for sid, name, site, cb, c4, st in menu_rows(nm):
        st = {'RET': 'stub ret', 'code': 'code'}.get(st, st)
        L.append('| `%s` | %s | `%s` | %s | %s | %s |' % (sid, name or '(name unknown)', site,
                 ('`%s`' % cb) if cb != '?' else 'shared (register)', ('`%s`' % c4) if c4 != '-' else '-', st))
    L.append('')
    L.append('### NET_SM states (values written by SetNetState `0x4383cc` or stored to `0xf11764`)')
    L.append('')
    L.append('| value | name (GOTO log) | written in (PS3 1.10) |')
    L.append('|---|---|---|')
    by = collections.OrderedDict()
    for val, cs, f, sname in sorted(netsm(nm), key=lambda x: (x[0] is None, x[0] or 0, x[1])):
        k = (val, re.sub(r'^.*?(NET_SM_[A-Z_]+).*$', r'\1', sname).strip() if 'NET_SM_' in sname and '?' not in sname else sname)
        by.setdefault(k, []).append('%x@%x' % (f, cs))
    for (val, sname), sites in by.items():
        L.append('| %s | %s | %s |' % (val if val is not None else '(computed)', sname or '-', ' '.join('`%s`' % x for x in sites)))
    for area, title in AREA_TITLES:
        L.append('')
        L.append('### ' + title)
        L.append('')
        mods = []
        for r in rows:
            if r['area'] == area and r['module'] not in mods:
                mods.append(r['module'])
        for mod in mods:
            L.append('**%s**' % mod)
            L.append('')
            L += md_table([r for r in rows if r['area'] == area and r['module'] == mod])
            L.append('')
    open(path, 'w', newline='\n').write('\n'.join(L) + '\n')
    return rows


def main(argv):
    if len(argv) < 5:
        print(__doc__)
        return 1
    nm = NetMap(*argv[1:4])
    cmd = argv[4]
    if cmd == 'tsv':
        rows = nm.rows()
        with open(argv[5], 'w', newline='\n') as fo:
            fo.write('\t'.join(COLS) + '\n')
            for r in rows:
                fo.write('\t'.join(x.replace('\t', ' ') for x in fmt_row(r)) + '\n')
        c = collections.Counter((r['area'], r['state'].split(' (')[0]) for r in rows)
        for k in sorted(c):
            print(k, c[k])
    elif cmd == 'md':
        write_md(nm, argv[5])
    elif cmd == 'show':
        rows = {r['ps3']: r for r in nm.rows()}
        for h in argv[5:]:
            f = nm.an.func_of(int(h, 16))
            r = rows.get(f)
            print(fmt_row(r) if r else '%x not in scope; match %s' % (f, nm.match(f)))
    elif cmd == 'netsm':
        for val, cs, f, s in netsm(nm):
            print('%s\t%x\t%x\t%s' % (val, cs, f, s))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
