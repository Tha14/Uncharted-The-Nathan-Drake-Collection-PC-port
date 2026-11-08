# Port job "gameplay2": remaining match-gameplay stubs

Static analysis only (nothing built or run). PS3 = 1.10 EBOOT, PS4 = Collection 1.02 image.
Code: `gow3_pc/src/u2mp/gameplay2.cpp`, `install_gameplay2(int &installed)` (not wired yet: the
lead adds it to install.cpp; it should run after `install_events`).

## Ported

| Site | PS3 | PS4 | Done |
|---|---|---|---|
| NetEventStartConnection::Apply (type 0, slot 4) | 0x474e50 -> 0x43d94c | 0x63c0e0 (ret), vtable 0x1041520 | vtable slot 4 -> `start_connection_apply` |
| NetEventConnectionDone::Apply (type 1, slot 4) | 0x474d90 -> 0x43dcd8 | 0x63bda0 (ret), vtable 0x10414d0 | vtable slot 4 -> `connection_done_apply` |

PS3 handshake: host NpNetUpdate 0x43bdd0 keeps the peer being connected in a pending net id
(0xe15594) and sends it StartConnection. The client's Apply sets 0xf12620 = 1; its NpNetUpdate then
connects to the other members and answers ConnectionDone(canTalkToAll, avg, avg) to the host
(0x43c8b4..0x43c958, sender 0x489ec8), clearing the flag. The host's Apply 0x43dcd8 takes the
tracker of the pending id: +580 = canTalkToAll, +573 = 1 (PS4 +0x27c / +0x275), pending = -1. The
two averages are only printed.

PS4 has no NpNetUpdate and no pending-id / flag globals (only NpNetUpdate and the PS3 init/reset
functions 0x4383fc, 0x43a444, 0x444bb0, 0x445020 use them). The ports:
- StartConnection (client, from a real connection): answers at once with the intact PS4 sender
  `0x60c0f0(host net id = match+0xb04, canTalkToAll = 1, 0, 0)` - the event arrived over our
  transport, and every client reaches the others through the host (session.cpp sets
  `T_TALKS_TO_ALL = 1` for the same reason). Host or local Apply: only sets our flag and logs.
- ConnectionDone (host): the sender is taken from the connection argument instead of the pending
  id: M = record + 0x40 of the records at 0x1152a18 (stride 0x220, as gamenet.cpp), record+0 key =
  tracker+0x23c. Sets tracker+0x27c / +0x275 and logs the PS3 message. NULL or stand-in
  connections (session.cpp `mailbox_post`) are ignored.
- Payload offsets from the surviving PS4 Read 0x63bf70 (+0xc bit, +0x10 / +0x14 32 bits) and the
  sender 0x60c0f0; tracker offsets from "Connections done" 0x5f2ea0 (+0x275, +0x26c).
- Install checks: those byte anchors, the vtable references at 0x60c165 / 0x60bf8c, and that each
  slot 4 still holds its ret stub.

Today nothing on PS4 sends either event (senders 0x60bf20 / 0x60c0f0 are dead code, session.cpp sets
+0x275 when it registers a player), so this only matters if the session layer starts using the
handshake.

## Checked, nothing to port

| Item | Finding |
|---|---|
| StartConnection Read/Write 0x63c0f0 / 0x63c100 | PS3 0x468c1c / 0x468c18 are `blr`; the event has no payload. |
| EarnMedalClient (60) Read/Write 0x61d260 / 0x61d270 | PS3 0x468dc4 / 0x468dc0 `blr`; Apply PS4 0x61d250 and PS3 0x468dc8 are both empty too. |
| CompleteTask (65) Read/Write 0x62d8f0 / 0x62d900 | PS3 0x468dd0 / 0x468dcc `blr`; Apply PS4 0x62d820 (PS3 0x4711c0 -> 0x49f510) never reads the event. |
| DCNetGetNetIdFromPlayer 0x668fe0 (0xd0 vs 0x530) | Not partial: PS3 0x4ce030 is the same tracker loop unrolled 10 times (0x4919b0(ngm, i) per index). PS4: same multiplayer check, name lookup 0x9bbe40 (= PS3 0x70cd08), handle check +0x44 (PS3 +0x38), returns tracker+0x120 (PS3 +280), same two error messages. |
| "(no pat found)" PS3 0x45e728 | Inlined on PS4. Its only caller PS3 0x45f7c0 is NetCharacter vtable 0xd709a0 slot 10 = PS4 vtable 0x100b010 slot 12 = 0x5fe4b0 (slots aligned by 0x45b8c8/0x601f40, 0x45dad0/0x600230, 0x45ce80/0x6016b0, 0x45f3d0/0x5fdef0, 0x45c124/0x6001a0). After the same three virtual calls (PS3 this+7768, PS4 +0x2cb0) PS4 runs the three ground-probe reads at +0x2d70 / +0x2df0 / +0x2e70 and stores the surface pattern to +0x2ef0.. (PS3 probes +7952/+8048/+8144, results +8240/+8244/+8248). Only the debug text (flag 0xf52d75) is gone. |
| HashTable::Init PS3 0x494ee0 / 0x4950f0 | Called only by NetGameManager::Init 0x49c740 (absent on PS4). Every PS4 instance is inlined; netinit.cpp already builds both tables (ngm+0x1ba0, +0x1c10; NETINIT.md). |

## Open: NetAnimList::AddAnim PS3 0x45212c

- AddAnim(list, anim, b, float, byte): 16-byte entries {+0 anim, +4 b, +8 float, +0xc byte, +0xd 0},
  count at list+0x100, at most 16 (assert "m_numAnims < kMaxAnims"). No inlined copy on PS4 (no
  `[+0x100]` count against 15/16 anywhere).
- Its only caller is PS3 0x453c10 (0xc98 bytes), NetAnimCmdGenerator vtable (vptr 0xd705a8) slot 5.
  PS4 vtable vptr 0x103fab0 (stored at 0x216e3c, 0x5fb869, 0x60671d). Pairing by content:
  slot 0/1 dtors, PS3 slot 3 0x45319c CreateAnimCmds = PS4 slot 2 0x5f3ca0 (both parse two snapshots
  with 0x4da608 = 0x66b430 and check the art group 0x4db060 = 0x66b070), PS3 slot 4 0x452fd4 = PS4
  slot 3 0x5f3f70 (interpolated locator between snapshots), PS3 slot 6 0x452e24 = PS4 slot 5 0x5f38e0
  ("Snapshot [%d]"). So 0x453c10 = PS4 slot 4 = **0x5f48d0, a ret stub** (PS3 slot 2 "return 7"
  0x451e38 sits at PS4 slot 8 0x5f3c90).
- What 0x453c10 does: builds the current NetAnimLists from the snapshots (0x4539f8, which exists on
  PS4 only inlined, around 0x246d10), compares them with the list kept at this+1856 (count this+2112),
  re-adds the playing anims with AddAnim (fade values) and starts/updates them on the animation
  controller with 0x7ab88 (anim entry +4 -> +36 flags, +40 -> +34 frame count / +44, +56). It looks
  like the receive side that plays remote characters' animations.
- Not ported: it needs the PS4 NetAnimCmdGenerator layout (+1856 list, +2112), the PS4 equivalent of
  0x7ab88 and of the anim structs, and a re-implementation of the inlined 0x4539f8 - none verified,
  and a wrong guess would corrupt animation state of every remote player. Next step: a two-player
  run checking whether remote characters animate; if not, find PS4 callers of vtable +0x20 on these
  objects and port 0x453c10 with those layouts.
