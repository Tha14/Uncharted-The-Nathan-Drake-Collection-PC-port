"""u2_match_report.py - function map of Uncharted 2 multiplayer MATCH, GAMEPLAY and PROGRESSION
code, PS3 1.10 against PS4 1.02. Writes u2mp/docs/functions-match-gameplay.tsv (every PS3
function in the in-scope ranges) and prints the per-area summary used by
FUNCTIONS-MATCH-GAMEPLAY.md. Read-only; writes addresses, names, ids and short strings only.

Usage:
  python u2_match_report.py <PS3 EBOOT.elf> <PS4 boot-linked.bin> <PS4 eboot.elf> tsv <out.tsv>
  python u2_match_report.py ... rows <area>      print the rows of one area (md-ready)
  python u2_match_report.py ... counts           states per area
  python u2_match_report.py ... events           NetEvent type table (md-ready)
  python u2_match_report.py ... all <out.tsv> <dir>   tsv + counts + md fragments in one run

Needs (build first):
  u2_func_match.py ... build     -> work/match/match.pickle
  u2_netevents.py ...            -> work/match/netevent-slots.tsv
Sources of each row:
  * ROLES below: hand-checked roles (address, role, notes, optional manual PS4 pairing with the
    evidence written next to it). Manual pairings were checked in the disassembly.
  * every other PS3 function inside AREAS gets an automatic role: NetEvent method (type/slot
    from the factory tables), the event's sender (a function that stores that event's vtable),
    a script native name, a "Class::Method" or first log text it prints, else '-'.
PS4 state:
  intact-alive  PS4 counterpart has code and is referenced (call/jmp/lea, or a data slot)
  intact-dead   PS4 counterpart has code but nothing alive references it
  stub          PS4 counterpart is a bare ret / ret 0 / ret const
  partial       PS4 counterpart exists but lost a part: a NetEvent whose Write/Read are stubs,
                or the PS4 body is under 25 % of the PS3 size (PS3 > 0x200 bytes)
  inlined       no own PS4 function; the code sits in the PS4 function named in the PS4 column
                (state of that host in the evidence)
  absent        no PS4 counterpart: its source file or its identifying (non-log) strings are
                gone from the PS4 image, or the PS4 executable does not import the API it needs
  unresolved    not paired: the PS4 image still has its source file / identifying strings, so
                it probably exists but the static matcher could not prove which function it is
                (log-only functions: the PS4 build compiles all log text out)
"""
import collections, csv, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import u2_func_match as U

REPO = U.REPO

# (start, end, file label, area)  -- PS3 1.10 address ranges; areas: match players modes weapons
# progression natives
AREAS = [
    (0x0a02dc, 0x0a0aa8, 'hiddentreasure.cpp', 'modes'),
    (0x0c2030, 0x0c2500, 'medal manager', 'progression'),
    (0x0cfd94, 0x0cffa0, 'moneymanager.cpp', 'progression'),
    (0x1731a0, 0x173c90, 'trophy manager', 'progression'),
    (0x173f60, 0x175400, 'unlockablemanager.cpp', 'progression'),
    (0x2814e0, 0x281e10, 'net-skill.cpp', 'modes'),
    (0x433b98, 0x435d00, 'cinema.cpp', 'progression'),
    (0x45212c, 0x4552b8, 'net-anim-command-generator.cpp', 'players'),
    (0x4552b8, 0x455a90, 'net boosters', 'weapons'),
    (0x455a90, 0x457f30, 'net-character-animate.cpp', 'players'),
    (0x457f30, 0x458b00, 'net-character-death.cpp', 'players'),
    (0x458b00, 0x459458, 'net-character-fire-animation.cpp', 'players'),
    (0x459458, 0x459b64, 'net-character-reaction.cpp', 'players'),
    (0x459b64, 0x4631d8, 'net-character.cpp', 'players'),
    (0x4631d8, 0x464464, 'cinema file store', 'progression'),
    (0x468c10, 0x48f16c, 'net-event.cpp', 'players'),
    (0x492ba8, 0x4a9e94, 'net-game-manager.cpp', 'match'),
    (0x4a9e94, 0x4aba88, 'leaderboards (sceNpScore)', 'progression'),
    (0x4b4244, 0x4b5d80, 'debug Net menu', 'match'),
    (0x4c4aa8, 0x4c8188, 'net-player-data.cpp', 'progression'),
    (0x4c8188, 0x4cc8e4, 'net-player-tracker.cpp', 'players'),
    (0x4cc8e4, 0x4cff50, 'net script functions', 'natives'),
    (0x4d93e8, 0x4d987c, 'net-simple-snapshot.cpp', 'players'),
    (0x4da608, 0x4db1d0, 'net-snapshot.cpp', 'players'),
    (0x4dce9c, 0x4e43b8, 'net stat game (money, skill)', 'progression'),
    (0x4e43b8, 0x4e6cd0, 'net-territory.cpp', 'modes'),
    (0x69a9c0, 0x69ae60, 'player-weapon-gets.cpp', 'weapons'),
    (0x6c0240, 0x6c04d8, 'playerstatmanager.cpp', 'progression'),
    (0x89d5e0, 0x89dda0, 'weapon-pickup.cpp', 'weapons'),
]
EXTRA_NATIVES = r'^D[Cc](Net|.*WeaponSet|.*Booster|WaitUntilNetTime|AlivePlayerCount|ActivePlayerCount|PauseCoop|GetCaptureZone|SetForceTension|UpdateFightRef)'
MODE_RX = re.compile(r'Territor|Flag|Juggernaut|[Cc]oop|Wave|Treasure|Revive|Capture|KnockedDown|EndWave|KillEntity|NpcFight|SniperLaser|Griefer')
WEAPON_RX = re.compile(r'Pickup|PickUp|Grenade|Rocket|Projectile|Booster|Weapon|Turret')
PROG_RX = re.compile(r'Medal|NetStat|Money|money|Trophy|Score')
ABSENT_FILES = {'cinema.cpp', 'flow.cpp', 'http.cpp', 'net-matchmaking.cpp', 'net-custom-games.cpp',
                'net-session.cpp', 'net-session-manager.cpp', 'net-connection.cpp', 'net-livedata.cpp'}

# hand-checked roles: (area, ps3, role, notes, ps4 override or None, override evidence)
ROLES = [
    # ---- 1. NetGameManager / match flow
    ('match', 0x494ea8, 'NetGameManager::IsHost (this = g_netGameManager 0xf1dc80)', 'tiny getter; PS4 inlines it everywhere', None, ''),
    ('match', 0x4a9410, 'NetGameManager::Update (per frame; "net-task: %s"; calls ClientUpdate, HostUpdate)', 'callers: TaskManagerOnlineUpdate 0x442558, replay 0x436220. PS4 merged Update+ClientUpdate+HostUpdate+game/round-over checks into one function', 0x63e6a0, 'manual: shares consts dec97a4/2743c4b0/9f2aeafb/d3b86d60, callees 63e1b0 (SpawnNetFlag) and a572d0; PS4 callers 5e5c80/5f14d0 (online/replay task update)'),
    ('match', 0x494930, 'NetGameManager::ClientUpdate (pickup counters, tracker visibility)', 'inlined on PS4', ('inl', 0x63e6a0), 'manual: its callee UpdatePlayerTrackerVisibility (sid c032f430) sits in 63e6a0'),
    ('match', 0x494608, 'NetGameManager::UpdatePlayerTrackerVisibility', 'inlined on PS4', ('inl', 0x63e6a0), 'unique sid c032f430 is inside 63e6a0'),
    ('match', 0x494c38, 'NetGameManager::KickVisibilityJob', '', None, ''),
    ('match', 0x4a8af8, 'NetGameManager::HostUpdate (host-only rules: game/round over, flags, pickups, net tasks)', 'inlined on PS4', ('inl', 0x63e6a0), 'manual: 63e6a0 calls 63e1b0 (= SpawnNetFlag 0x4a03f8) and 611160 (= 0x48d628) which only HostUpdate calls on PS3'),
    ('match', 0x4a49e8, 'CheckGameOver: time limit, score limit, round limit, last team standing; returns end reason (6 = time)', 'reads net-games record (*net-games* sid 3b9a067d, 0x80 stride) and NGM fields; see field map', ('inl', 0x63e6a0), 'manual: 63e6a0 @63f31d..63f3e5 = same sequence (net-games lookup, now-start >= 1500, record+0xc != 10, time-limit float * const, cvttss2si, reason 6)'),
    ('match', 0x49f114, 'CheckRoundOver: round time expired / only one team alive', 'no own PS4 function found', ('inl', 0x63e6a0), 'probable: HostUpdate is merged into 63e6a0; log-only on PS3'),
    ('match', 0x4a85a0, 'StartNetGame: picks net game, sets m_startTime, starts Play Task, spawns players', 'callers: NET_SM_UPDATE 0x444660 (flow), cinema 0x4348f0', 0x647a50, 'manual: same callees 49ed20->6471a0 and 49fc70->646bf0, "Net Menu"/"Play Task" strings; 0x49eb80 is inlined into it'),
    ('match', 0x49eb80, 'StartNetGame helper (net-games record setup, sid ad66fbf6)', 'inlined into StartNetGame on PS4', ('inl', 0x647a50), 'unique sid ad66fbf6 sits in 647a50'),
    ('match', 0x4a46b0, 'Net Menu "Play Task" (start from debug/net menu)', '', None, ''),
    ('match', 0x4a1be8, 'NetGameManager::AssignTeams (small variant)', '', 0x640fc0, 'manual: both PS3 and PS4 have two functions with the "AssignTeams - num teams is zero" assert; 0x4a2490 pairs with 6400d0 by "pTrackers[0]"'),
    ('match', 0x4a2490, 'NetGameManager::AssignTeams / team balancing (teams, models, spawners)', 'sends NetEventAssignTeam', None, ''),
    ('match', 0x495cf0, 'NetGameManager team accessor (assert team < kMaxNetTeams)', '', None, ''),
    ('match', 0x4a6008, 'spawn/respawn players every frame (spawner choice incl. coop spawners)', 'no static PS3 caller (reached through a pointer)', 0x642d20, 'manual: same consts 09b2fb53/20bea032/33364361/3b9a067d/5bfae217/d50bc34b, both call the coop-spawner helper 0x49f618 = 642b60; PS4 caller Update 63e6a0'),
    ('match', 0x493d30, 'respawn helper (sid 332fb3f6)', 'inlined on PS4', ('inl', 0x642d20), 'unique sid 332fb3f6 sits in 642d20'),
    ('match', 0x4a1eb0, 'initial player spawn at game start', 'no static PS3 caller', 0x644220, 'manual: consts 09b2fb53/33364361/3b9a067d/5bfae217/d50bc34b, calls 642b60 (coop spawner); PS4 caller StartNetGame 647a50'),
    ('match', 0x4caa28, 'per-slot host update (called 10x by HostUpdate)', '', 0x6640a0, 'manual: PS4 Update 63e6a0 calls 6640a0 ten times'),
    ('players', 0x4c9088, 'per-slot tracker update (called 10x by Update)', '', 0x6658d0, 'manual: PS4 Update 63e6a0 calls 6658d0 ten times'),
    ('players', 0x4cac50, 'NetPlayerTracker per-frame update (create/update remote characters)', '', 0x664bc0, 'probable: PS4 callee of Update that calls 63ffb0 (= 0x492a20) and the tracker spawn helpers 6636b0 (= 0x4c9880) / 663280 (= 0x4c9b58)'),
    ('match', 0x49bb00, 'NetGameManager::GetRandomNetGameIndex (map/mode pick from *net-games*)', 'log-only on PS3', None, ''),
    ('match', 0x4942ac, 'NetGameManager::ClearNetPlayers', '', None, ''),
    ('match', 0x495790, 'NetGameManager::ClearStatsNetPlayers', '', None, ''),
    ('match', 0x493ca8, 'NetGameManager::SendNetEventToHost', '', None, ''),
    ('modes', 0x4a03f8, 'NetGameManager::SpawnNetFlag (CTF flags; Plunder treasure carrier)', '', None, ''),
    ('modes', 0x4a00c0, 'NetGameManager::SpawnNetTerritory (Turf War / King of the Hill zones)', '', None, ''),
    ('modes', 0x49d880, 'NetGameManager::ShuffleNetTerritorySpawners', '', None, ''),
    ('modes', 0x49f618, 'coop: spawn players at coop spawners', '', None, ''),
    ('modes', 0x4a0d88, 'coop: map player -> coop id / spawner index', '', None, ''),
    ('modes', 0x4976e0, 'coop arena: wave complete (duration, bonus)', '', None, ''),
    ('match', 0x4a12e8, 'NPC art-group / anim set for net NPCs (anim-npc-multiplayer)', '', None, ''),
    ('match', 0x498b28, 'NetGameManager::AddNpcTracker', '', None, ''),
    ('match', 0x4983c8, 'NetGameManager::AddPickup (net pickup table)', '', None, ''),
    ('match', 0x499fe0, 'NetGameManager::RequestPickUp', '', None, ''),
    ('match', 0x49a12c, 'NetGameManager::CanPickUp (host arbitration)', '', None, ''),
    ('match', 0x49a608, 'NetGameManager::AbortPickUp', '', None, ''),
    ('match', 0x4b4244, 'debug Net menu (Force Rank, Disable Min Player Limit, give medals...)', '', None, ''),
    ('modes', 0x4e43b8, 'NetTerritory object (Turf War / King of the Hill zone)', '', None, ''),
    ('modes', 0x4e450c, 'NetTerritory owned state ("Owned")', '', None, ''),
    ('modes', 0x4e45c8, 'NetTerritory update', '', None, ''),
    ('modes', 0x47b468, 'NetEventTerritoryOwn::Apply (zone captured)', 'Write/Read of the event are stubs on PS4', None, ''),
    ('modes', 0x47af00, 'NetEventTerritorySpawn::Apply (zone moves)', 'Write/Read stubs on PS4', None, ''),
    ('modes', 0x475d38, 'NetEventTerritorySetTimer::Apply', 'Write/Read stubs on PS4', None, ''),
    ('modes', 0x47b3fc, 'NetEventTerritoryScore::Apply', '', None, ''),
    ('modes', 0x476cd4, 'NetEventCaptureFlag::Apply (Plunder/CTF capture)', '', None, ''),
    ('modes', 0x477f40, 'NetEventSetJuggernaut::Apply', '', None, ''),
    ('modes', 0x47ae08, 'NetEventSpawnHiddenTreasure::Apply', 'Write/Read stubs on PS4', None, ''),
    ('modes', 0x47780c, 'NetEventCoopTeamFailed::Apply (co-op fail)', '', None, ''),
    ('modes', 0x476338, 'NetEventEndWave::Apply (arena wave end)', '', None, ''),
    ('modes', 0x475ec8, 'NetEventRevivePlayer::Apply (co-op revive)', '', None, ''),
    ('modes', 0x0a0790, 'HiddenTreasure::Init (treasure pickups)', '', None, ''),
    ('modes', 0x2814e0, 'NetSkill (net NPC skill, co-op AI)', '', None, ''),
    # ---- 2. players
    ('players', 0x4c8568, 'NetPlayerTracker spawn at spawner ("Missing spawner")', '', None, ''),
    ('players', 0x4c9880, 'spawn remote NetCharacter (hero-default, factory 0x7069c8, sid f2c86c22)', '', None, ''),
    ('players', 0x4c9b58, 'NetPlayerTracker set model ("Model %s")', '', None, ''),
    ('players', 0x46055c, 'NetCharacter::Init (health, anim layers)', '', None, ''),
    ('players', 0x45dad0, 'NetCharacter attach/props (gun_holster_mp_male...)', '', None, ''),
    ('players', 0x4da608, 'NetSnapshot parse (anim commands)', '', None, ''),
    ('players', 0x4db060, 'NetSnapshot anim art-group check', '', None, ''),
    ('players', 0x4d93e8, 'NetSimpleSnapshot interpolation', '', None, ''),
    ('players', 0x480698, 'FillAnimCmdBitStream (snapshot animation stream writer)', '', None, ''),
    ('players', 0x47e550, 'CreateNetEventByType (factory, 80 types)', 'factory and type table identical on PS4', 0x6180f0, 'manual: same switch over 0..79 (cmp 0x4f) storing one vtable per type; "CreateNetEventByType" assert'),
    ('players', 0x468ed8, 'net event debug dump (kNpNetEvent* names table 0xd70c20)', '', None, ''),
    # ---- 4. weapons
    ('weapons', 0x455900, 'NetBoosterManager::GetAmount (boosted scale)', '', None, ''),
    ('weapons', 0x4552b8, 'NetBoosterManager: booster id lookup ("Invalid booster id")', '', None, ''),
    ('weapons', 0x4555d0, 'Net Boosters debug menu', '', None, ''),
    ('weapons', 0x6617c0, 'DcPlayerSaveWeaponSet (player-save-weapon-set)', 'loadout save', None, ''),
    ('weapons', 0x661cec, 'DcWeaponSetReset (weapon-set-reset)', '', None, ''),
    ('weapons', 0x669a88, 'DcGivePlayerWeapon', '', None, ''),
    ('weapons', 0x89d5e0, 'WeaponPickup draw control / pickup object', '', None, ''),
    # ---- 5. progression
    ('progression', 0x4dce9c, 'NetStatGame::CaptureTallies', 'log-only', None, ''),
    ('progression', 0x4dcf88, 'NetStatGame::AddMoneyToTally (cash per event, multiplier)', 'log-only', None, ''),
    ('progression', 0x4dfc08, 'NetStatGame::Stop (total money of the match)', 'log-only', None, ''),
    ('progression', 0x4e1b24, 'NetStatGame::RestoreCapturedTallies', 'log-only', None, ''),
    ('progression', 0x4df898, 'skill rating update ("Player %s skill %f ->")', '', None, ''),
    ('progression', 0x4e2630, 'NPC kill points (coop/arena score)', '', None, ''),
    ('progression', 0x4df644, 'NetStatGame::AddCoopTreasurePickedUp', '', None, ''),
    ('progression', 0x0cfd94, 'MoneyManager: NPC points from net-npc-info', '', None, ''),
    ('progression', 0x0c2030, 'MedalManager: award medal', '', None, ''),
    ('progression', 0x4c6298, 'NetPlayerData::Publish (Save Game Stats thread, TUS)', 'PS4 TUS code lives in d5bb28 (collection-wide)', None, ''),
    ('progression', 0x4c65f8, 'NetPlayerData::FetchTusData', '', None, ''),
    ('progression', 0x4c699c, 'NetPlayerData::FetchHddData', '', None, ''),
    ('progression', 0x4c6ab8, 'NetPlayerData::RunConnectThread', '', None, ''),
    ('progression', 0x4c4cd8, 'NetPlayerData::Data::Decrypt', '', None, ''),
    ('progression', 0x4c4df0, 'NetPlayerData::Data::Encrypt', '', None, ''),
    ('progression', 0x4c5268, 'NetPlayerData trophy stat counter (kNpdTrophyStatCount*)', '', None, ''),
    ('progression', 0x4ab854, 'leaderboards init (sceNpScoreInit, title ctx)', 'sceNpScoreInit/CreateTitleCtx are not imported on PS4 (it imports only sceNpScorePollAsync/DeleteRequest)', None, ''),
    ('progression', 0x4a9f78, 'leaderboards poll/destroy transaction', 'PS4 still imports sceNpScorePollAsync/DeleteRequest (one caller near 64dae0)', None, ''),
    ('progression', 0x4ab6d0, 'leaderboards record score (sceNpScoreRecordScore)', 'sceNpScoreRecordScore is not imported by the PS4 executable', None, ''),
    ('progression', 0x4aa170, 'leaderboards get rankings', 'sceNpScoreGetRanking* not imported on PS4', None, ''),
    ('progression', 0x1735b8, 'TrophyManager thread (create context, register, unlock)', 'PS4 has its own trophy manager at b894a8 (new SDK API)', None, ''),
    ('progression', 0x174a68, 'UnlockableManager skin swap', '', None, ''),
    ('progression', 0x175090, 'UnlockableManager weapon swap', '', None, ''),
    ('progression', 0x4348f0, 'cinema: start replay (NET_SM_REPLAY, multi-player-cinema)', 'cinema.cpp is gone on PS4', None, ''),
    ('progression', 0x435be8, 'cinema: replay state machine', '', None, ''),
    ('progression', 0x433b98, 'TaskManagerReplay (vtable)', '', None, ''),
    ('progression', 0x464334, 'cinema file paths (cinema%d/...)', '', None, ''),
]


def area_of(f):
    for a, b, fl, ar in AREAS:
        if a <= f < b:
            return fl, ar
    return None, None


def main(argv):
    if len(argv) < 5:
        print(__doc__)
        return 1
    ps3, img, elf, cmd = argv[1:5]
    args = argv[5:]
    mt = U.load(ps3, img, elf)
    an, cg = mt.an, mt.cg
    cg.starts_set = set(cg.starts)
    s3, k3, c3 = U.ps3_features(an)
    s4all = mt.s4all
    # PS4 strings incl. unreferenced ones (asserts of removed code would be gone entirely)
    img_bytes = cg.d
    # netevents
    ev = list(csv.DictReader(open(os.path.join(U.OUT, 'netevent-slots.tsv')), delimiter='\t'))
    evrole = {}
    evtype = collections.defaultdict(dict)
    SLOT = {'0': 'dtor', '1': 'deleting dtor', '2': 'Write', '3': 'Read', '4': 'Apply', '5': 'base method', '6': 'size (ret 0)'}
    for r in ev:
        evtype[int(r['type'])][int(r['slot'])] = r
        if r['ps3'] and r['slot'] in ('2', '3', '4'):
            evrole.setdefault(int(r['ps3'], 16), (r, '%s::%s (type %s, slot %s)' % (r['name'], SLOT[r['slot']], r['type'], r['slot'])))
    # vtable -> type, for senders
    import u2_netevents as N
    r3, i3 = U.vtables3(an)
    t3 = N.ps3_types(an, r3)
    names = N.type_names(an)
    vt2type = {v[1]: t for t, v in t3.items()}
    sender = {}
    for v, t in vt2type.items():
        for s, k in an.byval.get(v, []):
            f = an.func_of(s)
            if f != N.PS3_FACTORY:
                sender.setdefault(f, set()).add(names.get(t, str(t)))
    # PS4 senders: functions other than the factory that load the PS4 vtable of the type
    r4, i4 = U.vtables4(cg)
    t4 = N.ps4_types(cg, r4)
    sender4 = collections.defaultdict(set)
    for t, (tgt, v, k) in t4.items():
        for kind, s in cg.callers.get(v, []):
            g = cg.func_of(s)
            if g != N.PS4_FACTORY:
                sender4[names.get(t, str(t))].add(g)
    sender3 = collections.defaultdict(set)
    for f, ns in sender.items():
        for n in ns:
            sender3[n].add(f)
    evsend = {}
    for n, fs3 in sender3.items():
        fs4 = sender4.get(n, set())
        if len(fs3) == 1 and len(fs4) == 1:
            evsend[next(iter(fs3))] = (next(iter(fs4)), 'evsender: only function storing the %s vtable on each side' % n)
    stripped_types = set()
    for t, sl in evtype.items():
        if any(sl.get(k, {}).get('ps4_state', '').startswith('stub') and sl[k]['ps3'] and
               (an.func_end(int(sl[k]['ps3'], 16)) - int(sl[k]['ps3'], 16)) > 0x30 for k in (2, 3)):
            stripped_types.add(names.get(t, str(t)))
    # natives
    nat = {}
    for c, d, f in mt.nat3:
        nat.setdefault(f, (c, d))
    n4 = {}
    for c, d, f in mt.nat4:
        n4.setdefault(c, f)
    roles = {r[1]: r for r in ROLES}
    # universe
    fs = set()
    for a, b, fl, ar in AREAS:
        fs.update(f for f in an.starts if a <= f < b)
    for f, (c, d) in nat.items():
        if re.match(EXTRA_NATIVES, c):
            fs.add(f)
    fs.update(roles)
    callers3 = collections.defaultdict(set)
    for f, lst in c3.items():
        for t in lst:
            callers3[t].add(f)
    rows = []
    for f in sorted(fs):
        fl, ar = area_of(f)
        size = an.func_end(f) - f
        role, notes, ev_ = '-', '', ''
        g, how, state = None, '', ''
        if f in nat:
            role = 'script native %s (%s)' % (nat[f][0], nat[f][1])
            ar = 'natives' if 'Net' in nat[f][0] or ar is None else ar
            fl = fl or 'script natives'
        if f in evrole:
            role = evrole[f][1]
        elif f in sender:
            role = 'build/send %s' % ','.join(sorted(sender[f]))
        if role == '-':
            ss = sorted(s3.get(f, ()), key=len)
            cm = [m.group(0) for s in ss for m in re.finditer(r'\b[A-Z]\w+::\w+', s)]
            logs = [s.strip() for s in ss if not s.endswith('.cpp') and len(s.strip()) >= 6]
            if cm:
                role = cm[0]
            elif logs:
                role = 'prints "%s"' % logs[0][:50].replace('\t', ' ').replace('\n', ' ')
        if f in roles:
            r = roles[f]
            ar, role, notes = r[0], r[2], r[3]
        txt = role + ' ' + ' '.join(s3.get(f, ()))
        if ar in ('match', 'players') and MODE_RX.search(txt) and f not in roles:
            ar = 'modes'
        elif ar in ('match', 'players') and WEAPON_RX.search(role) and f not in roles:
            ar = 'weapons'
        elif ar in ('match', 'players') and PROG_RX.search(role) and f not in roles:
            ar = 'progression'
        # PS4 pairing
        ov = roles.get(f)
        if ov and ov[4]:
            if isinstance(ov[4], tuple):
                g, how = ov[4][1], 'inlined (%s)' % ov[5]
                state = 'inlined'
            else:
                g, how = ov[4], ov[5]
        elif f in mt.m:
            g, how = mt.m[f]
        elif f in evsend:
            g, how = evsend[f]
        elif f in mt.inl:
            g, c = mt.inl[f]
            how = 'inlined (features %.0f%% inside)' % (100 * c)
            state = 'inlined'
        else:
            r = None
            if f in evrole and evrole[f][0]['ps4']:
                r = evrole[f][0]
                g, how = int(r['ps4'], 16), 'evtable: same type/slot in the factory vtables'
        if g is not None and not state:
            st = U.ps4_state(mt, g)
            if st.startswith('stub'):
                state = 'stub'
                how += '; PS4 ' + st
                if size <= 0x40:
                    how += ' (PS3 body is only 0x%x bytes too)' % size
            elif st == 'dead':
                state = 'intact-dead'
            else:
                state = 'intact-alive'
                s4 = cg.func_end(g) - g
                if size > 0x200 and s4 < 0.25 * size:
                    state = 'partial'
                    how += '; PS4 size %x vs PS3 %x' % (s4, size)
            if f in evrole and evrole[f][1].split('::')[0] in stripped_types and evrole[f][0]['slot'] == '4':
                if state == 'intact-alive':
                    state = 'partial'
                how += '; Write/Read of this event are stubs on PS4'
        elif state == 'inlined':
            how += '; host %s' % U.ps4_state(mt, g)
        if g is None:
            ids = [s for s in s3.get(f, ()) if '%' not in s and '\n' not in s and len(s) >= 6
                   and not re.search(r'\.(cpp|h|inl)$', s)]
            if fl in ABSENT_FILES:
                state, how = 'absent', 'source file %s has no trace in the PS4 image' % fl
            elif ids and not any(s.encode('latin1') in img_bytes for s in ids) and (
                    len(ids) >= 2 or re.search(r'[(<>=]|->', ids[0])):
                state, how = 'absent', 'identifying strings missing on PS4 ("%s")' % ids[0][:40]
            elif f in roles and 'not imported' in roles[f][3]:
                state, how = 'absent', 'needed PS4 API not imported'
            else:
                state = 'unresolved'
                how = 'no static pairing' + (' (small; probably inlined)' if size <= 0x60 else
                                              ' (log-only on PS3)' if s3.get(f) else '')
        cal = sorted(callers3.get(f, ()))
        cee = []
        for t in c3.get(f, []):
            if t not in cee:
                cee.append(t)
        glob = sorted(set(v for s, v, k in an.refs if k == 'm' and f <= s < f + size and 0xe20000 <= v < 0x1100000))
        rows.append({
            'area': ar or 'other', 'file': fl or '-', 'role': role,
            'ps3': '0x%x' % f, 'ps3_size': '0x%x' % size,
            'ps4': ('0x%x' % g) if g is not None else 'absent' if state == 'absent' else '-',
            'state': state, 'evidence': how.replace('\t', ' ').replace('\n', ' '),
            'ps3_callers': ' '.join('%x' % x for x in cal[:6]) + (' +%d' % (len(cal) - 6) if len(cal) > 6 else ''),
            'ps3_callees': ' '.join('%x' % x for x in cee[:8]) + (' +%d' % (len(cee) - 8) if len(cee) > 8 else ''),
            'ps3_globals': ' '.join('%x' % x for x in glob[:5]),
            'notes': notes,
        })
    cols = ['area', 'role', 'ps3', 'ps4', 'state', 'evidence', 'notes', 'file', 'ps3_size',
            'ps3_callers', 'ps3_callees', 'ps3_globals']
    if cmd == 'all':        # all <out.tsv> <fragment dir>: tsv + md fragments in one run
        import contextlib
        jobs = [('tsv', [args[0]], 'tsv.log'), ('counts', [], 'counts.txt'), ('events', [], 'events-md.txt'),
                ('rows', ['natives'], 'natives-md.txt')]
        jobs += [('rows', [a, '--roles'], 'roles-%s-md.txt' % a) for a in ('match', 'modes', 'players', 'weapons', 'progression')]
        os.makedirs(args[1], exist_ok=True)
        for c, a, fn in jobs:
            with open(os.path.join(args[1], fn), 'w', encoding='utf-8') as fo, contextlib.redirect_stdout(fo):
                emit(c, a, rows, cols, roles, evtype, sender, names, N)
        return 0
    return emit(cmd, args, rows, cols, roles, evtype, sender, names, N)


def emit(cmd, args, rows, cols, roles, evtype, sender, names, N):
    if cmd == 'tsv':
        with open(args[0], 'w', encoding='utf-8', newline='\n') as fo:
            fo.write('# Uncharted 2 MP match/gameplay/progression functions, PS3 1.10 vs PS4 1.02. '
                     'Generated by u2mp/tools/u2_match_report.py; see FUNCTIONS-MATCH-GAMEPLAY.md\n')
            fo.write('\t'.join(cols) + '\n')
            for r in rows:
                fo.write('\t'.join(r[c] for c in cols) + '\n')
        print(len(rows), 'rows')
    elif cmd == 'counts':
        c = collections.defaultdict(collections.Counter)
        for r in rows:
            c[r['area']][r['state']] += 1
            c['ALL'][r['state']] += 1
        for a in c:
            print(a, sum(c[a].values()), dict(c[a]))
    elif cmd == 'rows':
        want = args[0]
        only_roles = '--roles' in args
        for r in rows:
            if r['area'] != want:
                continue
            if only_roles and int(r['ps3'], 16) not in roles:
                continue
            print('| %s | %s | %s | %s | %s | %s |' % (r['role'].replace('|', '/'), r['ps3'], r['ps4'], r['state'],
                                                        r['evidence'].replace('|', '/'), r['notes'].replace('|', '/')))
    elif cmd == 'events':
        for t in range(N.NTYPES):
            sl = evtype.get(t, {})
            def cell(k):
                x = sl.get(k)
                if not x or not x['ps3']:
                    return '-'
                st = x['ps4_state']
                return '%s -> %s %s' % (x['ps3'], x['ps4'], 'STUB' if st.startswith('stub') else st)
            snd = sorted('%x' % f for f, n in sender.items() if names.get(t) in n)
            print('| %d | %s | %s | %s | %s | %s |' % (t, names.get(t, '?'), cell(2), cell(3), cell(4), ' '.join(snd[:3])))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
