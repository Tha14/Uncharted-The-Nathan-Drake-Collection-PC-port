# Uncharted compatibility work — 13 October 2026

Target: CUSA02344 v01.00, eboot.bin (Drake’s Fortune).

The game boots through the Collection menu and opening movie into gameplay.
The user confirms normal controls and working audio after the clock correction.
3D appeared briefly, but movement led to GPU device loss. Work is incomplete.

Current investigation: dynamic shader resource reads. The GOW3 default disables
GPU direct memory access and substitutes flat-buffer data for unflattened reads.
The latest run enables GOW3_DIRECT_MEMORY_ACCESS=1; outcome is not yet verified.

Implemented: package/metadata extraction; three-executable preparation; full
fiber context switching; FFmpeg Videodec2; memory range names; dynamic GPU hook
queue; installed PlayGo metadata; disk space; offline services; concurrent event
queue lifetime safety; interrupt registry; optional executable placement below
2 GiB; buffered controller reports; optional native CPU clock consistency.

Clock tests passed in both modes. This CPU runs at about 4.3 GHz, whereas the
base runtime reports 1 GHz. Uncharted executes inline RDTSC instructions; the
launcher now measures and reports the matching hardware frequency.

Shader changes: support uniform ReadFirstLane resource offsets, balanced walker
stack on failed pointer offsets, bounded GPU semaphore diagnostics with explicit
error handling. Dynamic Phi/lane-dependent resource offsets remain unsupported
by the CPU walker; the GPU memory-read path is being tested.

Settings: vsync and 60 FPS; background shader compilation, draw pipeline, Vulkan
recording threads, draw preparation, prepared resources, deferred stream copies
and parallel copies disabled during diagnosis.

Launch: ../Start-Uncharted.cmd → tools/probe_uncharted.py →
gow3_pc/out/gow3-probe.exe. Game data: game/CUSA02344/uroot. No timeout.

Native tests passed for fibers, memory names/notification ordering, services,
content, concurrent handles, video decode, buffered pad and clock timing.
Windows build passes. Uncharted 2/3 gameplay has not been tested.

Logs, attempts and frame captures: uncharted/. Thread snapshot: thread-sample.json.
Older import counts predate later API implementations. REA opened the reconstructed
ELF, but decompilation was unavailable with the installed Windows providers.

## Update 13 October 2026 (evening): gameplay reached

The device loss on the way into gameplay (menu blur / loading, cutscene skip) is fixed.
Cause: TextureCache::FindDepthTarget linked any image starting at the depth buffer's
stencil address as its stencil proxy. Uncharted reuses that memory for a 960x540 RGBA8
texture; sampling it was redirected to the 960x540 D32S8 depth image and the GPU read past
that image's memory (VK_EXT_device_fault: ReadInvalid page fault). Only 8-bit images or the
format-less placeholder are now linked, and GetAssociatedDepth drops stale links.
The user confirmed gameplay after the fix.

Also fixed: evicted samplers are destroyed only after the GPU passed them (deferred).

Diagnostics (GOW3_GPU_CHECKPOINTS=1, set by tools/probe_uncharted.py; =2 adds
CommandBuffer() caller markers): on device loss the log shows the device fault report,
the draws/dispatches with their bound images and buffers after the last completed marker,
and which Vulkan object/guest image owns the fault address (device address binding report
plus image/buffer placement records). Diagnostic-only switches: GOW3_KEEP_FREED_IMAGES=1,
GOW3_FEEDBACK_LOOP_LAYOUT=0. The probe still disables several GOW3 performance features.

## Update 14 October 2026

- FrameBegin assertion (fg-draw-mgr.cpp:234, m_gfxEopTick of frame N-3): the game's wait for a
  frame's GPU end-of-pipe stamp (0x3fd6xx) gives up after 2.0 s. tools/probe_uncharted.py now
  NOPs that timeout (`ja` at 0x3fd816) in boot-linked.bin by unique pattern (EBOOT_PATCHES).
- Lighting: tiled lighting compute cs 0x43b8ee5e indexes 224-byte light records with a
  GPU-computed index (waterfall loop, v_readfirstlane); the SRT walker cannot pre-read them.
  GOW3_DIRECT_MEMORY_ACCESS=1 is now the probe default. Its per-light shadow texture
  descriptors (8 "not flatenned") remain unresolved.
- Jungle chapter (temple-0a/0b) once showed a grey world (level geometry, shadows and sky
  probe computes not rendered; Nate black) until a treasure pickup; all packs were read
  completely, PlayGo reports all 100 chunks available. After the new save it loads fine.
  Cause not established; watch for it on new chapter loads.
- Pipeline cache and background shader compilation are on again in the probe; GOW3 stale and
  deferred readbacks are off in analysis/uncharted-user/probe.ini.
- Diagnostics added: GOW3_FILE_TRACE=<text> (timestamped opens/reads), GOW3_CAPTURE_PASSES /
  number in the capture trigger file (frame capture of N passes), GOW3_SKIP_FILE (live list of
  shader hashes to skip), shader hashes in SRT/sharp warnings.
- Audio output timing is clean (no starvation); latency ~83 ms queued plus device buffer.
  Gunfire audio missing and black circles on bullet hits are still open.


## Diagnostic update 14 October 2026 (01:xx)

User-confirmed gunfire decrements ammunition but remains silent. Recorded input PCM on all eight main channels is zero during shots, before volume/downmix; port gains are nonzero, sound category gains are 1, and output never starves. Common/temple sound banks open and read successfully. Missing effects occur before SDL output conversion.

Shader compilation disabled and low renderer cache optimizations disabled did not recover the grey scene (mask restored to 159760). Native process counter now shares TSC units/frequency; clock tests pass, but user confirms this did not fix either symptom.

Compared Supermedo/bloodborne_pc windows commit e5d04e2 (see bloodborne-comparison.md). Ported cached SRT walker handler registration and speculative/concurrent fault safety, preserving local locking and balanced failed PushPtr. Build passes; scene remained grey. Shader binary/meta cache versions bumped 7 to 8 so earlier compiler output cannot silently survive updates. Latest test disables both pipeline and binary cache for fresh compilation.

One-shot guest image diagnostics added to Presenter: guest-images.trigger in GOW3_DUMP_DIR lists hex address and guest size; copies depth/stencil or uncompressed color. Only activates on frame-dump trigger. Intermediate capture shows depth containing props/characters, normals containing additional geometry, two lighting targets RGB zero. Capture files in uncharted/captures/1791923083373662700. New capture notes include precise per-draw depth/stencil flags. Root cause still open.

Latest follow-up: fresh shader test (pipeline and binary cache both disabled) still grey. Depth/stencil flags and stencil plane captured under uncharted/captures/1791923397807259500, pass notes in uncharted-user/captures/frame_20261014-013133_1.txt. Stencil has values 0/128/192/194/208/210, not entirely zero. Lighting output addresses rotate; cs43b8ee5e writes 0x11438f0000 in this capture, so zero 0x11408b0000 alone does not prove that compute failed. Presenter captures may observe a subsequent in-flight frame's depth; need capture at the specific rendering stage before attributing missing geometry to depth.

Corrected CLOCK_PROCTIME (ID15) to elapsed time since process start, rather than accumulated host CPU time. Regression tests pass default/native. Trace currently observes clock ID4 only; no evidence that ID15 caused current symptoms. Latest run session15495, capture folder1791923766600283800, still grey. Exe gow3_pc/out/gow3-probe.exe, launcher Start-Uncharted.cmd. No goal completion claimed.

## Update 14 October 2026 (later): fixes and the open "grey world" investigation

Fixed (in the build):
- Event queues (gpu/shadps4/core/libraries/kernel/equeue.cpp): the poll path (timeout 0)
  read and cleared triggered events without the queue lock, losing a concurrent trigger;
  waits with a pending small HR timer ignored other triggered events; TriggerEvent woke one
  waiter only (now notify_all).
- Shader cache key vs DMA: shaders compiled with GOW3_DIRECT_MEMORY_ACCESS=0 stay in the
  pipeline cache; the old cache was moved to analysis/uncharted-user/cache-before-1014.
- Evicted samplers are destroyed after the GPU passed them (texture_cache.cpp).
- Audio latency: queue target 1-2 periods (GOW3_AUDIO_QUEUE_BUFFERS, default 1) and a 480-frame
  WASAPI buffer request (GOW3_AUDIO_DEVICE_FRAMES): ~41 ms queued instead of ~83 ms, no starvation.

Diagnostics added:
- GOW3_FILE_TRACE=<text>: timestamped opens/reads/seeks; every failed open is now logged
  (errno, file table full, path translation).
- GOW3_AUDIO_TRACE=1: Ajm instance create/destroy always, RIFF jobs, 5 s decoder totals;
  sound1/ paths traced.
- GOW3_RETEXTURE_FILE=<file>: creating the file marks all CPU-sourced textures for re-upload.
- GOW3_SKIP_FILE, GOW3_CAPTURE_PASSES / pass count in the capture trigger file.
- Scratchpad tools (memory peek/watch/snapshot) read game globals at guest base 0xb400000.

Grey/black world after loading the jungle (intermittent, reproduced via main menu -> Continue):
- Workaround: Pause -> Treasures -> back (a plain pause or other menus do not help).
- The game renders reduced material variants: no sun shadow pass, no level G-buffer pass,
  no sky/ambient computes (cs 2a2e99be, 0a57ac2f, 43225bd9); ground flat or missing, Nate
  sometimes unlit.
- Texture streaming (texturedict1/*-tstrm.pak) reads stop right after the load and resume in
  a burst when Treasures opens. The streamer object (0xe66800) stops changing; its update gate
  flag (0x994188+0x39b7) stays 1.
- Ruled out: failed file I/O (all reads succeed), PlayGo (all 100 chunks available,
  0xd30df0), package loading (all 74 packages status 12, manager 0xd1b988), stale GPU
  textures (forced re-upload of 447 textures: no change), readback mode (Precise: no change),
  occlusion queries (always visible), Windows red-zone clobbering (measured intact on the
  Ryzen 9 9950X3D), SRT/DMA lighting.
- Correlated: the menu host (0xabb148) page stack top index (+0x790, pages at +0x590) is 2 in
  the broken state and 0 after the Treasures fix. Forcing it to 0 did not fix the world, so it
  is a symptom of the same condition.

Audio: only music plays in the broken state (level-driven sounds absent). Sound banks
(common.bnk etc., mono ATRAC9 RIFF) are opened; no bank voice reaches Ajm. Gunfire was the
only missing sound when the level loaded correctly.


## Grey/black ground investigation, 14 October 2026 (afternoon)

Ruled out (bad Continue load compared with the same load after Pause -> Treasures -> back):
- Texture streamer gates: manager 0xd60590 (dispatcher 0x3dfb90 registered at 0xe66710,
  mode 2 path 0x3e1b90). Enable byte +0xf1530 = 1, pending-op ring +0xf1558/+0xf1560 empty,
  streaming-state arg null. Needed-set +0x8c0 empty and request slots (0xe66800, stride 0x88,
  state +0x50) idle in good and bad loads alike, so the stall is a symptom (nothing is drawn,
  so nothing is requested).
- Rendered-background list (UpdateBackgroundsForRendering 0x3aeb10, manager 0xfdb5f0: list at
  +0x0, count +0x180): the same 4 level backgrounds in good and bad loads. Their objects and
  all objects they point to are unchanged by the Treasures fix (only a camera-following
  transform differs; +0x80 is a "transform changed" flag, set at 0x3afdcb).
- Debug flag "Draw Levels Outside Vis" (0x98f8e0+0xc5) forced on: ground still black.
- FgDrawMgr (0xd5c500) slot bitsets: no difference. Thread stacks: no stuck thread.
- The Treasures screen registers a treasure model with FgDrawMgr (0x3c65a0) and loads two
  packages (package manager 0xd1b988 +0x100/+0x168 go 7 -> 9).

In the bad load, grass, rocks and characters draw but the ground under them is black, and
the fix is only partial (large black ground areas remain). Next step: the frame capture now
records indirect draws and indirect dispatches, with arguments as the CPU sees them
(vk_rasterizer.cpp DrawIndirectRecord/DispatchIndirectRecord); earlier captures omitted
them, so "no level G-buffer pass" may have been indirect draws that were not recorded.
Launch with GOW3_CAPTURE_TRIGGER=<file> (write a pass count into the file) or
GOW3_FRAME_CAPTURE_KEY=1 (F11).
Tools: scratchpad heapsnap.py (full guest memory save/diff in ~5 s), threads.py (guest stack
sampler), bgdump.py.

### Capture comparison, same load, bad vs after Treasures fix (10:40 / 10:42)
Captures: uncharted-user/captures/frame_20261014-104041_1.txt (bad) and
frame_20261014-104215_2.txt (fixed), 600 passes each (GOW3_CAPTURE_TRIGGER with a pass count).
- No indirect draws or dispatches at all (new capture notes would show them).
- Both frames issue the same passes and draws (7602 vs 7612 draws, 78 computes, same shaders).
  The ground IS drawn in the bad state; it renders black. Earlier "no level G-buffer pass"
  conclusion is wrong for this repro.
- Frame structure: depth prepass interleaved with one-draw depth-only test passes (ps d3cd03d8 /
  bb6ab1e5, sampling an R8 64x64) = occlusion-query boxes; ZPASS_DONE results are synthesized
  as "visible" (liverpool.cpp EventWrite), SET_PREDICATION is ignored.
- Only sampled-texture difference: 4 streamed textures. Bad: 0x11547a0000 BC5 512, 0x1155038000
  BC1 sRGB 512, 0x1155064000 BC1 512, 0x115853c000 BC1 64. Fixed: 1024x1024 versions at new
  addresses (0x115ef84000, 0x115f938000, 0x11603bc000, 0x1164578000, 0x116478c000).
- Working hypothesis: the low-mip ground textures hold stale/black content in the texture cache
  (filled by a path the cache does not observe, e.g. GPU/DMA copy, or marked GpuModified so the
  forced re-upload skipped them); the Treasures fix streams in higher mips at new addresses,
  which are created fresh. Next: dump guest bytes at those addresses vs cached image contents.


### Review and texture evidence, 14 October 2026 (10:50)

Reviewed the 10:40/10:42 comparison. Important limitation: vk_frame_capture.cpp
AddSampled keeps only the first 32 unique sampled/storage textures per pass;
AddShader keeps only 8 shader hashes. Thus "only sampled-texture difference: 4"
is a bounded observation, not a complete comparison of all materials/shaders.

Added one-shot BC1/BC5 cached-image dumps (base mip, layer 0) to the existing
Presenter guest-images.trigger diagnostic, plus guest layout/flags metadata.
It does NOT call UpdateImage, avoiding an accidental refresh before capture.
Build passed. Latest running capture folder: uncharted/captures/1791956845059173200,
pass capture uncharted-user/captures/frame_20261014-104946_2.txt. This save resumes
near a rock arch and still has missing/grey terrain and black Nate.

Actual decoded GPU images for the corresponding four current-run addresses:
- 0x115478c000 BC5 512: pistol normal map.
- 0x1155024000 BC1 sRGB 512: pistol colour/material atlas.
- 0x1155050000 BC1 512: pistol material mask/atlas.
- 0x115852c000 BC1 64: small pistol-associated mask (sampled alongside the above).
The former 10:40 addresses differ by allocation placement in this run. These
captures do not support attributing those four texture changes to the ground.
See texture-comparison.png (GPU left, CPU micro-detile interpretation right).

Read-only guest snapshots: tools/snapshot_guest_textures.py <capture-directory>.
Comparing base-mip compressed-block multisets removes ordering/tiling as a source
of false mismatches: the four suspect textures differ from current CPU bytes;
other captured textures match exactly (0x11549f4000, 0x1154a00000, 0x1154ecc000).
All nine captured images have flags 0x40 (Registered, no Dirty/GpuModified/Aliased).
GPU pistol images are recognizable and nonblack; CPU bytes may have been reused
or overwritten. A mismatch is observed, but does not yet establish which copy is
correct or explain the missing ground. Do NOT force arbitrary CPU re-uploads as
a fix based on this comparison alone.

Earlier opt-in GOW3_DEPTH_PROBE_FILE diagnostic remains available (0 guest,
1 Equal->LessOrEqual, 2 Equal->Always, only draws without depth writes). It has
NOT been tested in gameplay and is not enabled in this run. No graphics or audio
fix is claimed from this review.

### Main-pass buffer comparison, 14 October 2026 (11:07, reviewed later)
analysis/bad-mainpass.txt vs good-mainpass.txt (pass #154, 795 draws, same shaders):
- Of 17 recorded buffers, exactly one differs zero/non-zero: the first draw (vs da3d9c98 /
  ps 45d0299c) slot 1, 42616 bytes, 28-byte vertex stride (positions about -8..9). Bad: all
  zeros (0x11009cd520). Good: filled (0x1100ecd520).
- The same draw's slot 0 constants [158..170] are zero in bad, and in good hold
  1,1,1, 107/255,88/255,73/255, 5,25,4,60, 0.2,0,1 (looks like a colour plus parameters).
- The vertex data is not stored verbatim in temple-0*/common packs, so it is generated per frame.
- tools/scan_ground_state.py (read-only) searches live memory for both patterns -> analysis/ground-scan.log.

### Ported from the MediEvil port (reference/medievil, fork base ce1886a), 14 October 2026 17:28
- (reverted after the 17:47 Fios EIO crash) runtime_mutex.c retirement of destroyed mutexes/conds;
  a destroy of a held mutex returns EBUSY.
- Depth bias constant scaled by PA_SU_POLY_OFFSET_DB_FMT_CNTL (regs.h 0x2DE) vs the host depth
  format; the log prints "GPU: depth bias ... scaled" once if it ever applies.
Build passes. No mutex unit test exists. Not relevant to the grey ground: MediEvil's CMask fill
(Uncharted never logs "Unhandled metadata") and memory-pool APIs (not imported by Uncharted).

## Grey world: cause found, fix pending test (14 October 2026, evening)
Live bad state (after main menu -> Continue): menu host 0xabb148 page stack top (+0x790) = 2,
stack = slot +0x3b0, slot +0x3c0, slot +0x448. +0x448 (and its twin +0x440) is the one-button
message page; descriptor host+0x8f0 = message 0x155d235e, button 0xc6467f2d, action 0x2dac.
Code (llvm-objdump of analysis/uncharted/eboot.elf; REA cannot open this ELF on Windows):
- Menu step 0x14a8b0 (also 0x149698, 0x14a986): if 0x577b60(&0x11a69d0) is false, toggle
  0xabbaf4 and push message 0x155d235e on page +0x448 or +0x440 (alternating). OK (0x2dac)
  goes to menu state 6, the same as the "ready" path (0x14aa2c).
- 0x577b60: true if flag 0x994188+0x3a04 == 0 (a general flag, read by the renderer too),
  otherwise object state (+8) not 0 and not 2. Object 0x11a69d0 is the PSN TUS client
  (its state machine near 0x578000 calls sceNpTus*/np_signed_out through GOT 0x9875d0..0x987608).
  Signed out, state stays 0.
- The message page stays on the stack in gameplay, so menu overrides persist: level sounds muted,
  level lighting constants zero (ps 45d0299c [158..170]), ground vertex buffer empty.
  Pause -> Treasures -> back clears the stack.
Fix: tools/probe_uncharted.py EBOOT_PATCHES makes 0x577b60 return true (b0 01 c3). Other callers
(0x149083, 0x14ca5c, 0x14d160) also require +0x28c4 ("TUS data loaded"), which stays 0.
Not yet tested in game. Test: Continue at least 3 times from the main menu; check the ground,
level sounds, and that the page stack top (image 0xabb8d8) is 0.
- 17:47 run with the PSN patch plus the MediEvil lock port crashed during the temple-0a -> 0b load:
  ASSERTION ErrFromFios (ndlib/io/streaming.cpp:523), SCE_KERNEL_ERROR_EIO, fault 0x395ac2.
  First occurrence. The lock port is reverted: its cond_destroy always destroyed even with waiters,
  while the PS4 returns EBUSY there, so a Fios wakeup could be lost. Depth-bias port and PSN
  patch kept. Rebuilt 17:5x.
- PSN patch tested 17:5x: popup gone, world still grey. Menu stack then [+0x3b0, +0x3c0], top 1.
  The menu host mode (+0x0; 1 = front end, 2 = in-game; IsFrontEnd 0x14adf0 = mode < 2) is 1 on bad
  loads and 2 after Treasures (0x14b2e0 opens the in-game menu: SetMode 0x148630(2) and replaces the
  stack with slot +0x468). Writing mode 1 into a good state changed nothing visible, so it's a symptom.
  PSN patch reverted.
- Key evidence (same session, 11:06:15 bad vs 11:06:59 after Treasures): the ground draws (vs
  823cb43a pass #137, vs da3d9c98 pass #154) read a 42616-byte vertex buffer that is all zeros in
  guest memory at 0x11009cd520. After Treasures the same mesh is filled at 0x1100ecd520 (+5 MB),
  and the SRT constants [34..37] (25, 4, 60, 0.2) of ps af7700b5 are filled too. So level geometry/data is
  allocated but never filled on bad loads; Treasures reloads it. The fixed state still shows grey holes
  in the distance (screenshot via PrintWindow).
  Suspect: level data streaming via Fios (ndlib/io/streaming.cpp; the same code asserted EIO at 17:47).
- Next run: GOW3_FILE_TRACE=.pak (probe default for now, 200000-line cap) to see whether pak/stream
  reads stop or fail on a bad load. tools/globals_diff.py (image globals save/diff) and
  tools/peek_any.py added.

### 14 October 2026 (evening): ground geometry is not generated on bad loads
- Reference review: reference/bloodborne (Supermedo windows branch) and reference/shadps4 (sparse,
  kernel/fiber) show no file, lock or DMA fix that applies. Our DMA CopyBuffer matches upstream, and
  FindImageFromRange already defaults to ensure_valid.
- .pak trace (GOW3_FILE_TRACE): every read succeeds; temple-0a/0b are read completely; streaming reads
  stop after the load.
- Per-frame GPU ring at 0x1100000000 (1 MB per frame slot; tools/region_map.py): bad frames write about
  0x90000 bytes, good frames 0xC0000. The extra data is ground vertices (stride 28, constant 0.00785
  before each position), so the game's CPU code doesn't emit the ground on bad loads.
- Menu host forced to the good configuration (mode 2, stack [+0x3b8], top 0) in a bad state:
  still black. Menu state is a symptom.
- Globals diff, two sessions (tools/globals_diff.py, analysis/globals/*): 60 consistent differences,
  e.g. allocation bitmaps of the object at 0xe924f0 (vtable 0x982810, methods 0x73xxxx/0x74xxxx,
  block 0xe92a10 size 0x5860) all free on bad loads; 0xe65190/0xe652e0 0 vs -2.
- Next: find the ground vertex writer with a hardware write watchpoint in a good state.
  The runtime now logs "Watch:" lines (probe.c watch_hit); arm with tools/watch_write.py <addr...>,
  e.g. ring offsets +0xa0000 of the active slots. Profiles: tools/sample_guest.py + profile_report.py.
- Watchpoints (DR0-3 on all threads, slot +0xa0000 of the active 1 MB blocks at 0x1100x00000): no CPU
  write hits, before or during a second Treasures. WriteProcessMemory to these pages fails (GPU page
  tracking keeps them write-protected), so an earlier "value restored" test was invalid.
- Leading model: the ground vertices are produced on the GPU. cs 308607aa (18 dispatches per frame,
  groups 26x1x1) reads compressed meshes from package memory (0x119e...) and writes decompressed
  vertices into 0x1100...; captures listed only its first 4 dispatches. The ground draws (vs 823cb43a,
  vs da3d9c98) read 0x1100xcd520.
- Capture now lists every draw's and dispatch's buffer addresses with GOW3_CAPTURE_ALL_BUFFERS=1
  (probe default), triggered by writing a pass count into analysis/uncharted-user/capture.trigger.
  Next: bad vs good capture; check whether a 308607aa dispatch writes the range the ground draws read.
- 18:3x: the ground shader's lighting records (64-byte table at 0x1038ee8380.., referenced through
  V# 0x1038ee858d in ps 45d0299c's flattened SRT data; same address in every run) are zero in bad
  states. After Treasures, 0x1038ee8500 became 1,1,1, 0.42.. with no CPU write (DR watch armed), so
  the GPU writes this table. Our SRT walker reads it from guest memory on the CPU at draw recording:
  candidate CPU/GPU ordering or coherency bug.
  Also, GPU compute output (e.g. 7d7e7206, 308607aa) is not visible in guest memory, so zero vertex
  buffers in guest memory prove nothing.
- Added GOW3_WATCH_RANGE=lo:hi (gpu/shim/gow3_watch_range.h; probe default 1038e00000:1038f00000):
  logs storage-buffer binds, DMA copies (CPU/GPU path), fills and WRITE_DATA into the range.
  Build 18:3x installed; the running game used gow3-probe.1818.exe (renamed).
- 18:4x ROOT CAUSE CANDIDATE: GOW3_WATCH_RANGE shows shader 2d215c3b (storage slot 1, many 16-172-byte
  writes) writing the constant memory 0x1038e7.../0x1038eb... that the ground shaders' SRT data points
  into. GOW3_READBACKS defaults to Relaxed (emulator_settings.h): GPU-modified pages are not
  read-protected, and GPU data reaches guest memory only when the CPU writes that page (write-fault
  readback). The CPU-side SRT walker therefore flattens stale zeros: ground constants (transform and
  lighting) are zero, so the ground collapses and Nate is unlit. Treasures happens to cause a CPU write to
  the page, which triggers the readback.
  Test: GOW3_READBACKS=2 (Precise) as probe default. The earlier "Precise: no change" predates this
  evidence.
- 18:4x CONFIRMED: in a grey state (no menu use), copying the GPU-modified range 0x1038e00000-0x1038f00000
  back to guest memory (GOW3_READBACK_TRIGGER "1038e00000 1038f00000"; BufferCache::DownloadRanges)
  restored the ground and Nate's lighting at once. Cause: GPU-written constants (copy shader 2d215c3b),
  flattened by the CPU-side SRT walker from stale guest memory under Relaxed readbacks.
  Precise readbacks (GOW3_READBACKS=2) hang on Continue. Fix candidate: GOW3_SYNC_GPU_CONSTANTS
  (Rasterizer::SyncSmallGpuWrites copies storage writes of 4 KB or less back at each submission,
  merged into 64 KiB windows).

## FIXED 14 October 2026 (18:50): grey/black ground, unlit Nate
Cause: the game writes per-object constants (ground transform and lighting records in 0x1038e.....)
with a GPU copy shader (cs 2d215c3b). Draws read them through SRT pointers, which our CPU-side SRT
walker flattens from guest memory. With GOW3_READBACKS=Relaxed (default) GPU writes reach guest
memory only when the CPU later writes the same page, so the walker flattened zeros. Pause -> Treasures
-> back fixed it only because a CPU write landed on that page and caused the write-fault readback.
Fix: Rasterizer::SyncSmallGpuWrites (vk_rasterizer.cpp) plus BufferCache::DownloadRanges. Shader
storage writes of 4 KB or less are copied back to guest memory at each submission (64 KiB windows,
one GPU wait). GOW3_SYNC_GPU_CONSTANTS=1 is the probe default; it costs about 1 FPS (57-59 FPS).
GOW3_READBACK_TRIGGER file: "auto"/"off", or "<lo> <hi>" for a one-off range readback.
Verified: a live grey state was fixed by a one-off readback of 0x1038e00000-0x1038f00000, and auto
mode is on. Fresh Continue loads with auto mode from launch are still to be confirmed by the user.
Not fixed by this (open): silent gunfire, black bullet-hit circles, the Fios EIO crash seen once
(17:47).
Ruled out on the way: PSN TUS popup and menu host mode (symptoms), Precise readbacks (hang on load).
Diagnostics left in the code (off by default): GOW3_WATCH_RANGE, GOW3_CAPTURE_ALL_BUFFERS,
Watch: debug-register hit logging in probe.c (tools/watch_write.py), tools/globals_diff.py,
sample_guest.py, region_map.py, peek_any.py.

## Sound effects silent (gunfire, footsteps, ambience); music and dialogue fine (14 October 2026, evening)
- Audio settings in game: all volumes full, 2 ch TV. Menu host mode/Treasures make no difference.
- Engine: Sony Scream with the BRB2 synth (brbDriver thread); Bluepoint audio config manager
  (biglib\audio\acm.cpp). Only 4 Ajm instances (codec AT9, flags 0x41 x3, 0x9 x1) decode the
  streams (music 4 ch, speech 1 ch); bank voices never reach Ajm (27 RIFF jobs per session, none on
  gunfire). common.bnk is intact in memory at 0x1000000000.
- The sound config text (sound1/config/ps4/prototypes.txt, banks.txt, reverbs.txt, environments.txt,
  mixers.txt) is NEVER opened and is not in memory. The load runs at startup:
  0x9bba5 -> 0x522d80 (acm 0x104c210) -> 0x519320/0x519410/0x5193a0/0x5159a0 (prototypes.txt)
  -> 0x515a50 -> file object 0x10fddf8 (enabled, callbacks installed: open = 0x8ef80) -> path builder
  0x8dfa0 (mode 1 loose override is off: 0x994188+0x3990 = 0; mode 2 = <root>/build/main/sound1/
  config/ps4/<name>) -> game FS 0x393760 (Fios). Without prototypes, every sound-proto request
  (footsteps, weapons, ambience) resolves to nothing; streams don't use protos.
- Next run (build 19:07): GOW3_AUDIO_TRACE also logs stat/access of sound paths;
  GOW3_FILE_TRACE=sound1/config. Find where the config open fails.
- 19:2x CAUSE FOUND (startup instruction breakpoints, GOW3_EXEC_WATCH, probe.c exec_watch_thread):
  open callback 0x8ef80 is called for reverbs.txt, mixers.txt, environments.txt and prototypes.txt
  (flags 1, type 1, mode 0). The path builds fine, but the Fios open (0x393760 -> 0x771a50) returns
  0x80820015 (SCE_FIOS_ERROR_READ_ONLY), mapped to EROFS 0x1e: `testb $5,%cl` at 0x8f1da turns flag
  1 into O_CREAT (0x40 -> Fios O_CREAT 8) on read-only /app0. Fios fails it without any kernel call.
  Fix: EBOOT_PATCHES "sound config files open read-only" (f6 c1 05 -> f6 c1 04). To be tested in
  gameplay; GOW3_EXEC_WATCH=515cc9 logs the loader result (expect rax 0).

## FIXED 14 October 2026 (19:3x): sound effects (footsteps, ambience, gunfire)
Confirmed by the user in gameplay. Log: all 4 config files open (reverbs 11405, mixers 405,
environments 10291, prototypes 204474 bytes); loader result 0 for each; Ajm now decodes bank
voices (794 RIFF jobs vs 27 before).
Fix: EBOOT_PATCHES "sound config files open read-only" (0x8f1da testb $5 -> $4).
Diagnostics left in code, off by default: GOW3_AUDIO_TRACE (now also stat/access of sound paths),
GOW3_FILE_TRACE, GOW3_EXEC_WATCH (startup instruction breakpoints, up to 4 guest offsets; hits log
registers and strings at rsi/rdx).
Still open: black bullet-hit circles; the Fios EIO crash seen once (17:47).

## 14 October 2026 (19:4x): dark interiors - fire and flashlights add no light; hair and cobwebs bright
- In the temple interior the braziers light nothing around them, the torch barely lights the walls,
  and hair, cobwebs and vines look unshadowed or bright.
- Wider readbacks (0x1030000000-0x1040000000, 0x112d200000-0x112d300000) changed nothing.
- Lights: cs 9bd0e806 builds per-light data (32 KB at 0x112d220200, including indirect args for the
  flashlight volume draws vs 1bf62085 / ps fe99f21a, 8a04b99b; those sample a D32 2048 shadow map and a
  512x512 cookie). cs 43b8ee5e (tiled lighting) loads 8 T# from light records with a GPU-computed
  index; SharpLocationFromSource -> UNKNOWN_LOCATION -> Image::Null -> null descriptor reads 0, so those
  lights are fully shadowed.
- Change (build 19:4x): resource_patching_pass returns 1.0 for reads of unresolvable T#s (lights apply
  without their per-light shadows); shader cache versions 8 -> 9. To be tested.
- Result: no visible change. 43b8ee5e has a single unresolvable image (its 8 T# dwords). The change was
  reverted (shader cache versions -> 10).
- Further facts for the dark-interior issue (open):
  - Flashlight volume indirect args are valid on the GPU (0x112d220200: 1230 indices, 0x112d220220:
    1200; the CPU view is stale zero).
  - Shadow map 0x1137458000 (D32 2048) is rendered by pass #15 (131 draws, depth less, no clear in the
    frame).
  - Indirect barrier is correct (dst AllCommands, MemoryRead|Write).
  - Depth-bias scaling never triggers.
  - Next: dump the light accumulation targets (0x113f8b0000 and 0x11408b0000 R16G16B16A16, written by
    the volume passes #20/#22) and the shadow map in this scene (frame dump diagnostic,
    guest-images.trigger) to see where the flashlight light is lost.
- 19:3x image dumps (launcher now always sets GOW3_FRAME_DUMP_TRIGGER/GOW3_DUMP_DIR =
  analysis/uncharted/dumps; write guest-images.trigger "addr size" lines, then touch trigger):
  in the dark room, the flashlight volume pass (ps fe99f21a) samples a 256x8 R16G16B16A16 ramp/LUT at
  0x1134ab0000 that is ALL ZERO (texture cache image flags 0x40; guest memory zero; GPU buffers zero).
  The light accumulation targets 0x113f8b0000/0x11408b0000 and the tiled output 0x11438f0000 are zero
  too. Shadow map 0x1137458000 (depth 0.18-0.79), cookie and normals look valid. Nothing writes the LUT
  in any capture; it is built at load/startup.
- GOW3_WATCH_RANGE now also logs render targets (BeginRenderingFull) and storage images (BindTextures);
  probe default 1134a00000:1134b00000 to find the LUT writer on the next launch/load.
- 19:4x narrowed GOW3_WATCH_RANGE (1134ab0000:1134ad0000): NO GPU-side write (render target, storage
  buffer/image, DMA, fill, WRITE_DATA) to the LUT in a whole session (startup, load, dark room), and the
  LUT is still sampled at 0x1134ab0000 (ps fe99f21a). Direct memory aliases share one section, so
  aliasing is not the issue. Next: startup data watchpoints on the LUT texels (GOW3_EXEC_WATCH now
  accepts w:<absolute address>, 8-byte write watch; GOW3_EXEC_WATCH_SECONDS, default 600).

## 14 October 2026 (20:0x): Uncharted 2 and 3 from the Collection menu

Symptom: picking Uncharted 2 or 3 in the menu closed the game. Four earlier boot logs show
`STOP: first unsupported PS4 import: JoBqSQt1yyA#libSceSystemService`, caller 0x14c7fd.

Cause:
- The Collection is three executables. The menu (eboot.bin, also Drake's Fortune) calls
  `sceSystemServiceLoadExec("/app0/big2-ps4_Shipping.elf"` or `big3`, with argv
  `{"eboot.bin", "ispartofuccollection"}` (string table 0x7778b0/0x7778d0 and 0x7a95ba/0x7a95c4;
  `"simulateplaygo"` is appended when a debug flag is set).
- The runtime had no LoadExec, so it exited.

Changes (not yet tested in game; the game was running during the build):
- runtime_services.c `load_exec`:
  - Writes the path and arguments, one per line, to GOW3_LOADEXEC_FILE (analysis/uncharted-user/loadexec.txt).
  - Exits with code 30. Without a handoff file it exits with 31 and a STOP line.
- probe.c: new `--guest-path /app0/<exe>` and repeatable `--guest-arg` options build guest argv (previously fixed to `/app0/eboot.bin`).
- tools/probe_uncharted.py loops:
  - On exit 30 with a handoff file, it prepares the requested executable and relaunches with those arguments. The same path handles U2/U3 back to the menu.
  - U2/U3 started directly (Start-Uncharted2.cmd / Start-Uncharted3.cmd) get the menu's arguments.
- U2/U3-only imports, mapped in import_names.inc and implemented:
  - POSIX open/read/write/lseek/close/fsync through libScePosix (existing implementations).
  - fcntl, select and sockets (network down).
  - pthread_condattr_init, scePthreadGetthreadid.
  - NpSignaling GetConnectionInfo/TerminateConnection/GetPeerNetInfo, NpScorePollAsync.
  - A `--check-imports --cpu-only` diff now shows nothing missing in U2/U3 that is not also missing in U1.
- The 2 s GPU frame-wait patch also applies to U2 (image 0x718886) and U3 (0x79f316); the code around it is byte-identical to U1's.
- The U1 sound-config patch has no counterpart: U2/U3 have no prototypes.txt.
- The U1 dark-interior diagnostics (GOW3_WATCH_RANGE, GOW3_EXEC_WATCH) are only set for eboot.bin.

Result (14 October 2026, 20:2x):
- The first test showed a grey screen at 60 FPS. The game loop ran, but nothing loaded and every draw was a fullscreen pass.
- Cause: the launcher passed argv as `/app0/big2-ps4_Shipping.elf eboot.bin ispartofuccollection`.
  - The U2 option parser (0x637250, called from 0x14972c) appends unknown arguments to a list at +0x3a10 and clears +0x4975.
  - So "eboot.bin" was taken as something to load, and the title screen never started.
- LoadExec's argv is the new program's complete argv, with argv[0] "eboot.bin". The launcher now passes it that way.
- U2 and U3 both reach their "Press X to Begin" title screens at 60 FPS (no input sent).
- Gameplay is not tested yet.
- Diagnostics used, in the scratchpad: walk.py (thread frame chains), fibers.py (sceFiber objects and the call chain of each suspended fiber), busy.py (non-idle guest call chains).
  - U2's game loop runs as job 0x14e020; its frame counter is at image+0xebd4e0.

## 14 October 2026 (20:3x): Uncharted 2 new game froze on the loading screen (dagger icon)

The game loop stopped at frame 0x501. An lldb backtrace of all threads (`lldb -p PID --batch -o "thread backtrace all"`) showed a deadlock:
- The GPU command thread: Rasterizer::Draw → GetGraphicsPipeline → QueueShader → `capture_job.Wait()`.
- The ShaderCapture worker running that job: SRT walker (JIT) read of 0x1038e0cd80 (GPU-written constants) → read fault → Rasterizer::ReadMemory → BufferCache::ReadMemory → `liverpool->SendCommand<true>` (waits for the GPU thread).
- Every other thread that faulted, plus direct_unmap → UnmapMemory → InvalidateMemory, queued behind the GPU thread.

Fix:
- PipelineCache::capturing_for_gpu_thread is a thread_local, set inside a capture job that the GPU command thread waits on.
- Rasterizer::IsGpuSideThread() returns true for that thread, so the fault takes the inline readback path (assume_locks) that a GPU-thread fault would take.
- This is safe because the GPU thread is blocked in Wait() for the whole job.

Not yet confirmed in game.

Second hang (20:34, caught by scratchpad/hangwatch.py, which runs lldb when U2's frame counter stops for 8 s):
- A draw-preparation worker in GetProgram held `programs_mutex` (shared).
  - Inside its speculative section (runtime_fault_recover set), ParseFetchShader read a GPU-modified page.
  - The read fault went to gow3gpu_handle_fault → ReadMemory → SendCommand, which waits for the GPU thread.
- The GPU thread was waiting for `programs_mutex` exclusively (GetProgram line 1245).
- Fix (probe.c, both the Windows and the POSIX path): during a speculative read, a fault skips GPU page handling and takes the recovery path. The draw-prep result fails and the GPU thread does that draw itself.

## 14 October 2026 (20:4x): U1 dark interiors - lights FIXED by synchronous shader compilation

Cause:
- The 256x8 R16G16B16A16 LUT at 0x1134ab0000 (sampled by the flashlight volume pass, ps fe99f21a/8a04b99b) is rendered once at startup by ps 2d7325a4 (render target 128 KiB).
- With async_shaders=1, PipelineCache::GetGraphicsPipeline returns nullptr for a pipeline that is still compiling, and that draw is skipped.
- Pipelines are not warmed up at start ("on demand"), so this one-time draw was skipped every session.

Fix (tested by the user 20:4x: the torch and flashlight light the room):
- The launcher sets GOW3_ASYNC_SHADERS=0 for eboot.bin. This overrides async_shaders in probe.ini.

Still open:
- Hair and cobwebs are bright in dark rooms.
  - Hair is pass #33 (vs 58b25301, ps cc2d0c16; alpha blend). Cobwebs and other transparents are pass #35.
  - Both sample the D16 2048 shadow map 0x11352d0000 (pass #5: a depth-ALWAYS full-screen draw first, then 68 casters) and the light accumulation 0x113f8b0000.
  - Suspect: the sun or directional term is unshadowed for forward-lit objects.
  - The guest image dump now supports D16Unorm.
- Frame drops to 45-57 FPS in the temple.
  - A host sample of the GPU command thread: about 49% is in BufferCache::DownloadMemory → Scheduler::Wait, called via SendCommand from BufferCache::ReadMemory. Guest CPU threads read GPU-modified pages, and every fault waits for the whole GPU.
  - GOW3_FAULT_STATS=1 (page_manager.cpp, launcher default for now) prints the counts, the hottest guest RIPs and the 64 KiB windows every 5 s.

## 14 October 2026 (21:1x): temple frame rate and bright hair/cobwebs, measurements

Frame rate (GOW3_FRAME_STATS):
- The temple room runs a steady 45-49 FPS (21 ms median) with 0 shader compiles.
- The GPU command thread is never idle and is blocked on GPU ticks about 50% of the time.
- The DMA sync of the tiled lighting shader (BufferCache::SynchronizeDmaBuffers before each cs 43b8ee5e dispatch) uploads about 3 GB per 5 s, all CPU-dirty resident memory.
  - New live modes via GOW3_DMA_TRIGGER (analysis/uncharted-user/dma.trigger): "full" (default), "off", "pointers <KiB>" (windows around guest addresses in the stage's user data and flattened SRT).
  - Measured: full 45 FPS, off 48, pointers 48 (about 170-260 MB per 5 s). So the sync is about 3 FPS of the loss. The rest is GPU time.
- Next: GOW3_GPU_PROFILE=1 (now a launcher default) for GPU ms per pass.

Hair and cobwebs:
- The D16 2048 map 0x11352d0000 is cleared to 0 inside a rectangle (a z=-1 TriList quad, DX clip space, z clip off, viewport z 0..1). Every LESS caster fails against 0, which is also what a PS4 does.
- All shadow lookups use image_sample_c_lz with compare LESS. Hair ps cc2d0c16 (hero-default.pak), cobweb 7187fa25 (catacombs-1.pak) and eddie 3d0b0bda each have one.
- Live compare overrides ("cmp never|always|normal|border-black|border-white|edge" in the same trigger file):
  - never: the cobweb and hair go dark.
  - always: walls are sunlit too.
  - normal: walls are dark, but the cobweb and hair are lit.
- So the hair/cobweb lookups land outside the cleared rectangle (border 65535) or beyond the texture edge. The compare-sampler setup (clamp/border) is now logged once per setup.

## 14 October 2026 (22:0x): bright hair/cobwebs - cause found: MUBUF ADDR64 loads were unimplemented

- The D16 2048 sun shadow map 0x11352d0000 is a 4-slice array, one slice per cascade (DB_DEPTH_VIEW slices 0-3).
  - Slice 0 is all 0 in the temple: the roof is pancaked to the near plane, which is correct.
  - Slices 1-3 have content. Dumps now include every array layer (_L<n> files).
- Hair (ps cc2d0c16, hero-default.pak), cobweb (ps 7187fa25, catacombs-1.pak) and eddie (ps 3d0b0bda) look up shadows with image_sample_c_lz (DA=1, compare LESS, clamp to edge).
- Their cascade data (192 bytes per cascade) is read with BUFFER_LOAD_DWORD/X4 in ADDR64 mode:
  - vaddr v[0:1] = s30 + 192*cascade; V# s[64:67].
  - The translator ignored mubuf.addr64 and treated v0 as a buffer offset, so the matrices were garbage and the lookups landed in lit texels.
  - The walls' shader does not use ADDR64, so walls were right.
- Fix (vector_memory.cpp BUFFER_LOAD): for ADDR64 untyped dword loads, address = V#.base + v[vaddr:vaddr+1] + soffset + inst offset, read with ReadConst (the direct memory path; the shader becomes a DMA user).
  - Shader cache versions 10 -> 11.
- Tooling: gpu target `gcn-disasm` (tests/gcn_disasm.cpp) disassembles GCN binaries with the recompiler's decoder. Shader binaries come from the game packs by their OrbShdr footer hash.

Hair/cobweb FIX confirmed (22:1x, user screenshot):
- The cobweb is dark like the room and Nate's hair is natural. No flicker: 6 consecutive screenshots give the same cobweb-region brightness.
- The first ADDR64 build flickered. DMA reads of pages that the BDA page table did not map yet return 0 and are mapped only after the submission (FaultManager), and per-frame data lives on fresh pages.
  - Fix: DMA sync mode "pointers" (launcher default GOW3_DMA_SYNC="pointers 256") calls ObtainBuffer on 256 KiB windows at each guest address in the DMA stage's user data / flattened SRT data. The windows become resident and current before the draw.
  - The full per-draw sync is gone: 490 -> 75 ms per 5 s on the GPU thread.
- Flatten pass: ReadConsts with a per-lane (non user-data) base are left dynamic, and SimplifyReadConstAddressAdd checks the base arity.

Temple frame rate (47-49 FPS), cause:
- About 1000 GPU downloads per 5 s with a full GPU wait each (buffer_cache.cpp DownloadMemory; about 2.5 s per 5 s).
- They are all CPU writes into GPU-written memory in 0x1038d00000 / 0x1038e00000 (GPU constants of cs 2d215c3b), 0x1036200000 and 0x1061e00000.
- They come through Rasterizer::OnWriteFault -> InvalidateMemory, with the download run on the GPU thread (SendCommand).
- Fix in test: BufferCache remembers 1 MiB windows where this happened (IsCpuWriteHot). GPU storage writes up to 4 MiB in those windows join the per-submission batched download (SyncSmallGpuWrites), so the CPU's later writes find clean pages.
- Measuring tools: GOW3_FAULT_STATS (download stats by window), GOW3_PERF_DIAG (wait profile by caller), GOW3_FRAME_STATS, GOW3_GPU_PROFILE (now works for Uncharted: frames are marked at the display pass after a flush).

## 14 October 2026 (22:5x): temple frame rate - lazy CPU writes into GPU-written pages (in test)

Measured before the change (brazier room, 46-52 FPS):
- About 800 CPU write faults per 5 s land in GPU-written pages. Each one downloaded a 512 KiB window with a full GPU wait (buffer_cache.cpp DownloadMemory, about 2.5 s of waiting per 5 s; "GPU ticks" blocked about 50%).
- None of the faulting writes touch GPU-written bytes. They hit other bytes in the same page: about 300 GPU-written bytes per page.
  - 0x1036221bf4..c0c: GDS-to-memory DMA copies.
  - 0x1061e00000: 1 KiB storage writes of cs 6a6422a9.
- The 2d215c3b scalar-copy HLE (vk_shader_hle.cpp) had already removed the 0x1038d/e stalls.

Change (BufferCache::TryLazyWrite and LazyPage; GOW3_LAZY_WRITES=0 turns it off; relaxed readbacks only):
- A write fault whose bytes don't intersect gpu_modified_ranges unprotects the page and skips the download. The guest copy of the page's GPU-written bytes stays stale and is recorded.
- Uploads (SynchronizeMemory, through ForEachLazyUploadPiece) leave out recorded bytes whose guest copy still matches the record, so the GPU values in the arena survive.
- A byte that differs from its record was written by the CPU after the GPU, and the CPU value wins:
  - an upload sends it and subtracts it from gpu_modified_ranges;
  - a download (WriteBackGpuData) doesn't overwrite it.
- Downloads erase the records of their pages.
- The "Download stats" line now adds "lazy writes (no download)" and the number of GPU-written bytes the CPU overwrote later.

## 14 October 2026 (23:1x): async readbacks, 60 FPS in the temple; treasure-pickup crash fixed (in test)

Result of lazy writes:
- The DownloadMemory waits are gone.
- The next limit was the per-submission readback of small GPU writes (SyncSmallGpuWrites -> DownloadRanges, ~10 ms Finish per frame, 44 FPS with ~2600 draws per frame).
- Turning that readback off ("off" trigger) didn't help: the CPU then wrote GPU-written bytes about once per frame (0x1036200000), each a full wait.

Async readbacks (BufferCache::DownloadRangesAsync; GOW3_ASYNC_READBACK=0 waits instead):
- The copies are recorded at submission end into a held HostCached staging buffer.
- PollReadbacks (per BindResources and at OnSubmit) writes them back once the tick passes.
- Until then the pieces stay in gpu_modified_ranges. `rewritten` holds later GPU writes, which stay GPU-modified.
- A CPU write fault on bytes whose newest data is in a readback waits for that tick only.
- Synchronous downloads complete overlapping readbacks first.
- User result: steady 60 FPS (frame median 16.65 ms, ~3000 draws per frame).

Crash on picking up a treasure: STOP in ir_emitter Pack4x8 "Unsupported 4x8 number format: Float".
- cs 8f067540 bound garbage V#s (Clamped size errors).
- Likely cause: the 2d215c3b scalar-copy HLE checked GPU-modified per page. Lazy pages and pending readbacks keep page bits set longer, so copies went to the GPU, and the SRT walker read their output a frame late (uninitialised for a new effect).

Fixes:
- The HLE checks bytes (BufferCache::IsRangeGpuWritten) and unprotects dst itself (PrepareHostWrite, lazy snapshot of other GPU bytes in the page).
- The pack/unpack helpers in ir_emitter.cpp log and fall back to the default format instead of UNREACHABLE.
- GOW3_FAULT_STATS prints "Scalar copy HLE (5 s): N on the CPU, M on the GPU".
- Launcher: GOW3_GPU_PROFILE and GOW3_WATCH_RANGE are off (the profiler splits render passes).

## 14 October 2026 (late): Uncharted launcher, frame-rate unlock, render resolution

- **Launcher:** launcher/uncharted_launcher.pyw (Tkinter + Pillow), started by Start-Uncharted.cmd and Uncharted.lnk.
  - Game cards for U1/U2/U3; quality presets Low (720p/30) / Medium (1080p/60) / High (1440p/60) / Ultra (1440p, unlocked); Display, Performance and Advanced tabs.
  - Writes probe.ini (shared with the in-game menu); launcher-only options go to %APPDATA%/Uncharted Launcher/launcher.json.
  - Env passed to tools/probe_uncharted.py: UNCHARTED_DIAGNOSTICS, GOW3_VBLANK_HZ=0 for limits above 60, UNCHARTED_RESOLUTION, GOW3_DRAW_PIPE / GOW3_VK_RECORD_THREAD (multithreaded rendering, experimental, off).
- **Game runs as gow3_pc/out/Uncharted.exe**, copied from gow3-probe.exe at launch. The exe icon, the window icon (sce_sys/icon0.png) and the title are Uncharted.
- **In-game menu:** the GoW tabs and rows are removed (cheats, orbs, enemy health bar, engine frame rate, resolution patches, game hooks). FPS limits are 30/60/120/144/165/240/unlimited, without the engine-fps cap.
- **Frame-rate unlock:** the present thread uses the menu limit; above 60 needs a 480 Hz vblank (GOW3_VBLANK_HZ=0). Untested in game (game speed).

Render resolution (tools/probe_uncharted.py resolution_patch, UNCHARTED_RESOLUTION=WxH; all three games have the same code):
- The display settings constructors and the 1080p/720p mode switch store the size in many fields:
  - 1920x1080 at +0x08/+0x18/+0x24/+0x38
  - tiles +0x40 = w/8, +0x44 = h/4
  - a larger size at +0x30/+0x34 (2560x1216)
  - packed movabs 0x43800000780
  These are 33 stores in total. There is also a generic SetResolution(w, h, aspect) that computes the same fields.
- Reporting a 4K display (sceVideoOutGetResolutionStatus) has no effect: the size is hard-coded.
- Memory table (32-byte entries {id, parent, u64 size, u64 align, 0}):
  - Render Target Heap: id 0x85, parent 6. 497 MiB in U1/U2, 561 MiB in U3.
  - GPU (garlic) block: id 3, parent 2. 2988 MiB in U1/U2, 3052 MiB in U3.
  - Both grow by heap * (pixel factor * 1.1 - 1), rounded up to 64 MiB. A non-aligned garlic crashed right after the onion mapping.
- The table check `lea rsi,[rbp-0x20]; movabs rdx,0x120000000` expects the direct blocks to total 4608 MiB, so it's raised by the extra too.
- The runtime extends the game's 5056 MiB search end to GOW3_DMEM_MB (runtime_memory.c).
- **Tested** (boot to title, 45-50 s, no asserts): 720p and 1440p in U1; 1440p in U2 and U3.
- **Limits:**
  - The "GPU Mem" heap is the whole garlic block, and its size is cut to 32 bits: at 4K (4716 MiB) it became 620 MiB, and the 368 MiB sub-heap 0x87 failed. Garlic must stay under 4 GiB.
  - At 1800p a second heap, "Render Target" (id 0x84, 139.77 MiB), ran out (a 45 MiB request).
  - 4K needs the 32-bit cut fixed. The heap names are set by static constructors around 0x4123xx (GPU Mem object 0xd93c40), and the size comes from a separate init that hasn't been found yet.
- MediEvil (reference/medievil 3a16e0e) did 4K with the same method: a bigger framebuffer pool plus the address-range checks in its free paths.

## 15 October 2026 (00:xx): U2 crash (fiber stacks), native 4K

**Uncharted 2 in-game crash** (exit 0xC0000005, nothing logged; WER: fault at 0xb8ecf3e = guest 0x4ecf3e)
- Crash site: a `ChooseJob` worker's `mov [rsp+0xc0], rax` hit a write-protected stack page. With the stack unwritable, Windows can't push the exception frame, so it kills the process silently.
- Fiber stacks come from game memory (sceFiber contexts). The DMA pointer windows (256 KiB around any dword pair that looks like an address) can cover them.
- Fix: gow3_fiber.cpp registers every fiber context (Initialize/AttachContext; Finalize unregisters), and AddressSpace::Protect (gow3gpu.cpp Gow3KeepWritable) never write-protects those ranges.
- Tried first and discarded: a no-protect window sync. Re-uploading writable pages cost ~1 GB/s.
- Image layout: the runtime image is the end of boot-linked.bin (size at header offset 8) at base 0xb400000. Guest offsets are image offsets. **boot.bin is NOT the loaded layout.** Helper: scratchpad re/img.py.

**4K**
- Memory areas: entry 2 onion = context 5 ("CPU Mem"), entry 3 garlic = context 6 ("GPU Mem"). MemoryArea (memory-area-ps4.cpp) holds one page id per MiB in a 4096-entry array, so one area can't exceed 4096 MiB. MemoryArea::Initialize asserts MiB-aligned sizes. Each area starts on a 4 GiB boundary.
- Fix: above the garlic limit the Render Target Heap (0x85) moves to the onion area (context 5, rounded to 64 MiB). Garlic's rounding goes to the second RT heap (0x84), which also grows with the pixel factor.
- Boot-tested at 3840x2160 (title screen, 50 s, no asserts): U1 and U2. Gameplay is not tested.
- U3 at 4K, three attempts:
  - First: "HALT: Command context dcb full! Increase size!" (ndgi-orbis.cpp ComputeContextBufferFullCallback). The compute contexts are set up by the function that installs that callback; buffer sizes are "Deform Entities" 16 KiB and, U3 only, count*0x1400+0x1c00 capped at 0xd000.
  - Buffers enlarged x4: corruption crash (wild pointer at guest 0xe5260b).
  - Buffers enlarged x2: corruption crash at 0x55bc10.
  - Likely more buffers sized for 1080p (tiled lighting lists?). U3 is clamped to 1440p in probe_uncharted.py for now.

## 15 October 2026 (00:3x): resolutions above 1080p fail in gameplay; presets back to 1080p

User sessions:
- **U2, 1440p, 60 FPS:** played for minutes, then the game assert `m_lookAt.X() == m_lookAt.X()` (NaN) in UpdateFocus, player-base.cpp:1101.
- **U1, 4K Ultra:** assert `offsetTableIndex != -1 && offsetTableIndex < GenericOffsetTable::s_totalValidOffsetTables` (BP/Shared/TGenericOffsetPtr.h:100): Bluepoint's offset pointer tables don't cover the Render Target Heap after it moves to the onion area. Just before: 32k guest read faults per 5 s at 0x10ecxxxxxx (that heap).
- **U3, 1440p:** corrupted pointer while loading into the game (guest 0xe5dca2, same area as the 4K corruption).
- The green cutscene at 1440p is probably the same class of problem.

Changes:
- The launcher presets all render at 1080p. High and Ultra use FSR 1 to the screen; Ultra unlocks FPS with 2 frames queued.
- Resolution above 1080p is labeled experimental. Config version 2 resets a saved resolution to 1080p.
- U3 is clamped to 1080p in probe_uncharted.py.

Next for >1080p:
- CPU readbacks of render targets that assume the 1080p layout (look-at NaN).
- Buffers sized for 1080p (U3).
- BP offset tables, for the onion move.

## 15 October 2026 (01:00): U3 crash cause, U3 hair shadow flicker

**U3 "dcb full" crash at the London pub, even at 1080p**
- Bisect with the user: all three new mechanisms off: OK. Only GOW3_ASYNC_READBACK off: OK. So the cause is the asynchronous readbacks. U3 runs with GOW3_ASYNC_READBACK=0 (probe_uncharted.py).
- Planned fix: read-protect pages with in-flight readback pieces; a guest read waits for that readback only.

**U3 hair lit for a frame now and then (sunlit museum)**
- Frame dumps: about 1 in 8 frames loses the window-frame sun shadow on hair. The ADDR64 hair shaders (ps 5d1bfb2d / 9e3f2c9d / 47f0eee5 / b107ecc9, pass 29) read cascade data at (s42:s43) + v2.
- Ruled out: DMA page faults (0, new GOW3_FAULT_STATS "DMA page faults" line), full DMA sync, compiles and skipped draws.
- Cause: DMA reads are invisible to the barrier tracker. The window uploads recorded in SyncDmaMemory (end of BindResources) never set needs_barrier, so a read could see a copy mid-flight, and later copies into the arena had no barrier against the draw's reads.
- Fix: needs_barrier |= IsBufferAccessed(arena, whole) for the window arenas, and ResetBindings registers a shader read of those arenas (dma_read_arenas). Built 00:59; the user's test is pending.
- The U3 hair shaders were extracted from the actor33 packs (young-drake.pak etc.; scratchpad find_shaders.py and u3shaders/).
