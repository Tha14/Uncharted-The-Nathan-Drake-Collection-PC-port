"""Prepare and run Uncharted: The Nathan Drake Collection on the local runtime.

The launcher (launcher/uncharted_launcher.pyw) starts this with --boot --seconds 0. Its
environment carries the options: UNCHARTED_DIAGNOSTICS=0 leaves out the measurement logging and
live trigger files, GOW3_VBLANK_HZ=0 unlocks the frame rate (480 Hz vblank, menu limit).
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import shutil
import struct

ROOT = Path(__file__).resolve().parent.parent

# Code patches, applied to the linked image by byte pattern (hex, "??" = any byte). A patch lists
# variants (description, [(pattern, offset in pattern, original bytes, replacement bytes), ...]):
# the first variant whose pattern occurs exactly once is applied. The variants cover the game
# versions 1.00 and 1.02 (the update recompiled all three executables). A variant can also make
# several changes, all or none: (pattern, [(offset, original bytes, replacement bytes), ...]).
def pattern_regex(text):
    import re
    text = text.replace(" ", "")
    return re.compile(b"".join(b"." if text[i:i + 2] == "??" else re.escape(bytes.fromhex(text[i:i + 2]))
                               for i in range(0, len(text), 2)), re.S)


# The wait for a frame's GPU end-of-pipe stamp gives up after 2.0 s (vucomiss [2.0f]; ja). On PC
# the command processor can pause that long (shader compilation, readbacks), never on a PS4.
# 1.00 then asserts m_gfxEopTick of frame N-3 in FrameBegin (fg-draw-mgr.cpp:234; U1 0x3fd816);
# 1.02 halts right there (int 0x41; U1 0x4fdf96). NOP the `ja` that leaves the loop on timeout.
# The three games have the same loop; only the rip-relative offset of the 2.0f differs.
GPU_WAIT_PATCH = ("wait for GPU frame completion without the 2 s timeout", [
    ("c5f258c0 c5f82e05 ???????? 7739 f390 beffffffff", 12, bytes.fromhex("7739"), bytes.fromhex("9090")),
    ("c5f258c0 c5f82e05 ???????? 7710 f390 498b00", 12, bytes.fromhex("7710"), bytes.fromhex("9090")),
])
EBOOT_PATCHES = [
    GPU_WAIT_PATCH,
    # Silent sound effects (footsteps, gunfire, ambience; music and speech fine): the sound config
    # loader (biglib audio acm 0x515a50) opens reverbs/mixers/environments/prototypes.txt through
    # the sound file callback 0x8ef80 with flag 1, which `testb $5,%cl` at 0x8f1da turns into O_CREAT.
    # Our Fios2 rejects READ|CREAT on read-only /app0 with SCE_FIOS_ERROR_READ_ONLY (0x80820015,
    # mapped to EROFS 0x1e) before any kernel call, so no sound prototypes load and every effect
    # request resolves to nothing. Test bit 2 only: a read request opens read-only. (1.00 and 1.02)
    ("sound config files open read-only (no O_CREAT on /app0)", [
        ("4a8d542b14f6c1050f95c00fb6c0c1e0068db000020000f6c106", 5,
         bytes.fromhex("f6c105"), bytes.fromhex("f6c104")),
    ]),
]

# Uncharted 2's player torch aims with the camera. The torch update (Flashlight Active
# 0x69a190) aims the light along the torch joint (camera mode 9: the camera's horizontal forward,
# 0x69afbd), and then, every frame, the player light (record 0 of the light table 0x1157760) is
# re-aimed: target = pos + dir * K, dir = the camera's forward vector (0x104e820 + 0x270),
# pos = target - dir * K (function 0x698b50 and a copy inlined next to UpdatePadLightBar,
# 0x3fd78d). Turn the camera to face Nate and the light falls on the wall behind him. Skip both
# re-aims and take the joint path in every camera mode: the light follows the torch, like the
# beam drawn from it. Optional (UNCHARTED_TORCH_FORWARD=0 keeps the original); 1.02 only (1.00
# lays this code out differently).
TORCH_FORWARD_PATCHES = [
    ("Uncharted 2 torch: no re-aim along the camera (frame update copy)", [
        ("488d05cc9fd500 8b4850 84c9 7440 8a50", 12, bytes.fromhex("7440"), bytes.fromhex("eb40")),
    ]),
    ("Uncharted 2 torch: no re-aim along the camera (flashlight.cpp)", [
        ("488d0509ecab00 8b4850 84c9 7440 8a50", 12, bytes.fromhex("7440"), bytes.fromhex("eb40")),
    ]),
    ("Uncharted 2 torch: aimed by its joint in every camera mode", [
        ("833df040de0009 0f8534010000 4885c00f842b010000", 7,
         bytes.fromhex("0f8534010000"), bytes.fromhex("90e934010000")),
    ]),
]

# Idle job workers sleep (all three games, 1.02). The engine's 6 NdJobWorkerThreads spin on their
# job queues with `pause` unless the job manager's sleep flag (byte +0x59, set by its own
# SetSleepMode, U3 0xd96440) is on; with it on, an idle worker waits on the manager's condition
# variable after 1000 spins (10 ms timeout) and every job submission broadcasts to wake them. On
# the PS4 each worker owns a core and the flag stays off; on a 4-core PC the six spinning workers
# took 390% of 400% CPU (GPU command thread 5%, title screen 19-27 FPS on an RTX 5080). Treat the
# flag as on in the worker loop, its re-check before waiting, and the submit path.
# Another retail 1.02 build (EP9000-CUSA02344, found by currantsforeyes, Supermedo/uncharted_pc
# #346) has the same three sites with other registers: the re-check loads the manager through r13
# instead of r15, and the submit path tests rdi and jumps with a short je; its worker loop is the
# same. Each site takes whichever form matches. UNCHARTED_JOB_SLEEP=0 keeps the original.
JOB_SLEEP_PATCHES = [
    ("idle job workers sleep (worker loop)", [
        ("4d8d7001 807f5900 74?? 4981f8e8030000 7c??", 8, bytes.fromhex("74"), bytes.fromhex("9090")),
    ]),
    ("idle job workers sleep (re-check before waiting)", [
        ("498b07 80785900 74?? 4889c7 4883ef80", 7, bytes.fromhex("74"), bytes.fromhex("9090")),
        ("498b4500 80785900 74?? 4889c7 4883ef80", 8, bytes.fromhex("74"), bytes.fromhex("9090")),
    ]),
    ("idle job workers sleep (job submission wakes them)", [
        ("80785900 0f84????0000 4883c060", 4, bytes.fromhex("0f84"), bytes.fromhex("660f1f440000")),
        ("807f5900 74?? 4883c760 beffffffff", 4, bytes.fromhex("74"), bytes.fromhex("9090")),
    ]),
]

# The games' own Options > Controls > Aiming > Sensitivity (1.02): a 10-notch slider from 0 to 1.0
# with 0.5 in the middle (setting 0x1abac2d6; Reset Defaults sets 0.32). The aim speed is mixed
# linearly from two tuning values by it, and the aiming code asserts it is at most 1 ("sensitivity
# <= 1.0f"). Mouse aiming goes through the stick there, so the game's aim speed caps it: lift the
# check and raise the slider's top. The value is a float in the save file, read with or without
# this. UNCHARTED_AIM_RANGE=0 keeps the original sliders.
AIM_RANGE_PATCHES = {
    # Uncharted 1: the check at 0x276308; the slider (0x1453e4) holds steps, min, mid and max at
    # +0x20..+0x2c and gets mid 2.5, max 5.0: a notch is 0.5, the second one the old maximum.
    "eboot.bin": [
        ("Uncharted 1 aim sensitivity above 1 (aim speed range check)", [
            ("c5f82e05 ???????? 763d c5fa1145f4", 8, bytes.fromhex("763d"), bytes.fromhex("eb3d")),
        ]),
        ("Uncharted 1 aim sensitivity slider up to 10", [
            ("488d0dd5738000 48894818 48c740200a000000 48b9 0000003f0000803f 48894828", 21,
             bytes.fromhex("0000003f0000803f"), bytes.fromhex("0000a04000002041")),
        ]),
    ],
    # Uncharted 2: the check in 0x719d80; the slider item (0x5d25b1) takes its max from a constant
    # (mid = max / 2): point it at a 5.0, so a notch is 0.5 and the second one the old maximum.
    "big2-ps4_Shipping.elf": [
        ("Uncharted 2 aim sensitivity above 1 (aim speed range check)", [
            ("c5f82e05 ???????? 7649 488b05", 8, bytes.fromhex("7649"), bytes.fromhex("eb49")),
        ]),
        ("Uncharted 2 aim sensitivity slider up to 10", [
            ("81f9d6c2ba1a 0f85???????? 4989ff 4c8d35???????? c5fa1005 75fa8800", 26,
             bytes.fromhex("75fa8800"), bytes.fromhex("4de08800")),
        ]),
    ],
    # Uncharted 3: the check in UpdateAiming3Settings (0x927fe5). The slider's value -> notch
    # (0x62bf60) and notch -> value (0x62c090) functions keep min/mid/max in registers (0, 0.5,
    # 1.0); the aim setting's branch in each is rewritten in place: the same per-profile address
    # (array 0x17f97a0 + profile * 64 + 0x20, in fewer bytes: imul, then sign extension) and max =
    # 1.0 doubled twice = 4.0. The left half keeps its 0.1 notches, the right half goes 0.5 to 4.0.
    "big3-ps4_Shipping.elf": [
        ("Uncharted 3 aim sensitivity above 1 (aim speed range check)", [
            ("c5fa1015???????? c5f82ed0 7351 488b05", 12, bytes.fromhex("7351"), bytes.fromhex("eb51")),
        ]),
        ("Uncharted 3 aim sensitivity slider up to 10 (value -> notch)", [
            ("81f9d6c2ba1a7547 488d05dd63190148630048c1e006488d0dcfd71c01488d440120eb2b", 8,
             bytes.fromhex("488d05dd63190148630048c1e006488d0dcfd71c01488d440120eb2b"),
             bytes.fromhex("6b05dd63190140488d0df6d71c014801c8f30f590d13c1b4ffeb2c90")),
        ]),
        ("Uncharted 3 aim sensitivity slider up to 10 (notch -> value)", [
            ("3dd6c2ba1a7520 488d0da662190148630948c1e106488d1598d61c01488d4c0a20c5f828daeb36", 7,
             bytes.fromhex("488d0da662190148630948c1e106488d1598d61c01488d4c0a20c5f828daeb36"),
             bytes.fromhex("6b0da662190140488d15bfd61c014801d1c5f828daf30f590dd8bfb4ffeb3790")),
        ]),
    ],
}

# FrameBegin (ndlib fg-draw-mgr.cpp, all three games, 1.02) frees frame N-3's instances and
# asserts that frame's end-of-pipe GPU timestamp is set (m_gfxEopTick of the render frame params,
# 32 slots: +0xa28, Uncharted 1 +0xa08). Players' crash logs (GitHub #11/#12/#17, RTX 3050 and
# 5060, and ours) show it after 40-55 ms stalls (shader compiles, waits for a GPU that fell behind):
# the timestamp, written when the GPU gets there (or after a ray-cast readback lands), was not
# there yet. On the PS4 the GPU never lags that far. The assert's path (59 bytes up to where the
# check continues) becomes a wait for the timestamp: pause in a loop until it is set, at most
# 50 million times (about 0.1-1.5 s by CPU), then carry on. rdx (frame N-3) is kept; the rest of
# the path's registers were scratch there. UNCHARTED_FRAME_WAIT=0 keeps the assert.
def frame_begin_wait(offset):
    code = (bytes.fromhex("488dbc08") + offset.to_bytes(4, "little")  # lea rdi, [rax + rcx + offset]
            + bytes.fromhex("be80f0fa02")  # mov esi, 50000000
            + bytes.fromhex("48833f00")  # loop: cmp qword ptr [rdi], 0
            + bytes.fromhex("7506")  # jne done
            + bytes.fromhex("f390")  # pause
            + bytes.fromhex("ffce")  # dec esi
            + bytes.fromhex("75f4")  # jnz loop
            + bytes.fromhex("eb20"))  # done: jmp to where the check continues
    return code + b"\xcc" * (59 - len(code))


FRAME_BEGIN_ASSERTS = {  # executable: (the check and its assert path, timestamp offset)
    "eboot.bin": ("4883bc08080a000000753b488b057035be004885c0742d488d3dd2053400488d35710434004889d3"
                  "488d15fd053400488d0dd01c350041b8ea0000004531c9ffd04889dacd41", 0xa08),
    "big2-ps4_Shipping.elf": ("4883bc08280a000000753b488b05609b99004885c0742d488d3dad5f2e00488d354c5e2e00"
                              "4889d3488d15d85f2e00488d0d38ff210041b8ea0000004531c9ffd04889dacd41", 0xa28),
    "big3-ps4_Shipping.elf": ("4883bc08280a000000753b488b05f06edb004885c0742d488d3d7df56400488d351cf46400"
                              "4889d3488d15a8f56400488d0d6409570041b8ea0000004531c9ffd04889dacd41", 0xa28),
}
FRAME_BEGIN_WAIT_PATCHES = {
    executable: [(f"FrameBegin waits for frame N-3's GPU timestamp (no m_gfxEopTick assert)", [
        (code, 11, bytes.fromhex(code)[11:], frame_begin_wait(offset)),
    ])]
    for executable, (code, offset) in FRAME_BEGIN_ASSERTS.items()
}

# Uncharted 3 (1.02) Player::UpdateFocus (player.cpp:1897, 0xbcee10) springs the point Nate looks
# at ([player + 0xef0], velocity + 0xf00) towards the target the player gives (kept at rbp-0x80) and
# asserts the result is not NaN (GitHub #13: crash climbing the fort in chapter 8). Once NaN it
# stays NaN, and the function's own reset (0xbcf27a, taken when the point is over 6 m off) never
# runs for it: NaN compares false. The assert's path (53 bytes) now takes that reset instead when
# the target itself is a number (velocity register xmm5 zeroed first, as on the reset's usual
# way in); a NaN target still asserts. UNCHARTED_FOCUS_RESET=0 keeps the original.
# The 1.00 checks and assert paths (exact bytes; each occurs once in its 1.00 executable only).
U3_FOCUS_100 = ("c4e37904cb00c5f0c2c900c5f850c183f80f7438e8c3c110004885c07427488d3dd698c100"
                "488d350595c100488d15e598c100488d0d3b95c10041b8690700004531c9ffd0cd41")
U2_FOCUS_100 = ("c4e37904c900c5f0c2c900c5f850c183f80f7438e8a8d71a004885c07427488d3d4c9c8300"
                "488d354b998300488d155b9c8300488d0d7e99830041b84d0400004531c9ffd0cd41")
FOCUS_NAN_PATCHES = {
    "big3-ps4_Shipping.elf": [
        ("Uncharted 3 player focus: reset instead of asserting on NaN", [
            ("c4e37904cb00c5f0c2c900c5f850c183f80f743a488b05f73302014885c07427488d3d74ff8900"
             "488d3546fd8900488d1583ff8900488d0d6ece7d0041b86a0700004531c9ffd0cd41", 20,
             bytes.fromhex("488b05f73302014885c07427488d3d74ff8900488d3546fd8900488d1583ff8900"
                           "488d0d6ece7d0041b86a0700004531c9ffd0cd41"),
             bytes.fromhex("c5d057edc5f8284580c5f8c2c800c5f850c183f80f0f84a5feffffcd41") + b"\xcc" * 24),
            # 1.00 (26 Oct 2026, GitHub #148: the same assert, int 0x41 at 0x3908e9, on a copy
            # without update 1.02): the same function at 0x390330. The assert path at 0x3908b8 is
            # 51 bytes (it fetches the handler with a call); the target sits at rbp-0xa0, the
            # reset is 0x390779 and its velocity register is xmm4.
            (U3_FOCUS_100, 20, bytes.fromhex(U3_FOCUS_100)[20:],
             bytes.fromhex("c5d857e4c5f8288560ffffffc5f8c2c800c5f850c183f80f0f84a3feffffcd41")
             + b"\xcc" * 19),
        ]),
    ],
    # Uncharted 2 (1.02) PlayerBase::UpdateFocus (player-base.cpp:1101, 0x7272e0): the same spring
    # (point [player + 0x2b0], velocity + 0x2c0, target at rbp-0x60) and the same NaN assert (GitHub
    # #24, and the user's own U2 session on 16 Oct). Its reset when the point is far off (0x72769a)
    # snaps the point to the target. The assert's path (53 bytes) now clears the velocity and takes
    # that snap when the target is a number; a NaN target still asserts.
    "big2-ps4_Shipping.elf": [
        ("Uncharted 2 player focus: reset instead of asserting on NaN", [
            ("c4e37904c900 c5f0c2c900 c5f850c1 83f80f 743a 488b05???????? 4885c0 7427 488d3d????????"
             " 488d35???????? 488d15???????? 488d0d???????? 41b84d040000 4531c9 ffd0 cd41", 20,
             bytes.fromhex("488b05e267e4004885c07427488d3d7d4c7600488d352c4b7600488d158c4c7600"
                           "488d0dbdcb6c0041b84d0400004531c9ffd0cd41"),
             bytes.fromhex("c5f8284da0c5f0c2d100c5f850c283f80f7511c5e857d2c5f82993c0020000"
                           "e9e7feffffcd41") + b"\xcc" * 15),
            # 1.00 (26 Oct 2026, GitHub #24's first U2 log: int 0x41 at 0x33ba44): the same function
            # at 0x33b4f0, assert path at 0x33ba13 (51 bytes). Its snap (0x33b8f9) takes the target
            # from rbp-0x60 as in 1.02 (and its y from rbp-0xa0, stored on every way in).
            (U2_FOCUS_100, 20, bytes.fromhex(U2_FOCUS_100)[20:],
             bytes.fromhex("c5f8284da0c5f0c2d100c5f850c283f80f7511c5e857d2c5f82993c0020000"
                           "e9c2feffffcd41") + b"\xcc" * 13),
        ]),
    ],
}


# 1 Nov 2026, GitHub #205, #304 (Uncharted 1), #326 (Uncharted 3, the alley cutscene after the first
# mission): "read access to 0000000000000020" in the same engine job in all three games (U1 1.02
# image 0x616aa0, U2 0xd24fb0, U3 0xf88f10). Its creator (U3 0xf6da4b) allocates a small record
# {object, object->+0x78} and queues the job with it; the job copies the descriptor that +0x78
# points at (mov rax, [r10+8]; mov rcx, [rax+0x20]). When the object's descriptor is not set yet
# (on the PS4 its data was in place earlier) that pointer is null and the job reads address 0x20.
# The two loads now jump to a stub that ends the job for this frame when the pointer is null
# (at the function's own exit path, which pops the profiler scope it pushed), and otherwise carries
# on as before. The stub sits in the dead assert path of that exit ("profiler stack empty"), which
# still starts with int 0x41. UNCHARTED_JOB_PARAMS_GUARD=0: not applied.
# 1 Nov 2026 (GitHub #293, Uncharted 1 and 2 "pAction != NULL" at high frame rates): the player's
# move actions come from a bitmap pool (U2 CreateAction 0x5df3b0: capacity [pool+0x118]) made with
# 64 slots where the character is set up (mov edx, 6; mov ecx, 64; mov r8d, 32). Two move states that
# swap every frame each keep an action per swap; at 120-144 FPS a burst (landing into a fight) fills
# the pool and the assertion stops the game. The pool gets 256 slots (a little more of the
# character's heap). One site in U1 (0x26ae75 in the 1.02 eboot) and one in U2; U3 has no such pool.
# UNCHARTED_MOVE_POOL=0: not applied.
MOVE_ACTION_POOL_PATCH = ("player move action pool: 256 slots (#293)", [
    ("ba06000000 b940000000 41b820000000", 5, bytes.fromhex("b940000000"), bytes.fromhex("b900010000")),
])
# 4 Nov 2026, Uncharted 2 chapter 21 (the convoy): Elena's truck sinks 2-4 m into the snow for about
# half a second after a bump. The bump plays an animation on the truck by adding a node to its
# animation blend (1.02 0x399700: node, anim, params); adding a node copies the params' bool at +0x11
# into bit 2 of the previous node's flags as (flag << 2). The play-animation call the convoy uses
# never writes that bool, so it holds whatever was on the stack: 0x64 << 2 also sets bit 4 ("the
# reference frame follows the object"), the rail animation then takes the truck's own last position
# as its base every frame and the bump's offsets pile up. On the PS4 the stack held other values.
# Only bit 0 of the bool is used now: and cl, 1; shl cl, 2; and byte [rax+0x80], 0xfb;
# or [rax+0x80], cl; nop (the same 20 bytes; dl and the flags are not read after them).
# UNCHARTED_CONVOY_SINK=0: not applied.
CONVOY_SINK_PATCH = ("Uncharted 2 convoy: animation node flag from bit 0 of its bool", [
    ("11 8a9080000000 c0e102 80e2fb 08ca 889080000000", 1,
     bytes.fromhex("8a9080000000c0e10280e2fb08ca889080000000"),
     bytes.fromhex("80e101c0e10280a080000000fb08888000000090")),
])


# gow3 5 Nov 2026: Uncharted 2's leg IK (character-leg-ik.cpp:391, IsFinite(deltaTransform)) asserts
# on a NaN root delta (upstream #88/#253, the chapter 1 train wreck). The assert path is taken only
# when the vector compare found a non-finite lane (vmovmskps; test; jz over it); it now zeroes the
# delta ([r15]) for that frame and goes on where the finite path goes on. 1.02: 58 bytes (the
# handler from a global, then the reload of [r15]); 1.00: 51 bytes (the handler from a call; the
# reload follows). Each occurs once in its executable. UNCHARTED_LEG_IK_RESET=0: not applied.
LEG_IK_NAN_PATCHES = {
    "big2-ps4_Shipping.elf": [
        ("Uncharted 2 leg IK: a NaN delta is zeroed instead of asserting", [
            ("488b05b8cb0f01 4885c0 7427 488d3df8a2a000 488d35c3a1a000 488d15d486a500 488d0d932f9800"
             " 41b887010000 4531c9 ffd0 cd41 c4c1782807", 0,
             bytes.fromhex("488b05b8cb0f014885c07427488d3df8a2a000488d35c3a1a000488d15d486a500"
                           "488d0d932f980041b8870100004531c9ffd0cd41c4c1782807"),
             bytes.fromhex("c5f857c0c4c1782907eb2f") + b"\xcc" * 47),
            ("e8a3cbbfff 4885c0 7427 488d3d1bfe2500 488d3592fc2500 488d1506fe2500 488d0dd1fc2500"
             " 41b887010000 4531c9 ffd0 cd41", 0,
             bytes.fromhex("e8a3cbbfff4885c07427488d3d1bfe2500488d3592fc2500488d1506fe2500"
                           "488d0dd1fc250041b8870100004531c9ffd0cd41"),
             bytes.fromhex("c5f857c0c4c1782907eb2d") + b"\xcc" * 40),
        ]),
    ],
}

JOB_PARAMS_GUARD = {  # executable: (pattern start, the two loads, exit landing, cave)
    "eboot.bin": ("49c7424800000000488b05a4b6b80049894210498b4208488b482049894a18488b4818",
                  0x616aed, 0x616b00, 0x6175ab, 0x617602),
    "big2-ps4_Shipping.elf": ("49c7424800000000488b0514e4940049894210498b4208488b482049894a18488b4818",
                              0xd24ffd, 0xd25010, 0xd25ab9, 0xd25b10),
    "big3-ps4_Shipping.elf": ("49c7424800000000488b05c4c3d60049894210498b4208488b482049894a18488b4818",
                              0xf88f5d, 0xf88f70, 0xf89a19, 0xf89a70),
}
JOB_PARAMS_CAVE_ORIGINAL = {  # the first 24 bytes of each dead assert path (mov rax, [rip+..]; ...)
    "eboot.bin": "488b05efe6a9004885c07427488d3d2ce91e00488d35c4e7",
    "big2-ps4_Shipping.elf": "488b05618484004885c07427488d3dfa731800488d35ef71",
    "big3-ps4_Shipping.elf": "488b05418dc6004885c07427488d3d08414f00488d35fd3e",
}


def job_params_guard_patch(executable):
    pattern, start, loads, landing, cave = JOB_PARAMS_GUARD[executable]
    stub_at = cave + 2                                              # after the kept int 0x41
    stub = bytes.fromhex("cd41 498b4208 4885c0")                    # int 0x41; mov rax,[r10+8]; test
    stub += b"\x0f\x84" + struct.pack("<i", landing - (cave + len(stub) + 6))   # jz exit landing
    stub += bytes.fromhex("488b4820")                               # mov rcx, [rax+0x20]
    stub += b"\xe9" + struct.pack("<i", (loads + 8) - (cave + len(stub) + 5))  # jmp back
    original = bytes.fromhex(JOB_PARAMS_CAVE_ORIGINAL[executable])
    assert len(stub) == len(original) == 24
    jump = b"\xe9" + struct.pack("<i", stub_at - (loads + 5)) + b"\x90\x90\x90"
    return ("Engine job: skip a job whose descriptor is not set yet instead of reading null (#205 #304 #326)", [
        (pattern, [(loads - start, bytes.fromhex("498b4208488b4820"), jump),
                   (cave - start, original, stub)]),
    ])


def u3_light_ortho_guard_patch():
    """28 Oct 2026, GitHub #284 (Uncharted 3 1.02, "Stay in the Light", RTX 4070, after 38 min):
    the game asserts "zscale*n+zoffset" in CreateOrthographicProjection (render-camera.h:441,
    int 0x41 at image 0xf77095). The ortho builder 0xf768d0 fits a light camera around the main
    camera's view slice (near 0.01 m, far from the light's settings or 50 m) and asserts its own
    near plane maps to depth 0, which fails only when the box's depth is 0, infinite or NaN. Its
    caller 0xf3c7e0 (scene-view light camera, called from 0xee64b4 when a light entry is active)
    normalises the light direction and has 0x11c3390 build a light basis by Gram-Schmidt against
    (1,0,0), both with rsqrt and no guard: a zero direction, or one exactly along +-X, gives
    0 * inf = NaN in both light matrices (and a NaN/inf far does the same to the corners). The
    call site (0xf3cad8) now calls a stub that checks the two light matrices (rbp-0x1b0, 8 rows)
    and the far value for NaN/inf: good -> on to 0xf768d0 as before; bad -> this frame's update
    of that light camera is skipped (the ortho build and its follow-up 0x11c0d30 both), so it
    keeps the last good frame's matrices instead of NaN ones. The stub sits in the dead path of
    the neighbouring far-plane assert (0xf770b7, 171 bytes), which still starts with int 0x41;
    the near-plane assert itself is unchanged. UNCHARTED_LIGHT_ORTHO_GUARD=0: not applied."""
    pattern = "c5fa100524ed5000 c5fa108d0cfeffff e8f39d0300 4c89e7 e84b422800"  # 0xf3cac8
    path = 0xf770b7 - 0xf3cac8                # the far-plane assert's path, from the pattern start
    builder = 0xf768d0 - 0xf3cac8
    original = bytes.fromhex(
        "488b05fab6c7004885c00f8499000000488d3dcf4c5300488d352a2252004989d6488d15a14c53004989dc"
        "4889cb488d0d6451430041b8ba0100004531c9c57829adf0feffffc57829b5e0feffffc57829bdd0feffff"
        "c5f82995c0feffffc5f829a5b0feffffc5f829ada0feffffffd0c5f828ada0feffffc5f828a5b0feffff"
        "c5f82895c0feffffc57828bdd0feffffc57828b5e0feffffc57828adf0feffff4889d94c89e34c89f2cd41")
    stub = bytes.fromhex("cd41")                                       # int 0x41 (the old path's start)
    stub += bytes.fromhex("c5f8289d50feffff c5e05cdb")                 # xmm3 = row0 - row0 (NaN if not finite)
    for row in range(1, 8):                                            # + (row - row) for rows 1..7
        stub += bytes.fromhex("c5f828a5") + struct.pack("<i", -0x1b0 + 0x10 * row)  # vmovaps xmm4, [rbp-..]
        stub += bytes.fromhex("c5d85ce4 c5e058dc")                     # vsubps xmm4,xmm4,xmm4; vaddps xmm3,xmm3,xmm4
    stub += bytes.fromhex("c5f25ce1 c5e258dc")                         # + (far - far) (xmm1)
    stub += bytes.fromhex("c5e0c2db03 c5f850c3 85c0 7505")             # any lane NaN? -> bad
    stub += b"\xe9" + struct.pack("<i", builder - (path + len(stub) + 5))  # good: jmp 0xf768d0
    stub += bytes.fromhex("4883042408 c3")                             # bad: return past the follow-up call
    assert len(stub) <= len(original)
    call = b"\xe8" + struct.pack("<i", (path + 2) - (0x10 + 5))
    return ("Uncharted 3 light camera: skip a NaN light ortho instead of asserting (#284)", [
        (pattern, [(0x10, bytes.fromhex("e8f39d0300"), call),
                   (path, original, stub.ljust(len(original), b"\xcc"))]),
    ])


# Test knob for the #284 guard (UNCHARTED_TEST_LIGHT_ORTHO_NAN=1, never in the launcher): the light
# direction's normalisation (0xf3c964 vmulps xmm0, xmm0, xmm1) becomes vxorps xmm0, xmm0, xmm0, so
# every active light camera gets a zero direction -> NaN light matrices -> the #284 assert. The
# Chateau save has no such light active, so the knob also opens the caller's gate (0xee6481 je and
# 0xee649a jae, both skipping the light camera, become nops): the camera is built every frame.
U3_TEST_LIGHT_ORTHO_NAN = ("Uncharted 3 TEST: zero light-camera direction (UNCHARTED_TEST_LIGHT_ORTHO_NAN)", [
    ("c5e8591db41d4f00 c5e059c9 c5e85cc9 c5f859c1 c5f8298510feffff", 16,
     bytes.fromhex("c5f859c1"), bytes.fromhex("c5f857c0")),
])
U3_TEST_LIGHT_ORTHO_GATE = ("Uncharted 3 TEST: light camera built every frame (UNCHARTED_TEST_LIGHT_ORTHO_NAN)", [
    ("488b8360010000 c5f82e8010050000 7436 c5fa1005e94f5600 488b8360010000 c5f82e80680a0000 731d",
     [(15, bytes.fromhex("7436"), bytes.fromhex("9090")), (40, bytes.fromhex("731d"), bytes.fromhex("9090"))]),
])

# The most common Drake's Fortune crash (GitHub #27, #30, #42, #62: "execute access to 0", called
# from 0x64011f; Uncharted 2 and 3 have the same code). For each shader the constant-update code
# fills a table (a 0x28-byte header, then 0x30-byte entries whose 16-bit word picks the member
# function a draw calls later) in a leaf function (U1 1.02 0x63df10, U2 0xd645f0, U3 0x12d6cf0)
# that keeps its running entry indices in the 128 bytes below rsp. The tables lie in a ring next
# to GPU data, so one can cross into a page the GPU tracker has write-protected, and the store
# there faults in the middle of the function. Windows builds the exception frame on the same stack
# and its x64 ABI promises nothing below rsp (the 26300 build on our test PC happens to leave
# 1.6 KB free, which is why it never crashed here; a reporter runs Windows 10). An index then
# reads 0, an entry is written over another, and the walk calls the null member its stale word
# selects. The function's start now jumps to a stub that writes each page of the table once
# (lock or byte [p], 0: no value changes) before anything lives below rsp, then carries on: a
# protected page faults there, harmlessly. The stub sits in a dead assert path nearby, which
# still starts with int 0x41, so that assert still stops the game. UNCHARTED_CUE_PRETOUCH=0: off.
CUE_TABLE_FILL = "55 41 57 41 56 41 55 41 54 53 49 b9 00 24 00 0d 10 00 00 00 48 c7 07 00 00 00 00 45 31 f6"


def cue_pretouch_variant(cave, original):
    """The table fill's first 5 bytes (push rbp; push r15; push r14) become a jump to the stub at
    `cave` (from the start of the function), which replaces the 53-byte assert path `original`."""
    stub = bytes.fromhex(
        "cd41"            # int 0x41 (the assert path's end, now its start)
        "89d0"            # mov eax, edx                 entries
        "486bc030"        # imul rax, rax, 0x30
        "488d440727"      # lea rax, [rdi + rax + 0x27]  the table's last byte
        "4889f9"          # mov rcx, rdi
        "f0800900"        # touch: lock or byte [rcx], 0
        "6681c9ff0f"      # or cx, 0xfff
        "48ffc1"          # inc rcx                      the next page
        "4839c1"          # cmp rcx, rax
        "76ef"            # jbe touch
        "55 4157 4156".replace(" ", ""))  # the moved instructions
    stub += b"\xe9" + struct.pack("<i", 5 - (cave + len(stub) + 5))  # jmp to the function's 6th byte
    original = bytes.fromhex(original)
    assert len(stub) <= len(original) == 53
    return (CUE_TABLE_FILL, [
        (cave, original, stub + b"\xcc" * (len(original) - len(stub))),
        (0, bytes.fromhex("5541574156"), b"\xe9" + struct.pack("<i", cave + 2 - 5)),
    ])


CUE_PRETOUCH_PATCHES = {  # 1.02: the assert path (r14d < 0x10000 in U1, line 0xae4 in U2/U3)
    "eboot.bin": [("constant tables: pages written before the fill (no fault inside it)", [
        cue_pretouch_variant(0xfd7a, "488b056780a6004885c07427488d3de3671d00488d35f8671d00488d1524681d00"
                                     "488d0dca671d0041b8ad0100004531c9ffd0cd41"),
    ])],
    "big2-ps4_Shipping.elf": [("constant tables: pages written before the fill (no fault inside it)", [
        cue_pretouch_variant(-0xa850, "488b05d14181004885c07427488d3dac8e1500488d35e9801500488d15b28e1500"
                                      "488d0daca5090041b8e40a00004531c9ffd0cd41"),
    ])],
    "big3-ps4_Shipping.elf": [("constant tables: pages written before the fill (no fault inside it)", [
        cue_pretouch_variant(-0xb690, "488b05517192004885c07427488d3d4c841b00488d35b4761b00488d1552841b00"
                                      "488d0dc80b0e0041b8e40a00004531c9ffd0cd41"),
    ])],
}

# Uncharted 2 "read access to 0 at guest offset 0xd09665" (GitHub #61 #113 #115 #164 #181 #187;
# 24 Oct 2026): the particle setup's run grouping (particle-setup.cpp; the same leaf in all three
# games: 1.02 U1 0x5fa2f0, U2 0xd09580, U3 0xf6bef0; 1.00 U1 0x4d0b80, U2 0x661150, U3 0x60dd70)
# keeps its loop bounds below rsp: [rsp-8] = array + 0x10, [rsp-0x10] = count - 1 (1.00:
# 1 - count). A page fault inside it (GPU write tracking) lets Windows build the exception frame
# right below the 16-aligned rsp: [rsp-0x10] comes back wrong, the end-of-array test fails and the
# look-ahead reads the entry one past the end (stale memory) and dereferences it. The reloads now
# take the same values from r10 (array) and r11 (count), which the function never changes. Same
# length, in place (analysis/triage-1022/d09665.md). UNCHARTED_RUN_GROUP_FIX=0: off.
RUN_GROUP_PATCH = ("particle run grouping: loop bounds from registers, not below rsp", [
    # 1.02: mov rcx,[rsp-8] -> lea rcx,[r10+0x10]; nop | mov r15,[rsp-0x10] -> lea r15,[r11-1]; nop
    ("4c8b27 488b4c24f8 4a8d1c31 4c8b7c24f0 4929f7", [
        (3, bytes.fromhex("488b4c24f8"), bytes.fromhex("498d4a1090")),
        (12, bytes.fromhex("4c8b7c24f0"), bytes.fromhex("4d8d7bff90")),
    ]),
    # 1.00: mov rcx,[rsp-8] -> the same lea | mov rcx,[rsp-0x10]; lea r8,[rcx+rsi]
    #       -> mov r8,rsi; sub r8,r11; inc r8   (rcx is zeroed right after, r8 = i + 1 - count)
    ("4c8b27 488b4c24f8 4a8d1c31 488b4c24f0 4c8d0431 4531ed", [
        (3, bytes.fromhex("488b4c24f8"), bytes.fromhex("498d4a1090")),
        (12, bytes.fromhex("488b4c24f04c8d0431"), bytes.fromhex("4989f04d29d849ffc0")),
    ]),
])

# Everything unlocked (launcher -> Advanced -> Unlock all chapters, UNCHARTED_UNLOCK=1): the three
# games keep Bluepoint's tweak flags "Unlock Chapter Select by default" and "Unlock Brutal
# difficulty by default" (debug menu; +2/+1 after the Crushing flag, which the Collection already
# sets). At start the game fills the first and the chapter one from the *bp-ucctweaks* table
# (Crushing on, chapters off); Brutal starts off. The chapter flag is now set where the table is
# read, Brutal where it is cleared (1.02; U1 has two copies of that constructor).
# Cheats (launcher -> Advanced -> Cheats, UNCHARTED_CHEATS=invincible,ammo,noreload,grenades,onehit):
# the games keep Naughty Dog's debug-menu switches ("Invincible Player", "Infinite Ammo", "No
# Reload", "Infinite Grenades", "One Hit Kills"; the menu itself is unreachable), each a byte the
# menu item points at. The runtime keeps the chosen ones set (GOW3_POKE). Update 1.02 addresses
# (guest image offsets); Uncharted 3's invincibility sits in an options block the game allocates,
# reached through the pointer at 0x16e2f80.
# The cheats, unlocked rewards and extra quality kept set while the game runs (GOW3_POKE): launcher/native/runtime_env.cpp.

# 19 Oct: Uncharted 1 and 2 keep "Infinite Ammo" in their settings system (the Ultra Easy option
# sets it there, id 0xa6effd08 in U2), which writes the byte back every frame: a poke does not
# stick. Each place that reads it (`cmp byte [flag], 0; jne <infinite ammo>`) takes the jump as if
# it were set (1.02 code; the lea's displacement makes each pattern unique).
def _ammo_reader(pattern, jne_at):
    return (pattern, jne_at, b"\x75", b"\xeb")


# Drake's Fortune keeps each reward's status in its own record (bit 0 unlocked) and its debug
# switch also writes them, so its checks are patched instead: every reward reads as unlocked,
# usable and counted ("Unlocked 47 of 47"), and the Story Cutscenes list skips no chapter. The
# status bytes are not touched (nothing saved; earned rewards are still recorded). 1.02 code.
REWARD_PATCHES = {
    # Uncharted 2 and 3 let some rewards (tweaks, render modes) be enabled only once the game was
    # finished on the current difficulty or a harder one: the reward screen's item info sets
    # "needs the game finished" (+0xe) to 1 and clears it from the save's finished-difficulty bits.
    # It is now set to 0 (the user, 21 Oct: "cannot enable until you beat the game").
    "big2-ps4_Shipping.elf": [
        ("Uncharted 2 rewards can be enabled before the game is finished", [
            ("7441c6430e01833d401fa800", 5, bytes.fromhex("01"), bytes.fromhex("00"))]),
    ],
    "big3-ps4_Shipping.elf": [
        ("Uncharted 3 rewards can be enabled before the game is finished", [
            ("74054c39f0725c41c6450e01488d05", 11, bytes.fromhex("01"), bytes.fromhex("00"))]),
    ],
    "eboot.bin": [
        # IsUnlocked(item): `and al, 1` on status bit 0 -> `mov al, 1`
        ("Uncharted 1 rewards unlocked (item check)", [
            ("b001803f00750d4885f674098a86880000002401", 18, bytes.fromhex("24"), bytes.fromhex("b0"))]),
        # IsUnlocked(category, id)
        ("Uncharted 1 rewards unlocked (id check)", [
            ("75134d85e4740c418a8424880000002401eb02", 15, bytes.fromhex("24"), bytes.fromhex("b0"))]),
        # the "Unlocked N of 47" counter: `je skip` -> `je +0`
        ("Uncharted 1 rewards unlocked (counter)", [
            ("4c89f275154885c07422f68088000000017419", 18, bytes.fromhex("19"), bytes.fromhex("00"))]),
        # usable-by-id check: `je not` -> `je +0`
        ("Uncharted 1 rewards unlocked (usable by id)", [
            ("41803e00750af684088800000001741e488d55d4", 15, bytes.fromhex("1e"), bytes.fromhex("00"))]),
        # CanEnable: `test [item+0x88],1; sete dl` (dl = locked) -> `setb dl` (0: test clears CF)
        ("Uncharted 1 rewards unlocked (enable check)", [
            ("41f68688000000010f94c24584ff0f95c184c0", 9, bytes.fromhex("94"), bytes.fromhex("92"))]),
        # CanEnable's "finish the game on this difficulty first" reason (bit 1, the menu's
        # STORE_REQUIREMENT_CURRENT_DIFFICULTY_ENABLING): `mov r13b, 1` before the finished-
        # difficulty loop -> `mov r13b, 0` (the user, 21 Oct: a problem with rewards in U1)
        ("Uncharted 1 rewards can be enabled before the game is finished", [
            ("448bb87848000041b5014183ff067338", 9, bytes.fromhex("01"), bytes.fromhex("00"))]),
        # Story Cutscenes list: `cmp [node+0x50], 0; js skip` (chapter not reached) -> `js +0`
        ("Uncharted 1 story cutscenes unlocked", [
            ("488b7840837f50000f88880000004c8b4210", 10, bytes.fromhex("88"), bytes.fromhex("00"))]),
    ],
}


def cheat_names(executable):
    """The cheats asked for (UNCHARTED_CHEATS, launcher -> Cheats)."""
    return [name for name in os.environ.get("UNCHARTED_CHEATS", "").replace(" ", "").split(",") if name]


CHEAT_PATCHES = {
    "eboot.bin": {"ammo": [
        (f"Uncharted 1 infinite ammo (reader {n})", [_ammo_reader(pattern, at)])
        for n, (pattern, at) in enumerate((("488d15caca8200 b101 803a00 75", 12),
                                           ("488d0d79673800 803900 75", 10),
                                           ("488d0d3a5b3800 803900 75", 10),
                                           ("488d0d0a023800 803900 75", 10)), 1)] + [
        # 19 Oct (user: "Infinite ammo was not working"): Drake's Fortune's own Infinite Ammo only
        # spares the reserve when reloading (0x345e40/0x345ec0); each shot still takes a round from
        # the magazine (0x346440: `dec [inventory + slot*0x30 + 0x440]`, skipped for weapons whose
        # definition says so). With the cheat on that decrement is gone: the magazine stays full.
        ("Uncharted 1 infinite ammo (magazine)",
         [("89f3 4989fe e8???????? 4885c0 741b 83b82801000000 7512 89d8 488d0440 48c1e004 "
           "41ff8c0640040000 5b415e5dc3", 34, bytes.fromhex("41ff8c0640040000"),
           bytes.fromhex("0f1f840000000000"))])]},
    "big2-ps4_Shipping.elf": {"ammo": [
        (f"Uncharted 2 infinite ammo (reader {n})", [_ammo_reader(pattern, at)])
        for n, (pattern, at) in enumerate((("488d0dbc8d6f00 80793a00 75", 11),
                                           ("488d0d63736f00 80793a00 75", 11),
                                           ("488d0d09436f00 80793a00 75", 11),
                                           ("488d0d98635600 80793a00 75", 11),
                                           ("488d0d3b635600 80793a00 75", 11),
                                           ("4c8d2d285f5600 41807d3a00 75", 12),
                                           ("e8fd3e2600 41807d3a00 75", 10)), 1)]},
}

UNLOCK_PATCHES = {
    "eboot.bin": [
        (f"Uncharted 1 chapter select unlocked ({copy})", [
            (f"e8{call} 4885c0 7411 8a08 888b302c0000 8a4001 8883322c0000", 18,
             bytes.fromhex("8a40018883322c0000"), bytes.fromhex("c683322c0000016690")),
        ]) for copy, call in (("1", "fe250000"), ("2", "f51d0000"))
    ] + [
        (f"Uncharted 1 Brutal difficulty unlocked ({copy})", [
            (f"e8{call} c683302c000001 c683312c000000", 12,
             bytes.fromhex("c683312c000000"), bytes.fromhex("c683312c000001")),
        ]) for copy, call in (("1", "ea3b0000"), ("2", "e1330000"))
    ],
    "big2-ps4_Shipping.elf": [
        ("Uncharted 2 chapter select unlocked", [
            ("833800 0f9583a02d0000 83780400 0f9583a22d0000", 10,
             bytes.fromhex("837804000f9583a22d0000"), bytes.fromhex("c683a22d0000010f1f4000")),
        ]),
        ("Uncharted 2 Brutal difficulty unlocked", [
            ("c683a02d000001 c683a12d000000 c683a22d000000", 7,
             bytes.fromhex("c683a12d000000"), bytes.fromhex("c683a12d000001")),
        ]),
    ],
    "big3-ps4_Shipping.elf": [
        ("Uncharted 3 chapter select unlocked", [
            ("833800 0f9583602c0000 83780400 0f9583622c0000", 10,
             bytes.fromhex("837804000f9583622c0000"), bytes.fromhex("c683622c0000010f1f4000")),
        ]),
        ("Uncharted 3 Brutal difficulty unlocked", [
            ("c683602c000001 c683612c000000 c683622c000000", 7,
             bytes.fromhex("c683612c000000"), bytes.fromhex("c683612c000001")),
        ]),
    ],
}

# Ultrawide (GitHub #20): the games keep a 16:9 camera's horizontal field of view at every aspect
# ratio and lose the top and bottom of the picture instead (Vert-). The render camera holds
# zoom = 1/tan(horizontal FOV / 2) and aspect = height/width (U2/U3 +0x254/+0x250, Drake's Fortune
# +0x294/+0x290); the projection (x scale zoom, y scale zoom/aspect) and the view frustum's culling
# planes are built from them. Where those read the zoom of a perspective camera it becomes
# zoom * min(1, aspect * 16/9): wider than 16:9 keeps the 16:9 picture's vertical view and shows
# more at the sides (Hor+), 16:9 and narrower views and square cameras are unchanged, and
# orthographic cameras keep their own branches. The zoom loads jump to small stubs in dead assert
# branches of the projection code (each now starts with int 0x41, so it still stops the game), and
# back. Applied only to render sizes wider than 16:9 (UNCHARTED_HOR_PLUS=0: never).
_GPR = {name: number for number, name in enumerate(
    ("rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
     "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"))}
_NOPS = {3: b"\x0f\x1f\x00", 4: b"\x0f\x1f\x40\x00", 5: b"\x0f\x1f\x44\x00\x00"}


def _vex_mem(op, dst, src, base, disp):
    """op xmm<dst>, xmm<src>, [base + disp32] (scalar single; vmovss: src 0): VEX 2-byte for
    rax..rdi, 3-byte for r8..r15, with the SIB byte that rsp/r12 as a base need."""
    reg = _GPR[base]
    modrm = bytes([0x80 | dst << 3 | (reg & 7)]) + (b"\x24" if reg & 7 == 4 else b"")
    if reg < 8:
        prefix = bytes([0xc5, 0x80 | (~src & 15) << 3 | 2])
    else:
        prefix = bytes([0xc4, 0xc1, (~src & 15) << 3 | 2])
    return prefix + bytes([op]) + modrm + struct.pack("<i", disp)


def _vex_rip(op, dst, src, at, target):
    """op xmm<dst>, xmm<src>, [rip -> target], the 8-byte instruction placed at `at`."""
    return bytes([0xc5, 0x80 | (~src & 15) << 3 | 2, op, dst << 3 | 5]) + struct.pack("<i", target - (at + 8))


def hor_plus_variant(pattern, zoom, aspect, sites, caves):
    """One patch variant: sites (offset, base register, xmm with the zoom, a free xmm) are
    `vmovss xmm, [base + zoom]` loads; caves (offset, size, original first bytes) are the dead
    assert branches that take the stubs. Offsets are from the start of the pattern's match (a
    cave may lie before it or in a neighbouring function: its original bytes are checked too)."""
    edits, cave_list = [], [list(cave) + [2] for cave in caves]  # + bytes used (after int 0x41)
    stubs = {}
    for offset, base, dst, free in sites:
        assert dst < 8 and free < 8 and dst != free
        length = len(_vex_mem(0x10, dst, 0, base, zoom))
        size = 21 + 2 * length
        cave = next(c for c in cave_list if c[3] + size + 8 <= c[1])
        stubs.setdefault(cave[0], []).append((cave[0] + cave[3], offset, base, dst, free, length))
        cave[3] += size
    for cave_offset, size, original, used in cave_list:
        if cave_offset not in stubs:
            continue
        k, one = cave_offset + used, cave_offset + used + 4
        code = b"\xcd\x41"
        for at, site, base, dst, free, length in stubs[cave_offset]:
            mul_at = at + 8
            min_at = mul_at + length
            jmp_at = min_at + 8 + length
            code += (_vex_rip(0x10, free, 0, at, k) +                     # vmovss free, [16/9]
                     _vex_mem(0x59, free, free, base, aspect) +           # vmulss free, free, [aspect]
                     _vex_rip(0x5d, free, free, min_at, one) +            # vminss free, free, [1.0]
                     _vex_mem(0x59, dst, free, base, zoom) +              # vmulss dst, free, [zoom]
                     b"\xe9" + struct.pack("<i", site + length - (jmp_at + 5)))  # jmp back
            edits.append((site, _vex_mem(0x10, dst, 0, base, zoom),
                          b"\xe9" + struct.pack("<i", at - (site + 5)) + _NOPS[length - 5]))
        edits.insert(0, (cave_offset, original, code + struct.pack("<ff", 16 / 9, 1.0)))
    return pattern, edits


HOR_PLUS_PATCHES = {
    # Render camera update 0x2c9660 (perspective: infinite far 0x2c982e, finite 0x2c9ac7) and its
    # frustum planes 0x2ca4ca (Drake's Fortune 1.02).
    "eboot.bin": [("Drake's Fortune ultrawide: wider view instead of a cropped one", [
        hor_plus_variant("c5fa108b98020000 c5fa109b90020000 c5fa109394020000 c5f857c0 c5f82ec2 7251",
                         0x294, 0x290,
                         [(0x10, "rbx", 2, 0), (0x2a9, "rbx", 3, 0), (0xcac, "rdi", 0, 1)],
                         [(0x22, 0x51, bytes.fromhex("c5fa115dc0c5fa1155d0")),
                          (0x2c1, 0x63, bytes.fromhex("c5fa114db0c5fa115dc0"))]),
    ])],
    # 0x9dc750 (infinite far 0x9dc8fe, finite 0x9dcb55), frustum planes 0x9dd41d (Uncharted 2 1.02).
    "big2-ps4_Shipping.elf": [("Uncharted 2 ultrawide: wider view instead of a cropped one", [
        hor_plus_variant("c5fa10ab58020000 c5fa109b50020000 c5fa10a354020000 c5f857c0 c5f82ec4 0f82b6000000",
                         0x254, 0x250,
                         [(0x10, "rbx", 4, 0), (0x267, "rbx", 4, 0), (0xb2f, "rdi", 0, 1)],
                         [(0x26, 0xb6, bytes.fromhex("488b055d16b9004885c0"))]),
    ]),
    # Shadows past the 16:9 area (Uncharted 2): the culling frustums of the sun, rain and local
    # (flashlight, lamp) shadows are built from the render camera's own zoom and aspect, in
    # copies of the frustum-plane code (view-frustum.cpp MakeSunShadowViewFrustum 0xcccc90,
    # MakeRainShadowViewFrustum 0xccd080, MakeLocalShadowViewFrustum 0xccda70). With the 16:9
    # zoom their side planes stopped at the 16:9 picture: casters past it were culled from the
    # shadow maps, and shadows at the sides came and went as the camera turned. Their zoom loads
    # get the same zoom * min(1, aspect * 16/9); each stub goes in that function's own numPlanes
    # assert (UNCHARTED_UW_SHADOWS=0: not applied).
    ("Uncharted 2 ultrawide: sun shadows as wide as the view", [
        hor_plus_variant("c5fa10b350020000 c5fa10ab54020000 c4637904ce00", 0x254, 0x250,
                         [(0x8, "rbx", 5, 0)],
                         [(-0x194, 0x35, bytes.fromhex("488b0573128a004885c0"))]),
    ]),
    ("Uncharted 2 ultrawide: rain shadows as wide as the view", [
        hor_plus_variant("c5fa10ab50020000 c5fa10a354020000 c4637904c500", 0x254, 0x250,
                         [(0x8, "rbx", 4, 3)],
                         [(-0x124, 0x35, bytes.fromhex("488b05a50e8a004885c0"))]),
    ]),
    ("Uncharted 2 ultrawide: local light shadows as wide as the view", [
        hor_plus_variant("c4c17a10942454020000 c4c17a10ac2450020000", 0x254, 0x250,
                         [(0x0, "r12", 2, 5)],
                         [(0x183, 0x35, bytes.fromhex("488b05c6018a004885c0"))]),
    ]),
    # The scene view's GPU constants (0xcfcb00, called from the scene view setup 0xc901c0): two more copies of the frustum code and the normalized side planes
    # written into a constant buffer, for the render camera (r12) and a second view (r13). Same
    # zoom (a square view keeps its own: min(1, 1.0 * 16/9) = 1). Stubs in the function's two
    # SetConstantBuffers asserts and two of the neighbouring function 0xd01020's
    # (UNCHARTED_UW_VIEW_CONSTANTS=0: not applied).
    ("Uncharted 2 ultrawide: view constants as wide as the view (1)", [
        hor_plus_variant("c4c17a109c2454020000 c4417a10bc2450020000", 0x254, 0x250,
                         [(0x0, "r12", 3, 5)],
                         [(0x10e7, 0x38, bytes.fromhex("488b0549fa86004885c0"))]),
    ]),
    ("Uncharted 2 ultrawide: view constants as wide as the view (2)", [
        hor_plus_variant("c4c17a10842454020000 c57a100d???????? c5b25ec8", 0x254, 0x250,
                         [(0x0, "r12", 0, 1)],
                         [(0x2a24, 0x38, bytes.fromhex("488b0519da86004885c0"))]),
    ]),
    ("Uncharted 2 ultrawide: view constants as wide as the view (3)", [
        hor_plus_variant("c4c17a109d54020000 c4c17a10a550020000", 0x254, 0x250,
                         [(0x0, "r13", 3, 4)],
                         [(0x28d4, 0x35, bytes.fromhex("488b0504c486004885c0"))]),
    ]),
    ("Uncharted 2 ultrawide: view constants as wide as the view (4)", [
        hor_plus_variant("c4c17a108554020000 c5da5ec8", 0x254, 0x250,
                         [(0x0, "r13", 0, 1)],
                         [(0x2189, 0x35, bytes.fromhex("488b0568c186004885c0"))]),
    ])],
    # Infinite far 0x11c1107 (both of its paths), finite 0x11c11e1 and 0x11c1406, frustum planes
    # 0x11c1f21 (Uncharted 3 1.02).
    "big3-ps4_Shipping.elf": [("Uncharted 3 ultrawide: wider view instead of a cropped one", [
        hor_plus_variant("80bb6902000000 c5fa10a358020000 c5fa108b50020000 c5fa109354020000 0f840a020000",
                         0x254, 0x250,
                         [(0x17, "rbx", 2, 3), (0xf1, "rbx", 2, 0), (0x316, "rbx", 1, 0), (0xe31, "rdi", 0, 1)],
                         [(0x10d, 0x9f, bytes.fromhex("488b05b415a3004885c0")),
                          (0x32e, 0x65, bytes.fromhex("488b059313a3004885c0"))]),
    ])],
}


def hor_plus_patches(executable):
    """HOR_PLUS_PATCHES of the game, without the groups switched off in the environment."""
    skip = []
    if os.environ.get("UNCHARTED_UW_SHADOWS", "1") == "0":
        skip.append("shadows as wide as the view")
    if os.environ.get("UNCHARTED_UW_VIEW_CONSTANTS", "1") == "0":
        skip.append("view constants as wide as the view")
    return [patch for patch in HOR_PLUS_PATCHES.get(executable, [])
            if not any(text in patch[0] for text in skip)]


PATCHES = {
    "eboot.bin": EBOOT_PATCHES,
    "big2-ps4_Shipping.elf": [GPU_WAIT_PATCH],
    "big3-ps4_Shipping.elf": [GPU_WAIT_PATCH],
}
EXECUTABLES = tuple(PATCHES)
# Exit code of sceSystemServiceLoadExec (runtime_services.c): the game asked for another
# executable of the Collection; GOW3_LOADEXEC_FILE holds its /app0 path and arguments.
LOADEXEC_EXIT = 30


def job_sleep_enabled(executable=None):
    """JOB_SLEEP_PATCHES: on unless UNCHARTED_JOB_SLEEP=0 (launcher: Idle game threads -> Spin).
    Spinning job threads starve 4-6 core CPUs (GPU thread 5% of a core), heat laptops and, on a
    16-core desktop, used about 4 more cores for 2% fewer FPS.
    1 Nov 2026 (GitHub #274: Uncharted 1 uneven although the frame times looked flat; a player found
    Spin fixed it): each sleeping worker wakes a little late, which makes the game's own job timing
    uneven. Measured on the desktop (U1, full CPU): Spin 0.1% low 64 -> 110 FPS, worst frame 18.4 ->
    9.4 ms. auto (the default): Spin when the CPU has at least JOB_SPIN_MIN_THREADS hardware
    threads for this game, Sleep below and in games without an entry (4-core CPUs fall to ~5 FPS
    with Spin)."""
    import game_patcher
    return game_patcher.starter("job_sleep", executable or "") == "1"


# 1 Nov 2026 (#274, analysis/optimization-1028/runs/js-*): Spin helps with many CPUs (U1 0.1% low
# 64 -> 110 FPS on 16 threads). On 8 threads (4 cores x 2) U1 still gains a lot (the jerky camera:
# 0.1% low 59 -> 109 FPS, 13 hitches a minute -> 0) while U2 and U3 lose (U2 1% low 105 -> 83);
# on 4 threads Spin starves the game (U2 85 -> 4.5 FPS, U3 119 -> 5.9). Auto: Spin from 8 logical
# CPUs in Uncharted 1. Uncharted 2 and 3 keep Sleep (0.8.0): nothing measured a gain from Spin
# there (8 threads: U2 1% low 105 -> 83; 17 Oct, 16 cores: about 2% fewer FPS).
# JOB_SPIN_MIN_THREADS: Uncharted 1 from 8 hardware threads (launcher/native/starter.cpp job_sleep).


def host_hardware_threads():
    """Logical processors this process may run on (its affinity mask; the game inherits it)."""
    import game_patcher
    return int(game_patcher.starter("hardware_threads"))


def job_worker_count():
    """UNCHARTED_JOB_WORKERS=<1..6> | auto | 0/off (the engine's own 6). Threads work 28 Oct 2026
    (analysis/optimization-1028/THREADS.md): the job manager's init parameters (U2 0x5766e8, U1
    0x126543, U3 0x5acd80/0x5ad750, `mov qword [rbp-0x68], 6`) ask for six NdJobWorkerThreads, one
    per PS4 game core (affinity 1 << i, priority 767). auto: the host's hardware threads minus two
    (the GPU command thread and the game's main/submission threads), 2..6."""
    import game_patcher
    return int(game_patcher.starter("job_workers"))


def job_worker_patches(executable, count):
    """The job manager's worker count (6) in its init parameters; Uncharted 3 has two init sites
    (the same code with r13/r15)."""
    tail = "48c7458800080000 48c7459000040000 48c74598"
    new = bytes([count])
    description = f"job workers: {count} instead of 6 (UNCHARTED_JOB_WORKERS)"
    if executable == "big3-ps4_Shipping.elf":
        return [(description + f" (site {i})", [(f"4c89{reg}68ffffff 48899d70ffffff " + tail + " 06000000 48c745a0", 34, b"\x06", new)])
                for i, reg in enumerate(("ad", "bd"))]
    return [(description, [(tail + " 06000000 48c745a0 80000000", 20, b"\x06", new)])]


def u3_exposure_bias():
    """UNCHARTED_U3_EXPOSURE=<EV> (launcher: Uncharted 3 exposure; 0 = the game's own): GitHub
    #199/#195/#204/#212, Uncharted 3 too bright on some RTX 30/40 PCs, not on the test PC."""
    import game_patcher
    return float(game_patcher.starter("u3_exposure"))


def u3_exposure_patch(bias):
    """Uncharted 3 1.02's tonemap constants (pavlikdcx's patch in GitHub #199): the filmic exposure
    bias, read from the render settings at [rbx+0x72c] on two paths (image 0xed9d30 / 0xed9d49),
    gets <bias> EV more before the tonemap. Main path: the load jumps to a stub in the code
    segment's zero padding (image 0x1648800) that adds the bias; second path: its add of -0.5
    (shared constant 0x144b3b8) reads our own (-0.5 + bias) instead."""
    pattern = "c5fa10932c070000 c5f057c9 c5fa100570165700 e9d1000000 c5fa10832c070000 c5fa58055f165700"
    cave = 0x1648800 - 0xed9d30  # from the pattern's start
    stub = bytes.fromhex("c5fa10932c070000")                                    # vmovss xmm2, [rbx+0x72c]
    stub += bytes.fromhex("c5ea5815") + struct.pack("<i", 0x40 - (len(stub) + 8))  # vaddss xmm2, xmm2, [cave+0x40]
    stub += bytes.fromhex("c5f057c9")                                           # vxorps xmm1, xmm1, xmm1
    stub += b"\xe9" + struct.pack("<i", 12 - (cave + len(stub) + 5))            # jmp back (pattern + 12)
    stub = stub.ljust(0x40, b"\xcc") + struct.pack("<ff", bias, -0.5 + bias)
    jump = b"\xe9" + struct.pack("<i", cave - 5) + b"\x90" * 7
    second = struct.pack("<i", (cave + 0x44) - (0x21 + 8))                       # disp of the 2nd path's add
    return (f"Uncharted 3 exposure {bias:+.2f} EV (UNCHARTED_U3_EXPOSURE)", [
        (pattern, [(0, bytes.fromhex("c5fa10932c070000c5f057c9"), jump),
                   (cave, bytes(len(stub)), stub),
                   (0x25, bytes.fromhex("5f165700"), second)]),
    ])


def apply_patches(image, patches):
    """Applies the patches to the linked image (the native patcher, launcher/native/patcher.cpp): prints what was
    patched; SystemExit when a patch's place holds other bytes (the image is left as it was)."""
    import game_patcher
    game_patcher.apply_patches(image, patches)


def game_executable():
    """The runtime as Uncharted.exe (Task Manager, Windows' graphics settings and overlays show
    this name), copied from the build output whenever that is newer."""
    built = ROOT / "gow3_pc/out/gow3-probe.exe"
    game_exe = ROOT / "gow3_pc/out/Uncharted.exe"
    if os.environ.get("UNCHARTED_TEST_PROBE_EXE") == "1":  # test: not under the game's name
        return built
    # Tests of another build (UNCHARTED_PROBE_EXE=<file>) run it as Uncharted.exe and leave the
    # deployed build alone; any difference to the deployed build (not only a newer one) is copied
    # back at the next normal start.
    if override := os.environ.get("UNCHARTED_PROBE_EXE"):
        built = Path(override)
    try:
        built_stat = built.stat()
        if not game_exe.exists() or built_stat.st_mtime != game_exe.stat().st_mtime or \
                built_stat.st_size != game_exe.stat().st_size:
            shutil.copy2(built, game_exe)
    except OSError as error:  # still running from a previous start: use the build output
        print(f"Using {built.name}: {error}")
        return built
    return game_exe


def game_window_icon(game):
    """The game's own icon (sce_sys/icon0.png) as a BMP for the window (SDL loads only BMP)."""
    icon = ROOT / "launcher/assets/window_icon.bmp"
    if icon.exists():
        return icon
    try:
        from PIL import Image
        icon.parent.mkdir(parents=True, exist_ok=True)
        Image.open(game / "sce_sys/icon0.png").convert("RGB").resize((64, 64)).save(icon)
        return icon
    except Exception as error:  # Pillow missing or no icon: the default icon stays
        print(f"No window icon: {error}")
        return None


# Render resolution (Drake's Fortune). The engine's display settings (constructors and the
# 1080p/720p mode switch, all within ~0x700 bytes) store the size in several fields: display
# and render size (+0x08/+0x18/+0x24/+0x38), aspect, tile grid (+0x40 = width/8, +0x44 =
# height/4, as the engine's own SetResolution computes them) and a larger size at +0x30/+0x34
# (2560x1216 at 1080p), scaled with the rest. UNCHARTED_RESOLUTION=<width>x<height>.
class ResolutionTooLarge(Exception):
    pass


# Drake's Fortune above what its GPU area holds: these 16:9 sizes in turn (width/8, height/4 whole).
U1_FALLBACK_SIZES = [(3200, 1800), (2880, 1620), (2560, 1440), (1920, 1080)]


def resolution_patch(data, width, height, move=True):
    """The render resolution in the linked image `data` (launcher/native/patcher.cpp: the display settings' stores,
    the screen wipe, the render target heaps and the memory total check). Returns how many MiB the memory areas
    grew; ResolutionTooLarge when the size doesn't fit (move=False: Drake's Fortune keeps its render targets in the
    GPU area), SystemExit when the game's code is not as expected."""
    import game_patcher
    return game_patcher.resolution_patch(data, width, height, move, ResolutionTooLarge)


GAME_KEYS = {"eboot.bin": "u1", "big2-ps4_Shipping.elf": "u2", "big3-ps4_Shipping.elf": "u3"}
UPDATE_SUFFIXES = ("-patch", "-update", "_patch", "_update")


def find_update(game):
    """The game's update (e.g. 1.02, extracted from its PKG): UNCHARTED_UPDATE (a folder, or 0 for
    none), else a folder next to the game's named like it plus -patch/-update (any case), with or
    without uroot. An update folder holds eboot.bin and sce_sys/param.sfo (launcher/native/system.cpp)."""
    import game_patcher
    found, message = (game_patcher.system("find_update", game) + "\n").split("\n")[:2]
    if message:
        print(message)
    return Path(found) if found else None


def sfo_value(game, key):
    """A value of the folder's sce_sys/param.sfo, None when unreadable (launcher/native/system.cpp)."""
    import game_patcher
    text = game_patcher.system("sfo", game)
    return json.loads(text).get(key) if text else None


def game_version(game):
    """APP_VER of the game folder; 01.02 when its executables are 1.02 although param.sfo says
    otherwise (dumps with the update merged in that kept the 1.00 param.sfo), 01.00 when param.sfo
    says 01.02 but the executables are 1.00 (GitHub #148, #176): Uncharted 2's torch code as 1.02
    compiled it and its 1.00 UpdateFocus code (launcher/native/system.cpp)."""
    import game_patcher
    return game_patcher.system("starter_version", game)


def game_view(game, update):
    """The game with its update laid over it, as a folder of links (gow3_pc/scripts/game_update.py).
    It is made on the game's drive when possible: file links can't cross drives (else copies)."""
    sys.path.insert(0, str(ROOT / "gow3_pc/scripts"))
    try:
        from game_update import create_view
    finally:
        sys.path.pop(0)
    places = [ROOT / "analysis/views"]
    if Path(game.anchor).resolve() != Path(ROOT.anchor).resolve():
        places.insert(0, game.parent.parent / ".uncharted-views")
    for place in places:
        try:
            return create_view(game, update, place)
        except OSError as error:
            print(f"Game view in {place} failed: {error}")
    raise SystemExit("Could not combine the game and its update")


def boot_view(base, update):
    """For drives that hold no links (exFAT, FAT32): copies of the files the prepare scripts read
    (the executables, sce_module, sce_sys), the update's over the game's, in analysis/views
    (about 90 MB, copied again only when a source changes). The runtime reads the update over
    the game itself (--app0-overlay)."""
    chosen = {}
    executables = {name.lower() for name in GAME_KEYS}
    for layer in (base, update):  # the update last: its files win
        for entry in layer.iterdir():
            if entry.is_file() and entry.name.lower() in executables:
                chosen[entry.name.lower()] = (entry.name, entry)
            elif entry.is_dir() and entry.name.lower() in ("sce_module", "sce_sys"):
                for path in entry.rglob("*"):
                    if path.is_file():
                        relative = path.relative_to(layer)
                        chosen[str(relative).lower()] = (str(relative), path)
    title, version = sfo_value(update, "TITLE_ID") or "game", sfo_value(update, "APP_VER") or "update"
    view = ROOT / "analysis/views" / f"boot-{title}-{version}"
    for relative, source in chosen.values():
        dest = view / relative
        wanted = source.stat()
        try:
            current = dest.stat()
            # FAT32 keeps write times in 2-second steps.
            if current.st_size == wanted.st_size and abs(current.st_mtime - wanted.st_mtime) <= 2:
                continue
        except OSError:
            pass
        dest.parent.mkdir(parents=True, exist_ok=True)
        # copyfile, not copy2: a read-only source attribute would make the next refresh fail.
        shutil.copyfile(source, dest)
        os.utime(dest, ns=(wanted.st_atime_ns, wanted.st_mtime_ns))
    return view


def run_once(args, executable, guest_args, loadexec):
    base = args.game.resolve()
    update = find_update(base)
    if not update:
        version = game_version(base)
        if version and version < "01.02":
            title = base.parent if base.name.lower() == "uroot" else base
            print(f"Game version {version} without update 1.02: the fixes written for 1.02 are skipped "
                  f"(the patches marked NOT applied). Extract update 1.02 next to the game folder as "
                  f"{title.name}-patch.")
        return run_game(args, base, executable, guest_args, loadexec)
    print(f"Update {sfo_value(update, 'APP_VER') or '?'}: {update}")
    if (update_version := game_version(update)) and update_version < "01.02":
        print(f"The update folder holds version {update_version} executables (its param.sfo says "
              f"otherwise): the fixes written for 1.02 are skipped. Extract update 1.02 again.")
    try:
        if os.environ.get("UNCHARTED_TEST_OVERLAY") == "1":  # test: the drive-without-links path
            raise SystemExit("UNCHARTED_TEST_OVERLAY=1")
        view = game_view(base, update)
    except SystemExit as error:
        # exFAT/FAT32 drives (USB sticks) hold no links ([WinError 1] Incorrect function): the
        # runtime lays the update over the game itself, and the prepare scripts read copies of
        # the few files they need.
        print(f"{error}: the runtime lays the update over the game instead (drive without links)")
        try:
            boot = boot_view(base, update)
        except OSError as copy_error:
            print(f"Update files not copied ({copy_error}): starting the game without its update")
            return run_game(args, base, executable, guest_args, loadexec)
        return run_game(args, boot, executable, guest_args, loadexec, app0=base, overlays=[update])
    try:
        return run_game(args, view, executable, guest_args, loadexec)
    finally:
        sys.path.insert(0, str(ROOT / "gow3_pc/scripts"))
        try:
            from game_update import remove_view
            remove_view(view)
        except OSError as error:
            print(f"Game view {view} not removed: {error}")
        finally:
            sys.path.pop(0)


def instance_suffix():
    """UNCHARTED_INSTANCE=<n> (n >= 2; co-op tests, 25 Oct 2026): another game on this PC with its
    own folders (analysis/<game>-i<n>, analysis/uncharted-user-i<n>), so that two instances can
    run side by side. Instance 1, the default,
    uses the normal folders."""
    import game_patcher
    return game_patcher.starter("instance_suffix")


# What a new instance's user folder starts with (copied once from the main one): settings, saves,
# trophies and the shader caches (an empty cache would compile for minutes).
INSTANCE_USER_SEED = ("probe.ini", "probe-u1.ini", "probe-u2.ini", "probe-u3.ini",
                      "disabled-optimizations.txt", "skip.txt", "savedata", "trophy", "cache", "shader")


def user_dir():
    """The user folder (saves, settings, caches) of this instance."""
    main_user = ROOT / "analysis/uncharted-user"
    suffix = instance_suffix()
    if not suffix:
        return main_user
    user = ROOT / f"analysis/uncharted-user{suffix}"
    if not user.exists():
        user.mkdir(parents=True)
        for name in INSTANCE_USER_SEED:
            source = main_user / name
            if source.is_dir():
                shutil.copytree(source, user / name)
            elif source.is_file():
                shutil.copy2(source, user / name)
        print(f"Instance {suffix[2:]}: new user folder {user} (settings, saves and caches copied)")
    return user


def u3mp_image(executable):
    """Uncharted 3 multiplayer, set by the launcher (Multiplayer tab, u3mp/patcher/u3mp_import.py):
    UNCHARTED_U3MP_IMAGE (the linked image), UNCHARTED_U3MP_IMAGE_SHA256 (its digest),
    UNCHARTED_U3MP_OVERLAYS (folders, os.pathsep between them, the first wins), UNCHARTED_U3MP_VERSION.
    Anything missing or a digest mismatch: None, and Uncharted 3 starts as usual."""
    import game_patcher
    message, _, found = game_patcher.starter("u3mp_image", executable).partition("\n")
    if message:
        print(message)
    if not found:
        return None
    info = json.loads(found)
    return {"image": Path(info["image"]), "overlays": [Path(p) for p in info["overlays"]], "version": info["version"]}


def image_patches(executable):
    """The launcher's code patches for this executable and the current settings (environment),
    in the order they are applied to the linked image (apply_patches)."""
    patches = list(PATCHES[executable])
    if executable == "big2-ps4_Shipping.elf" and os.environ.get("UNCHARTED_TORCH_FORWARD", "1") != "0":
        patches.extend(TORCH_FORWARD_PATCHES)
    if os.environ.get("UNCHARTED_AIM_RANGE", "1") != "0":
        patches.extend(AIM_RANGE_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_FRAME_WAIT", "1") != "0":
        patches.extend(FRAME_BEGIN_WAIT_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_UNLOCK", "0") == "1":
        patches.extend(UNLOCK_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_UNLOCK_REWARDS", "0") == "1":
        patches.extend(REWARD_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_FOCUS_RESET", "1") != "0":
        patches.extend(FOCUS_NAN_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_LEG_IK_RESET", "1") != "0":
        patches.extend(LEG_IK_NAN_PATCHES.get(executable, []))
    if executable in JOB_PARAMS_GUARD and os.environ.get("UNCHARTED_JOB_PARAMS_GUARD", "1") != "0":
        patches.append(job_params_guard_patch(executable))
    if executable in ("eboot.bin", "big2-ps4_Shipping.elf") and os.environ.get("UNCHARTED_MOVE_POOL", "1") != "0":
        patches.append(MOVE_ACTION_POOL_PATCH)
    if executable == "big2-ps4_Shipping.elf" and os.environ.get("UNCHARTED_CONVOY_SINK", "1") != "0":
        patches.append(CONVOY_SINK_PATCH)
    # Test knob for that guard (never in the launcher): Uncharted 3's creator at 0xf6da01 stores a
    # null descriptor (mov rcx, [rax+0x78] becomes xor ecx, ecx), so every such job gets the null
    # the players' crash had.
    if executable == "big3-ps4_Shipping.elf" and os.environ.get("UNCHARTED_TEST_JOB_PARAMS_NULL") == "1":
        patches.append(("Uncharted 3 TEST: null job descriptor (UNCHARTED_TEST_JOB_PARAMS_NULL)", [
            ("498b4500488b48784989042449894c2408813810", 4, bytes.fromhex("488b4878"),
             bytes.fromhex("31c99090")),
        ]))
    if executable == "big3-ps4_Shipping.elf":
        if os.environ.get("UNCHARTED_LIGHT_ORTHO_GUARD", "1") != "0":
            patches.append(u3_light_ortho_guard_patch())
        if os.environ.get("UNCHARTED_TEST_LIGHT_ORTHO_NAN") == "1":
            patches.extend([U3_TEST_LIGHT_ORTHO_NAN, U3_TEST_LIGHT_ORTHO_GATE])
    if os.environ.get("UNCHARTED_CUE_PRETOUCH", "1") != "0":
        patches.extend(CUE_PRETOUCH_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_RUN_GROUP_FIX", "1") != "0":
        patches.append(RUN_GROUP_PATCH)
    for cheat in cheat_names(executable):
        patches.extend(CHEAT_PATCHES.get(executable, {}).get(cheat, []))
    # Ultrawide render sizes (the launcher's aspect ratio): the wider view (HOR_PLUS_PATCHES).
    size = (os.environ.get(f"UNCHARTED_RESOLUTION_{GAME_KEYS[executable].upper()}")
            or os.environ.get("UNCHARTED_RESOLUTION", "")).lower()
    if "x" in size and os.environ.get("UNCHARTED_HOR_PLUS", "1") != "0":
        size_w, size_h = (int(value) for value in size.split("x"))
        if size_w / size_h > 16 / 9 + 0.01:
            patches.extend(hor_plus_patches(executable))
    if executable == "big3-ps4_Shipping.elf" and (bias := u3_exposure_bias()):
        patches.append(u3_exposure_patch(bias))
    if workers := job_worker_count():
        patches.extend(job_worker_patches(executable, workers))
    if job_sleep_enabled(executable):
        patches.extend(JOB_SLEEP_PATCHES)
        # Diagnostics: UNCHARTED_JOB_SLEEP_SPINS=<n> idle spins before a worker sleeps (1000).
        if spins := os.environ.get("UNCHARTED_JOB_SLEEP_SPINS", "").strip():
            patches.append(("idle job workers: spins before sleeping", [
                ("4d8d7001 807f5900 ???? 4981f8e8030000 7c??", 13, bytes.fromhex("e8030000"),
                 int(spins).to_bytes(4, "little")),
            ]))
    return patches


def run_game(args, game, executable, guest_args, loadexec, app0=None, overlays=()):
    """game: the folder the prepare scripts read; app0 (default: game) with overlays laid over
    it is what the game sees as /app0."""
    out = ROOT / "analysis" / (("uncharted" if executable == "eboot.bin" else Path(executable).stem)
                               + instance_suffix())
    user = user_dir()
    user.mkdir(parents=True, exist_ok=True)
    scripts = ROOT / "gow3_pc/scripts"
    for script in ("prepare.py", "link_modules.py", "content_profile.py"):
        prepare_command = [sys.executable, str(scripts / script), str(game), "--out", str(out)]
        if script != "content_profile.py":
            prepare_command += ["--executable", executable]
        subprocess.run(prepare_command, check=True)
    mp = u3mp_image(executable)
    if mp:
        # Uncharted 3 multiplayer (launcher -> Multiplayer): the image the launcher's import linked on
        # this PC replaces the prepared one. It already holds the port's code patches it was tested
        # with, so the patches are not applied again (some would not find their bytes).
        shutil.copyfile(mp["image"], out / "boot-linked.bin")
        print(f"Uncharted 3 multiplayer v{mp['version']}: {mp['image']} with {len(mp['overlays'])} overlays")
    else:
        apply_patches(out / "boot-linked.bin", image_patches(executable))
    # Settings per game (the launcher keeps them apart): the started game's own, also when the
    # Collection menu switches games.
    game_key = GAME_KEYS[executable]
    resolution = (os.environ.get(f"UNCHARTED_RESOLUTION_{game_key.upper()}")
                  or os.environ.get("UNCHARTED_RESOLUTION", "")).lower()
    width, height = (int(value) for value in resolution.split("x")) if resolution else (1920, 1080)
    render_size = (1920, 1080)
    if (width, height) != (1920, 1080):
        # (The earlier Uncharted 3 failures above 1080p - "Command context dcb full", a corrupted
        # pointer while loading - came with asynchronous readbacks, which also broke it at 1080p.)
        image = out / "boot-linked.bin"
        data = bytearray(image.read_bytes())
        # Drake's Fortune keeps its render targets in the GPU area (see resolution_patch): above
        # ~1620p it renders at the largest size that fits there, and the output is upscaled.
        move = executable != "eboot.bin"
        candidates = [(width, height)] + [size for size in U1_FALLBACK_SIZES if size[0] * size[1] < width * height]
        if abs(width / height - 16 / 9) > 0.01:
            # Ultrawide: smaller sizes of the same shape (width/16 and height/4 whole: Uncharted 3
            # asserts widths in steps of 16 in its post processing).
            candidates = [(width, height)] + [
                (int(width * scale) // 16 * 16, int(height * scale) // 4 * 4)
                for scale in (0.9, 0.8, 0.75, 2 / 3, 0.6, 0.5)]
        extra_mib = 0
        for candidate in candidates:
            patched = bytearray(data)
            try:
                extra_mib = resolution_patch(patched, *candidate, move=move)
            except ResolutionTooLarge as error:
                print(f"Render resolution {error}")
                continue
            except SystemExit as error:
                if not mp:
                    raise
                # The multiplayer image keeps the PS4's render size if the patch does not fit it.
                print(f"Uncharted 3 multiplayer: render resolution not changed ({error})")
                break
            if candidate != (width, height):
                print(f"Drake's Fortune renders at {candidate[0]}x{candidate[1]} instead of {width}x{height}")
            data, render_size = patched, candidate
            break
        image.write_bytes(data)
    else:
        extra_mib = 0
    # The runtime's options (GOW3_* environment values) from the launcher's (UNCHARTED_*), the game and its render
    # size: cheats and extra quality, renderer and readback settings, the controls, upscaling, diagnostics and the
    # trigger files (launcher/native/runtime_env.cpp, which keeps the reasons for each).
    import game_patcher
    env, capture, trigger = game_patcher.runtime_env(executable, game, game_key, user, out, render_size, (width, height),
                                                     extra_mib, loadexec, args.capture)
    window_icon = game_window_icon(game)
    if window_icon:
        env.setdefault("GOW3_WINDOW_ICON", str(window_icon))
    command = [str(game_executable()), str(out / "boot-linked.bin"),
               "--no-game-hooks", "--low-image", "--content-profile", str(out / "content.bin"),
               "--app0", str(app0 or game), "--user", str(user)]
    # Uncharted 2 multiplayer (u2mp, set by the launcher): UNCHARTED_U2MP_OVERLAY, a folder made from the
    # player's own game (u2mp/patcher/u2mp_overlay.py), laid over /app0 ahead of the update.
    # os.pathsep between folders (the menu overlay, then the PS3 import); the first holding a file wins.
    if executable == "big2-ps4_Shipping.elf":
        for u2mp in os.environ.get("UNCHARTED_U2MP_OVERLAY", "").split(os.pathsep):
            if u2mp and Path(u2mp).is_dir():
                command += ["--app0-overlay", u2mp]
    # Uncharted 3 multiplayer: its four overlays (the first wins) ahead of the update.
    for overlay in [*(mp["overlays"] if mp else ()), *overlays]:
        command += ["--app0-overlay", str(overlay)]
    # guest_args is the game's complete argv; argv[0] is the program name.
    command += ["--guest-path", guest_args[0] if guest_args else "/app0/" + executable]
    for value in guest_args[1:]:
        command += ["--guest-arg", value]
    # Diagnostics: UNCHARTED_RENDERDOC=<RenderDoc folder or renderdoccmd.exe> loads RenderDoc's
    # Vulkan layer (its capture library) into the game: F12 captures a frame (RenderDoc's default
    # folder, %TEMP%\RenderDoc). Found through this process's environment only; nothing is
    # registered, and the game's log stays as it is.
    renderdoc = os.environ.get("UNCHARTED_RENDERDOC")
    if renderdoc:
        layer_dir = Path(renderdoc)
        layer_dir = layer_dir if layer_dir.is_dir() else layer_dir.parent
        if (layer_dir / "renderdoc.json").is_file():
            env["VK_ADD_LAYER_PATH"] = str(layer_dir) + (os.pathsep + env["VK_ADD_LAYER_PATH"]
                                                         if env.get("VK_ADD_LAYER_PATH") else "")
            env["VK_INSTANCE_LAYERS"] = "VK_LAYER_RENDERDOC_Capture"
            env["ENABLE_VULKAN_RENDERDOC_CAPTURE"] = "1"
            env["GOW3_PIPELINE_BINARIES"] = "0"
            print(f"RenderDoc: Vulkan layer from {layer_dir}")
    if not args.boot:
        command.append("--check-imports")
    if args.cpu_only:
        command.append("--cpu-only")
    name = "boot" if args.boot else "check-imports"
    if args.cpu_only:
        name += "-cpu"
    result = {"executable": executable, "command": command, "guest_execution_requested": args.boot, "timeout_seconds": args.seconds}
    if args.capture:
        result["capture_directory"]=str(capture)
    start = time.monotonic()
    with (out / (name + ".log")).open("wb") as log:
        process = None
        if mp and os.name == "nt":
            # As the multiplayer runner starts it: ASLR off for this child (tools/u3mp_process.py).
            try:
                from u3mp_process import Popen as U3mpPopen
                process = U3mpPopen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
            except (OSError, RuntimeError, ValueError) as error:
                print(f"Uncharted 3 multiplayer: started without its address layout ({error})")
        if process is None:
            process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        # Freeze watchdog (needs the Frame stats lines, i.e. diagnostics on). UNCHARTED_HANG_SECONDS
        # sets its wait (30 s), 0 turns it off.
        hang_seconds = int(os.environ.get("UNCHARTED_HANG_SECONDS", "30") or 0)
        # The watchdog (launcher/native/hang.cpp, tools/hang_report.py): the runtime logs a Frame stats line every 5 s
        # while frames are shown; none for that long while the game runs writes every thread's stack to
        # hang-<time>.txt beside the log (small enough to attach to an issue) and, in the source tree or with
        # UNCHARTED_HANG_DUMP=1, a minidump to dumps/ (the newest 2 stay). Once per freeze; frames re-arm it.
        watchdog = 0
        if os.name == "nt" and args.boot and hang_seconds > 0 and env.get("GOW3_FRAME_STATS") == "1":
            import hang_report
            watchdog = hang_report.hang_watchdog(process, out / (name + ".log"), out / "dumps", hang_seconds, ROOT)
        # Resource tracking for long sessions (memory leaks; launcher/native/memstat.cpp): memory-<time>.csv beside
        # the log, one row every UNCHARTED_MEMORY_SECONDS (10; 0 turns it off); each session's goes with its attempt.
        memory_csv = None
        memory_seconds = float(os.environ.get("UNCHARTED_MEMORY_SECONDS", "10") or 0)
        if os.name == "nt" and args.boot and memory_seconds > 0:
            import hang_report
            memory_csv = hang_report.memory_monitor(process, out, memory_seconds)
        try:
            if args.capture:
                while process.poll() is None:
                    remaining=args.seconds-(time.monotonic()-start) if args.seconds else 3
                    if remaining<=0:
                        raise subprocess.TimeoutExpired(command,args.seconds)
                    try:
                        process.wait(timeout=min(3,remaining))
                    except subprocess.TimeoutExpired:
                        trigger.touch()
                result["exit_code"]=process.returncode
            else:
                result["exit_code"] = process.wait(timeout=args.seconds or None)
            result["timed_out"] = False
        except subprocess.TimeoutExpired:
            process.kill()
            result["exit_code"] = process.wait()
            result["timed_out"] = True
    result["elapsed_seconds"] = round(time.monotonic() - start, 3)
    (out / (name + ".json")).write_text(json.dumps(result, indent=2), encoding="utf-8")
    # The game has ended: the log can take the freeze reports' names (the launcher looks there).
    hang_reports = []
    if args.boot and watchdog:
        import hang_report
        hang_reports = hang_report.hang_reports(watchdog)
    if args.boot and hang_reports:
        with open(out / (name + ".log"), "ab") as log:
            log.write("".join(f"\nHang watchdog: the game froze; report {report}" for report in hang_reports)
                      .encode("utf-8") + b"\n")
    attempt = out / "attempts" / (name + "-" + str(time.time_ns()))
    attempt.mkdir(parents=True)
    for suffix in (".log", ".json"):
        shutil.copy2(out / (name + suffix), attempt / (name + suffix))
    for report in hang_reports if args.boot else []:
        shutil.copy2(report, attempt / report.name)
    if args.boot and memory_csv is not None and memory_csv.exists():
        shutil.copy2(memory_csv, attempt / memory_csv.name)
    print(json.dumps(result, indent=2))
    print(f"Log: {out / (name + '.log')}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=ROOT / "game/CUSA02344/uroot")
    parser.add_argument("--boot", action="store_true", help="Execute guest code after preparation")
    parser.add_argument("--executable", choices=EXECUTABLES, default="eboot.bin")
    parser.add_argument("--guest-arg", action="append", default=None,
                        help="The game's argv, program name first (repeatable). A game started directly gets "
                             "the Collection menu's argv: eboot.bin ispartofuccollection")
    parser.add_argument("--collection-menu", action="store_true",
                        help="Start eboot.bin with the Collection's game selection instead of Drake's Fortune")
    parser.add_argument("--cpu-only", action="store_true")
    parser.add_argument("--capture", action="store_true", help="Request a renderer frame dump every three seconds")
    parser.add_argument("--seconds", type=int, default=30, help="External process time limit; 0 disables it")
    args = parser.parse_args()
    if args.seconds < 0:
        parser.error("--seconds must be nonnegative")
    user = user_dir()
    user.mkdir(parents=True, exist_ok=True)
    loadexec = user / "loadexec.txt"
    executable = args.executable
    guest_args = args.guest_arg
    if guest_args is None:
        # As the Collection menu starts each game: "ispartofuccollection" makes eboot.bin start
        # Drake's Fortune itself (without it, it shows the Collection's game selection, whose
        # cursor is on the last game played: the U1 card started U2/U3 after one press of Cross).
        if executable == "eboot.bin" and args.collection_menu:
            guest_args = ["/app0/eboot.bin"]
        else:
            guest_args = ["eboot.bin", "ispartofuccollection"]
    while True:
        loadexec.unlink(missing_ok=True)
        result = run_once(args, executable, guest_args, loadexec)
        if not args.boot or result["timed_out"] or result["exit_code"] != LOADEXEC_EXIT or not loadexec.exists():
            break
        # The Collection switched games (menu -> Uncharted 2/3, or back to the menu).
        lines = loadexec.read_text(encoding="utf-8", errors="replace").splitlines()
        target = Path(lines[0].strip()).name if lines else ""
        if target not in PATCHES:
            print(f"Game asked to start {lines[0] if lines else '(nothing)'}; not part of the Collection")
            break
        # LoadExec's argv is the new program's complete argv (the menu passes "eboot.bin" as argv[0]
        # for Uncharted 2/3; an extra leading argument makes them skip the menu and load nothing).
        executable, guest_args = target, lines[1:] or ["/app0/" + target]
        print(f"Collection: switching to {executable} {' '.join(guest_args)}")
    return 124 if result["timed_out"] else result["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
