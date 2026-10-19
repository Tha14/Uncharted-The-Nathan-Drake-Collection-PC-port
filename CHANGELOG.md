# Changelog

Uncharted: The Nathan Drake Collection PC port, by **BilalReloaded**
([YouTube](https://www.youtube.com/@BilalReloaded/videos), [GitHub](https://github.com/bc160201983)).
Technical details of every item: [analysis/STATUS.md](analysis/STATUS.md).

## 19 October 2026 (0.4.0)

- **No more freeze before some cutscenes** (#52): before a cutscene the games remove ~100 small
  blocks of memory, and for each one the port rebuilt the whole surrounding mapping (about 23 ms
  each on Windows, far more under Proton): Drake's Fortune froze 1.6 s, the Steam Deck up to 33 s
  in Uncharted 3's museum. Memory is now mapped in 2 MB pieces, so a removal only touches the
  pieces around it: the same cutscene went from a 1650 ms frame to ~200 ms.
- **Uncharted 3 no longer freezes for good with multithreaded rendering** (#51): a fence write on
  the Vulkan recording thread could fault while the GPU command thread waited for that thread.
  It now never faults. Multithreaded rendering stays off for Uncharted 3 until players confirm
  it (`UNCHARTED_U3_MULTITHREADED_TEST=1` keeps the launcher's choice).
- **Drake's Fortune: enemy grenade launchers no longer blow up on Drake** (#6): from the second
  shot on, the grenade exploded right in front of him. The shot started from a frame-old copy of
  data the GPU writes for the game (a copy shader whose results it reads, the cause of
  Uncharted 2's floating NPCs too); that data now reaches the game before it is used, as in
  Uncharted 2 and 3. No frame-rate cost measured.
- **Drake's Fortune: Infinite ammo keeps the magazine full** (Cheats tab): the game's own switch
  only spared the reserve when reloading; each shot still emptied the magazine.

From kuraeow (Steam Deck), merged:

- **Real upscaling** (#50, asked for in #45): Display -> **Upscaling**: FSR 1, FSR 3.1, FSR 4 or
  DLSS, with **Quality**, Balanced, Performance or Ultra Performance. The game draws below your
  resolution and each frame is upscaled to it before the HUD and menus are drawn, which stay
  sharp. **Native AA / DLAA** (no upscaling) stays as before. Upscaling saves GPU time: it helps
  at 4K or on weaker graphics cards, much less where the processor limits the frame rate.
  Moving characters can smear below Quality (the motion vectors come from the camera only).
  The **Low** preset is now 1080p with FSR 1 Performance (HUD at 1080p). Drake's Fortune above
  1620p draws at 1620p and is upscaled with the HUD at full size.

## 19 October 2026 (0.3.1)

- **Choose the game update in the launcher** (#32): Launcher -> Advanced -> GAME UPDATE.
  **Choose update folder**: your extracted update 1.02, wherever it is and whatever it is called
  (nothing is copied; the launcher remembers it). **Install from .pkg**: the update's `.pkg` file,
  unpacked next to the game folder under the right name. Both check that it is an update for your
  copy of the game (same region) and that nothing the port needs is missing (the update's
  `data.psarc` files, for example).
- **A damaged update is named instead of crashing:** with an update whose game archives are
  incomplete or damaged, all three games stop at start with "files.json -> not found" and
  `ASSERTION: root` (ingame-shaders.cpp). Before a game starts, the launcher now checks the update's
  archives (complete, readable, shader list inside) and says which file is damaged. The port still
  ships no game files: you need your own update PKG. Uses PkgTool from LibOrbisPkg (LGPL-3.0).
  Not fixed yet: the same stop with the US version CUSA02320 (#29, #43, #48), whose update files
  are intact. Until then, renaming its `-patch` folder starts the games as 1.00.
- **Shortcuts on a moved Desktop** (#39): Desktop shortcut and Game shortcuts failed with
  "DirectoryNotFound" when Windows keeps the Desktop elsewhere (OneDrive, a renamed or redirected
  folder). The launcher now asks Windows where it is.

- **"exception 0x20474343" says what failed** (#47): an error nothing caught on the GPU command
  thread ended the game with only that code. The log (and the launcher's crash message) now names
  it, for example "out of memory on the GPU command thread". The crash guard of 0.3.0 no longer
  needs memory to be available: when Windows refuses its snapshot, that readback lands as before.
- **A graphics pipeline the driver refuses no longer stops the game** (#40, AMD Radeon RX 9070 XT):
  "Failed to create graphics pipeline: ErrorInitializationFailed" ended every game at start. The
  pipeline is now created again without the pipeline cache; refused again, only the draws that need
  it are skipped and the log names it ("GPU: the driver refused graphics pipeline ...").

From SuperMedo/uncharted_pc (thanks to kj2223 and hugouchoasborges), ported:

- **Sun shadows no longer flicker with TAA, FSR or DLSS/DLAA** (#261, kj2223). The sun shadow pass
  (the same shader in all three games) rebuilt each pixel's position from the scene depth without
  the anti-aliasing's sub-pixel offset, so its shadows moved against the geometry every frame. It
  now gets the same offset as the scene.
- **Steadier anti-aliasing** (#261, kj2223): Uncharted draws twice into the picture it shows, and
  the port started a new frame on both. The second start threw away the anti-aliasing's history
  every frame and skipped every other sub-pixel position. It is now one frame start per frame
  (measured: 600 starts per 10 s at 60 FPS, was per 5 s).
- **Steam Deck and other Linux AMD/Intel GPUs** (#94, hugouchoasborges): memory binding goes to a
  GPU queue that supports it (on Steam Deck the graphics queue does not), and large texture
  conversions are split to stay within the GPU's limit (65,535 work groups on some GPUs). On
  NVIDIA and AMD on Windows nothing changes.

From kuraeow (Steam Deck), merged:

- **16:10 aspect ratio** (#35): 720p becomes 1280x800 (the Steam Deck's screen); the games show
  more at the top and bottom. Resolution buttons are labelled for the chosen ratio, with the exact
  render size below.
- **Cutscene movies keep their shape** at sizes that aren't 16:9 (#36): black bars instead of a
  stretched picture, at 16:10 and ultrawide.
- **Update 1.02 merged into the game folder** (#37) is recognised even when its param.sfo still
  says 1.00 (no more "install update 1.02" warning, cheats work).
- **Steam Deck / Proton**: one set of launcher options for every Steam shortcut (#38).

## 19 October 2026 (0.3.0)

- **Random crashes after a while, "exit code 3"** (#30, #24, #27): GPU results reach the game's
  memory a little late, up to a frame or two after the game was told the GPU had finished. When
  the game reused that memory in between (an area unloading, a cutscene starting), the late copy
  overwrote the game's new data: crashes in the game's memory allocator, a function pointer turned
  to 0, and similar. Late GPU data now never overwrites a 64-byte block the game changed in the
  meantime. In 51 minutes of Uncharted 3 it stepped in 38 times, once exactly where the game had
  crashed before. Logged as "Readback guard: kept ..."; `GOW3_READBACK_GUARD=0` turns it off.
- **Uncharted 3: blurry view when aiming** (#16): the aiming depth of field read its focus
  distance from a one-pixel image the GPU renders, which was only copied back to the game in
  Uncharted 2. It stayed at 0 m and everything behind Nate was blurred. Uncharted 3 now gets it
  too; the focus follows what you aim at.
- **Fewer shader hitches**: compute shaders found in the game's command buffers are compiled in
  the background before the GPU needs them, instead of all at once on the frame that first uses
  them (`GOW3_COMPUTE_PRECOMPILE=0` turns it off).
- **Cheats** (#26): the launcher's new **Cheats** tab: Invincible, Infinite ammo, No reload
  (Uncharted 2 and 3), Infinite grenades and One-hit kills (Uncharted 3). They switch on the games'
  own developer options; they need update 1.02. In Uncharted 1 and 2 the infinite ammo option lives
  in the games' settings system, which resets it every frame, so the places that read it are
  changed instead.
- **Ultrawide shows more at the sides** (#20): at 21:9 and 32:9 the three games kept the 16:9
  picture's width of view and cut off the top and bottom instead. They now keep the 16:9 view top
  to bottom and widen it to the sides, culling included (nothing missing at the screen edges);
  checked in all three games at 3440x1440 and in Uncharted 2 at 5120x1440. 16:9 is unchanged.

- **Start a game directly** (#25): Launcher -> Advanced -> **Game shortcuts** puts one desktop
  shortcut per game; each starts its game with the launcher's settings, without the launcher
  window. From a command prompt: `"Uncharted Launcher.exe" --play u1` (or `u2`, `u3`). A
  double-click on a game's card in the launcher plays it too.
- **Launcher: buttons on the Advanced page were hidden** (log folder, save folder, shortcuts,
  shader cache, reset, check for updates): the page had grown taller than the window. Pages
  that don't fit now scroll, and they are grouped as Shortcuts, Folders and Troubleshooting.
- **Launcher: easier to use.** More room for the settings (a smaller header; the window opens
  taller when the screen allows), plainer descriptions, recommended choices marked with a star,
  and Quick Setup marks the preset that suits your graphics card ("Recommended").
- **Warning without update 1.02** (#24): most of the port's game fixes, crash fixes among them,
  are written for update 1.02 and are skipped on 1.00 (the log listed them as "NOT applied"
  without saying why). The launcher now says so once when you press Play, and the log names the
  game version and where the update goes.
- **Patreon** link in the launcher header.
- **Crash messages say what crashed.** "Exit code 3" is the port's general crash code; the launcher
  now shows the crash's details from the log (what failed and where) and asks for boot.log, instead
  of only the exit code (#24, #27).
- **Freeze reports.** When a game shows no new frame for 30 seconds, the start script records where
  every thread is stuck (hang-<time>.txt in the log folder, a few KB) and the launcher points to it.
  Please attach it with boot.log to a GitHub issue: every freeze can now be traced to its cause.
  A fault inside the port's own fault handling now stops with a report instead of freezing.
- **Freeze in any game** (since 0.2.0, seen in Uncharted 3): when a shader's resource table held a
  garbage pointer, the 0.2.0 fix for the rooftop freeze looked that address up in its page table
  without a range check, which faulted inside the fault handler, endlessly. It now only looks up
  game addresses; such loads are skipped as before 0.2.0.
- **Crash after a long session** (seen in Uncharted 2 after 38 minutes, `Uncharted.exe+0x11474b`):
  when the texture cache's image table grew while a depth buffer got its stencil part, the code
  went on using the old location of the depth image. Fixed there and in one other place with the
  same mistake.

From the work on a ROG Zephyrus G14 laptop (Radeon 780M and RTX 5060 Laptop GPU), merged:

- **Graphics cards with 6-8 GB: no more texture thrashing.** The texture cache started throwing
  textures out at about 40% of the card's memory, so on 6-8 GB cards it evicted hundreds of
  textures every few seconds and reloaded them (75-150 ms hitches about every 15 s on an 8 GB RTX
  5060). It now goes by the driver's memory budget, as on integrated GPUs. The crashes in #24 (GTX
  1660 SUPER, 6 GB) all came while it was evicting.
- **Uncharted 3 could stop responding when a level started** (a guest write looping on a page a
  GPU readback still protected): the readback is now landed at once. A second guard beside the
  rooftop fix of 0.2.0.
- **Update 1.02 from exFAT and FAT32 drives** (USB sticks): those drives hold no links, so the
  update was skipped and the game ran as 1.00. The runtime now lays the update over the game itself.
- **"Error 1450" at start on PCs with 16 GB of RAM**: the game's memory is now committed as it is
  used instead of all at once.
- **Memory corruption after partly freeing game memory** (flexible memory split): could zero live
  game data on any PC, a candidate for random crashes.
- **Multithreaded rendering**: readbacks could be landed twice (heap corruption) in Uncharted 3;
  fixed. Uncharted 3 ran 24% faster with it on the 780M.
- **Drake's Fortune compiles shaders in the background** again (it froze 1-2.4 s in each new area;
  now about 120 ms at worst). UNCHARTED_U1_SYNC_SHADERS=1 restores the old way.
- **RTX laptops: fewer hitches** (render targets no longer get a driver allocation each), **AMD
  GPUs**: fewer allocation stalls at level start and a cheaper second-queue fetch.

## 18 October 2026 (0.2.3)

- **Uncharted 1: guns flickering flat grey** (#23). With multithreaded rendering on, Nate's rifle
  and pistol (and at times other objects) were drawn flat grey on some frames. Drake's Fortune now
  always runs without multithreaded rendering: it gained nothing from it there.
- **Uncharted 2 crash `m_lookAt.X() == m_lookAt.X()`** (#24): the point Nate looks at could become
  invalid (NaN) and the game stopped at its own check, as Uncharted 3 did in #13. Uncharted 2 now
  resets that point to where the player looks instead, as the Uncharted 3 fix does.
- **Frame captures** (diagnostics): a capture requested during play could end the game when a
  shader had a buffer at address 0; such buffers are now skipped.

## 18 October 2026 (0.2.2)

- **Windows 10: all three games crashed at start on some PCs** (#22): the games' job code runs on
  PS4 fibers with their own stacks, and some Windows 10 builds end the process when an exception
  resumes on a stack the thread doesn't have registered. The port now registers the stack in use
  before resuming.
- **Uncharted 3 crash in Syria** (#21, `PatchImageSharp`): a damaged image description while a
  level loads made the shader translator stop the game; that one write is now skipped.
- **All the Collection's languages.** Launcher -> Advanced -> **Language**: the language the PS4
  would be set to, from the 22 the games have text for (English US/UK, French, German, Italian,
  Spanish and Latin American Spanish, Portuguese and Brazilian Portuguese, Dutch, Danish,
  Norwegian, Swedish, Finnish, Polish, Czech, Greek, Russian, Turkish, Japanese, Korean,
  Traditional Chinese). Each game's main menu then offers it next to English: press **Square**
  to switch its menus and subtitles. Spoken language depends on your copy of the game: the list
  marks the languages it has voices for (the European CUSA02344 has English, Polish, Russian and
  Turkish). Before, the port always reported English, so only English was offered.
- **Unlock all chapters.** Launcher -> Advanced -> **Unlock all chapters**: every chapter in each
  game's Chapters menu (Drake's Fortune 1-22, Among Thieves 1-26, Drake's Deception 1-22) and the
  Brutal difficulty from the start, without finishing the games first. It turns on the
  Collection's own "unlock by default" switches, so saves and progress are untouched. Off by
  default; needs update 1.02.
- **Launcher errors are shown and saved** (`analysis\uncharted-user\launcher-error.log`); an error
  at start used to close it silently.
- **Long sessions:** a diagnostics list that grew with every GPU image and buffer made in a session
  (never shrunk, searched on every free) is now kept only with the GPU fault diagnostics on.

## 18 October 2026 (0.2.1)

- **NVIDIA DLAA and AMD FSR 4 Native AA are in the package.** The 0.2.0 build could not fetch
  their files (tools/fetch_aa_files.sh copied the Vulkan headers from `//clang64` on the build
  runner, where the MSYS2 root is `/`), so the menus offered only TAA and FSR 3.1.

## 18 October 2026 (0.2.0)

- **Smooth frame pacing above 60 FPS with VSync (all three games).** With a frame rate limit at or
  above the monitor's refresh rate and VSync on, the game made frames faster than the monitor
  shows them; they queued up behind VSync and the game was held back in bursts: two or three
  quick frames, then a long one. Now the game follows the monitor (its refresh rate, up to
  120 Hz), as it follows a 60 Hz screen: on a 75 Hz monitor at the 120 setting, Uncharted 3's
  rooftops and Uncharted 2 run at an even 75 FPS, 1% lows 32 -> 66 FPS (Uncharted 2: 22 -> 67),
  no frame over 20 ms (28% before). It switches live with the in-game VSync and limit; monitors
  above 120 Hz, VSync off and lower limits keep the earlier pacing. New frame rate limits 75, 90
  and 100 (launcher and in-game menu) for those monitors and variable refresh rate screens.
- **Uncharted 3: no freeze on the Cartagena rooftops (chapter 2).** The game froze about 30
  seconds into the rooftop chase at 120 FPS: it cleared a field of its next ray-cast batch in
  memory still guarded for a GPU result on its way back; the port's fault handler let the write
  fault again and again, and the result it waited for was never collected because the GPU
  thread was idle waiting for the game. Such a write now collects the result at once (or waits
  for the GPU when it is still running). Shorter versions of the same loop, about 30 a second
  in that area, were small hitches in all three games. While the GPU thread waits for the GPU
  at the end of a frame it now also collects finished results (fewer long frames at high
  frame rates).
- **Cutscenes play at normal speed with the frame rate above 60.** The Collection's cutscenes are
  prerecorded movies that show one picture per screen refresh; with the frame rate unlocked
  (120) they ran fast and out of sync with their sound (reported in Uncharted 3). While a movie
  plays the game now gets 60 Hz refreshes; gameplay keeps the chosen frame rate. (The same fix
  as in SuperMedo's Uncharted port.)
- **Anti-aliasing: TAA, AMD FSR 3.1 and FSR 4 Native AA, NVIDIA DLAA (all three games).** In-game
  menu -> Image -> Anti-aliasing, or launcher -> Display -> Anti-aliasing: Off (the game's own, as
  before), TAA, FSR 3.1, FSR 4 (listed when the GPU can run its 8-bit model; tested on an RTX 5080)
  or DLAA (NVIDIA RTX), with sharpening for FSR and DLAA. It works at any render resolution, ultrawide
  included, and switches at once from the menu. Jagged and shimmering edges (roof tiles, railings,
  foliage) are much calmer: edge flicker about halved in Uncharted 2, a third of what it was in
  Uncharted 3 (frame-to-frame measurements). The camera's motion comes from the games' own
  motion-blur velocity pass, an idea from SuperMedo's Uncharted port. Turning the camera leaves no
  trails; TAA softens the picture slightly. DLAA calms edges the most (Uncharted 2: a third of the
  flicker). Cost on an RTX 5080 at 4K in Uncharted 2: TAA and FSR 3.1 kept the 120 FPS cap, DLAA
  ran at about 84 FPS (NVIDIA's model, about 3.6 ms a frame at 4K) and kept 120 FPS at 1440p.
  The package carries NVIDIA's `nvngx_dlss.dll` (DLSS SDK 310.9.16) and the FSR 4 native model
  (tools/fetch_aa_files.sh fetches them for builds). UNCHARTED_TEMPORAL_AA=0 turns it all off.
- **Ultrawide monitors (GitHub #7).** Launcher -> Display -> Aspect ratio: 16:9, 21:9 or 32:9. The
  render resolution's height is kept and the picture widened (1080p at 21:9 is 2560x1080, 1440p
  is 3440x1440); all three games show more to the sides with nothing stretched, the HUD and menus
  keep their layout, aiming stays centred. Use Borderless or Fullscreen on the ultrawide monitor.
  Drake's Fortune, which runs out of GPU memory above about 1620p, steps down in the same shape.
- **Draw distance options (Uncharted 2 and 3, experimental).** In-game menu -> Image -> Draw
  distance: object fade distance (up to 4x the game's own, which fades objects and details in at
  0.58 of their authored distance), sun shadow distance and lamp/torch shadow distance (up to 3x
  the game's 40 m). Applied live; higher values cost some FPS.
- **Fewer hitches after loading (shader compiles).** On NVIDIA GPUs the cached shaders were only
  prepared when a level first needed them, and compute shaders were translated in the middle of
  the frame: Uncharted 3 hitched for 90-170 ms several times right after a load. The shader cache
  now loads in the background while the title screen shows (2,400+ pipelines in 7-16 s), so none
  are compiled in play. GOW3_ON_DEMAND_PIPELINES=1 keeps the old start.
- **In-game menu: your own shortcut (GitHub #15, #19).** Controls -> Menu shortcut: LT + right
  stick click (default), View + Menu together (no clash with the sniper zoom), none, or Custom:
  select "Custom menu shortcut" and hold any two controller buttons (or one the game does not
  use, such as Guide or a paddle). "Menu key (keyboard)" changes Insert to any key. The menu's
  title shows the current shortcuts.
- **Uncharted 3: no crash climbing the fort in chapter 8 (GitHub #13).** When the point Nate looks
  at became invalid (NaN), the game stopped (`m_lookAt.X() == m_lookAt.X()` in UpdateFocus); now
  it resets that point the way the game already does when it is far off.
- **Fewer crashes in all three games (GitHub #11, #12, #17).** The most reported crash, the game's
  check that the GPU finished frame N-3 (`m_gfxEopTick` in FrameBegin), came after short stalls on
  slower or busier PCs: the game's GPU event thread had not marked that frame done yet. The game
  now waits for it (a brief hitch at most) instead of stopping. UNCHARTED_FRAME_WAIT=0 keeps the
  original check.
- **The aim sensitivity sliders go higher in all three games.** Options -> Controls -> Aiming ->
  Sensitivity now reaches five times its old maximum in Uncharted 1 and 2 (a notch is half the old
  maximum, the second notch the old right end) and four times in Uncharted 3 (the left half keeps
  its fine steps, the right half goes up to 4x), for fast aiming with the mouse or a controller.
  (The games' own range checks are lifted; UNCHARTED_AIM_RANGE=0 keeps the original sliders.)
- **Controls in the in-game menu.** Insert (or LT + right stick click) -> Controls: rebind every
  action for keyboard, mouse and controller (select a slot, press the new key or button), mouse on
  and off, mouse sensitivity, invert, button icons, a controller camera speed (Uncharted 2 and 3,
  not yet in every camera) and Reset. Changes apply at once and are kept in the game's settings
  file, which the launcher's Controls tab now reads and writes too.
- **Keyboard and mouse button icons in all three games.** While you play with the keyboard and
  mouse, every button icon the games draw in their text shows your keys and mouse buttons instead
  of the PlayStation buttons: tutorial hints ("Press SPACE to jump"), menu legends (SPACE Select,
  CTRL Back, ESC Resume), the sticks (WASD, the mouse) and the d-pad (1-4). The icons are drawn
  from your own bindings, so remapped keys show up too. The prompts the games draw in the world
  (the Triangle beside a chest, the round Square/Circle/Cross buttons, Uncharted 3's L1/R1/L2/R2)
  change too: the same button picture with your key in place of the PlayStation symbol, or a mouse
  for the mouse buttons. Touch a controller and its icons come back at once; use the keyboard
  again and they switch back. Xbox and other controllers get their own buttons (A B X Y in their
  colours, LB RB LT RT, Menu, View); PlayStation controllers keep the game's own icons. Launcher:
  Controls -> Button icons: Auto (the device you use) / Keyboard and mouse / PlayStation / Xbox.
- **Launcher: controller bindings with button icons.** The Controls tab shows each action's
  controller button as an Xbox or PlayStation icon (Auto: the controller you have connected).
  Click a controller slot, then press a button on the controller to rebind it; right-click clears
  it; a button already in use swaps with the old one. The triggers and sticks stay Aim, Fire, Move
  and Look. The in-game icons follow your bindings. The launcher window can now be resized.
- **Uncharted 2 and 3: raw mouse look.** The mouse turns the camera directly (1:1, no stick
  emulation, no acceleration or dead zone), at the same speed horizontally and vertically;
  Sensitivity and Invert Y in the launcher apply. Uncharted 1 still uses the mouse as a stick.
- **Low-end PCs: the games no longer take every core of a 4-6 core CPU.** The engine's six job
  threads spin while waiting for work (on the PS4 each has its own core). On 4 cores they used 390%
  of 400% CPU and starved the GPU thread: Uncharted 3's title screen ran at 19-27 FPS on an RTX
  5080, its first level at 5-7 FPS on an integrated GPU. The engine has its own sleep mode for idle
  job threads (a job wakes them); it is now always on (launcher: Performance -> Idle game threads:
  Sleep / Spin). 4 cores: 60 FPS at the title and in Uncharted 1 and 3, about 1.5 cores used;
  integrated GPU: 27 FPS at 720p (was 5-7). 16-core desktop: Uncharted 3 uses about 2.3 cores
  instead of 6.4, and Uncharted 2's heaviest view runs 76.5 -> 78 FPS (uncapped).
- **540p render resolution** (launcher, Display) for integrated GPUs, with FSR 1 to the screen:
  tested in all three games; Uncharted 3 on an AMD integrated GPU 27.5 -> 33 FPS against 720p.
- **Less GPU command thread work per draw:** the resource-table checks before every draw asked
  the memory map for every table (6% of that thread); confirmed mapped areas are now remembered
  until memory is unmapped (the checks went from 12% to 6.5% of the thread in Uncharted 3).
- **Fewer crashes from half-written resource tables:** a draw whose tables held garbage while the
  game was still filling them made the shader compiler stop the game ("Thread ID buffer addressing
  is not supported outside of compute"), and one bad pointer turned that table read into zero for
  the rest of the session. Such a draw is now drawn once with the bad entries empty.
- **Uncharted 3: Sully's shirt flashing as big flat-coloured triangles fixed** (at 40-60 FPS; it
  never happened at 30). The cloth simulation shaders sum values in shared GPU memory the way the
  PS4's 64-lane waves allow: without waiting between steps inside one wave. NVIDIA runs each wave
  as two 32-lane halves, which raced, so the shirt exploded on every other frame whenever the game
  used 2 cloth sub-steps (40-60 FPS; at 30 FPS it uses 3). Both cloth solvers (the shirt uses two)
  now get the waits, including inside the loop that repeats until the cloth settles (a value read
  at the end of one pass was overwritten at the start of the next). On an AMD GPU it never happened.
  Only those two shaders recompile, the rest of the shader cache stays.
- **Uncharted 3: Nate's legs flickering and NPCs floating a little fixed.** Like Uncharted 2 in
  0.1.2: the results of the game's small GPU copies now come back with the frame's "done" fence.
- **Much faster at 4K and in crowds (all three games), measured on a Ryzen 9 9950X3D + RTX 5080,
  frame limit 120.** The heavy scenes are limited by the CPU thread that feeds the GPU, not by the
  GPU, so this helps every PC (render resolution makes no difference there):
  - Uncharted 3 re-copied every crowd character's mesh data (about 43 MB per frame) from game memory
    on every use, because each piece is small enough to be treated like per-frame constants. Data
    that stays the same is now kept on the GPU and only copied again if the game changes it.
  - Fence values the GPU writes for compute work landed on memory pages that also hold GPU data, and
    each such write waited for the GPU (about 1.7 ms, ~40 times a second): now written directly.
  - Fewer barrier checks (overlapping views of one buffer counted once), lock-free checks before
    each draw, fewer thread wakeups for GPU interrupts, faster page fetches on a second GPU queue.
  - Results at 4K: Uncharted 3's Cartagena market 64 -> 79 FPS at the checkpoint and 69 -> 90 in
    the crowd (the items above; the second-queue fetches before them took a crowded street 46 -> 58),
    Uncharted 2's heaviest view 38 -> 99 FPS, Drake's Fortune 95 -> 101 FPS.
- **Multithreaded rendering is now much faster** (launcher, per game):
  compute fences no longer stop the recording thread. 4K: Uncharted 3 100 FPS, Uncharted 2 118,
  Drake's Fortune 117 (the 120 limit). On 4 CPU cores Uncharted 3 at 1080p runs at the 120 limit
  (90 without). 5 minutes of Uncharted 3 with it: no errors.

### Known issues (0.2.0)

- **Drake's Fortune:** the grenade launcher can kill the player (GitHub #6). Being looked at.
- Uncharted 3 on an AMD integrated GPU stopped shortly after a level started in earlier builds (the
  game waited for an internal job): not seen any more in a 6-minute test with the rooftop freeze
  fix (probably the same cause). Please report it if it still happens.

## 16 October 2026 (0.1.2)

- **Uncharted 2: blurry background while aiming fixed.** The aiming depth of field reads its focus
  distance from a 1x1 texture the GPU renders; it was never copied back to the game (the distance
  stayed 0, so everything behind Nate was out of focus). Tiny GPU-rendered textures now come back
  with the frame's "done" fence (Uncharted 2 only; the other two games do not need it). Above 1080p
  the game also measured the depth at the wrong spot (the crosshair's 1080p HUD position over the
  larger render): that position is now scaled to the render size, so it is right at every
  resolution.
- **Uncharted 2: GPU hang ("Device lost") after a long session fixed.** The treasure pickup effect
  takes some buffer descriptors from table slots that hold other data; one that pointed into mapped
  memory with ~1e9 records hung the GPU. Such descriptors (unaligned, huge stride or over 64 MiB)
  are now ignored for its two shaders.
- **Multithreaded rendering:** the Uncharted 3 crash within minutes (flips overtook the frame's
  fences; the GPU-written range set was read and changed by two threads) and the Uncharted 2
  floating NPCs (fences did not wait for their readbacks on the recording thread) are fixed;
  Uncharted 3 ran 8 minutes crash-free. Still experimental.
- **Faster heavy scenes:** optimizations that were off since the grey-world and floating hunts
  (threaded recording, draw preparation, prepared resources, deferred stream copies, parallel
  copies) are on again by default: heaviest Uncharted 2 view 43.8 -> 49.0 FPS, Uncharted 3 about 9%
  less GPU-command-thread work.
- **74-85% less GPU readback in all three games:** windows the CPU stopped writing into cool down
  after 3 s, and small GPU writes come back on their own instead of with the whole 64 KiB around
  them (the effect shaders' data the game never reads made up nearly all of it). Uncharted 2
  2.9 -> 0.46 GB per 5 s, Uncharted 3 3.1 -> 0.48, Drake's Fortune 0.63 -> 0.16.
- **Frame rate limits 40, 45, 50 and 55** in the in-game menu and the launcher.
- Profiler (`GOW3_PROFILE`): no longer freezes or crashes the game (own lock-free stack unwinder).
- New diagnostics: `GOW3_READBACK_STATS=1` (readback bytes per window, why and who wrote them),
  `GOW3_TRACE_PS=<hashes>` (render targets, viewport, textures and constants of a shader's draws).

- **Uncharted 3: crash in the Cartagena museum and characters flying around fixed.** GPU results
  were copied back to the game without protection: a late copy could land on data the game had
  just written (garbage pointers, a broken GPU command list). New GPU data is now read-protected
  until it is back; the game's reads of it wait, everything else runs on.
- **Uncharted 2: NPCs coming from the sky fixed.** The game places characters on the ground with
  GPU ray casts and reads the results once a GPU "done" counter is set. That counter was written
  when the command was recorded, before the GPU had run it, and a shortcut from 15 October threw
  nearly every result away. The counter is now written once the results are back in game memory,
  in strict order with the frame's other counters (as on the console).
- **Uncharted 2: NPCs floating in the air, Nate's feet sinking into the floor and NPC legs
  flickering while they walk fixed** (also in 0.1.1). The game reads back data that a GPU copy
  shader writes every frame; the port returned it with the next submission, a frame late. Those
  small copies now come back before the frame's "done" fence the game waits for (no FPS cost; the
  shader's large copies, which the game never reads, are left out). Every preset queues 1 frame
  again; the launcher moves preset settings back from 2 to 1.
- **Faster heavy scenes:** guest-memory copies for the GPU no longer take a lock (10-15% of the GPU
  command thread in heavy Uncharted 2 scenes: 7.9 -> 6.4 microseconds per draw); GPU checkpoints
  (crash diagnosis, ~2%) only on request (`UNCHARTED_GPU_CHECKPOINTS=1`).
- Protected copy-back without the cost: pages where the game reads its own data beside GPU results
  and results the game reads every frame (such as scene brightness) stay readable; Uncharted 2
  went from 33-46 to 60 FPS with protection on. A deadlock that hung Uncharted 3 while loading
  with protection on is fixed (uploads copy through an unprotected view of the same memory).
- Tracking of small GPU writes no longer grows over a session.


- **First release, 0.1.0 (beta).** The package now carries `LICENSE.txt`, `CHANGELOG.md` and
  the licenses of all bundled libraries (`licenses/`).
- **Faster heavy scenes with every preset: 2 frames queued** (Low and Medium had 1). With 1 the
  CPU waited for the GPU to finish the previous frame every frame; in Uncharted 2's museum
  (1080p) that was 44-47 FPS, with 2 it is 56-59. The launcher moves existing settings over
  (not for a game with multithreaded rendering, which Uncharted 2 needs with 1).
- The game and launcher windows name the port's author in their title bar.
- **Launcher updates:** at start the launcher looks for a newer release and offers
  **UPDATE TO x.y.z**; it downloads the package, checks its SHA-256 against the one GitHub publishes,
  installs it with the new version's own installer (`tools/apply_update.py`: saves, settings and the
  shader cache stay; on any error every replaced file is put back) and restarts. Advanced tab: the
  check can be turned off or run now. Tested against a local fake release (install, restart,
  cleanup) and a locked file (full rollback of 1,077 files).
- **Automatic releases:** `tools/release.sh X.Y.Z` pushes the tag `vX.Y.Z`; GitHub Actions builds
  the package and publishes the release (marked Latest) with the notes from `tools/release-notes/vX.Y.Z.md`.
- Profiler: call stacks (`profile_*_stacks.txt`), also of the game's own thread that submits the
  frames, so a wait shows who waits.
- Packaging: the license step no longer hangs on GitHub Actions (the MSYS2 root `/` made
  `//usr/bin/pacman` a network path).
- **Uncharted 2: Nate floating above the ground or invisible fixed.** A CPU shortcut for one of
  the game's small GPU copy shaders (6a6422a9) copied the player's data before the game had
  finished it; that copy runs on the GPU again.
- **Uncharted 2: crash with multithreaded rendering fixed** (game assert `m_gfxEopTick` in
  `FrameBegin` after 1-3 minutes): GPU memory copies and fills no longer overtake the frame's
  end-of-pipe timestamp. Still open with 2 frames queued, see the known issues.
- **Heavy scenes: stalls removed.** CPU writes into ring memory whose old GPU data was still being
  read back waited for the GPU (a third of each frame with multithreaded rendering); the
  per-draw stream buffer grew from 128 to 512 MiB (it filled up with 2 frames queued);
  "Frames queued 2" is now the High preset's default. Heavy Uncharted 2 scenes: about 33 -> 40-45
  FPS (4K, multithreaded off).
- Built-in sampling profiler for the GPU threads (`GOW3_PROFILE=<seconds>`).
- **Drake's Fortune: bullet impacts, bullet holes and blood.** The game traces its bullet and
  decal rays on the GPU (`Cs_RayTrace`, `Cs_RayTraceGatherHits`) and reads the hits once the
  frame's end-of-pipe event fires. That event was signalled before the GPU had produced the
  hits, so every shot read as a miss. The results are now copied back asynchronously with their
  pages read-protected until they arrive.
- **Drake's Fortune: cutscene movie colours at 1620p** (grey picture with diagonal coloured
  lines). The GPU reads the movie's colour plane with rows padded to 64 texels (1472 at 1620p);
  the scaled frame copy now writes its rows the same way.
- **Frame-rate limit capped at 120.** At 140-160 FPS Drake's Fortune's movement broke (Nate fell
  through the world). The launcher and the in-game menu offer 30 / 60 / 120; older, faster
  settings are read as 120.
- **Launcher and in-game menu settings stay in step.** Closing the in-game menu no longer
  overwrites a change made in the launcher while the game runs, and the launcher shows what the
  in-game menu saved.
- **Uncharted 2: crashes at treasure pickups and in the Treasures screen fixed.** CPU-rewritten
  per-frame data (skinning tables) was corrupted by a byte-compare merge of GPU data; the merge
  is gone, and resource descriptors that cannot be real are ignored instead of crashing the GPU.
- **Uncharted 2: frame rate no longer falls over time.** In a busy scene it slid from about 42 to
  31 FPS within minutes: a per-page list of GPU writes grew every frame and was scanned on every
  buffer binding. Found with a new built-in sampling profiler (`GOW3_PROFILE`); the frame rate now
  stays level.
- **Frame drops in heavy scenes reduced.** CPU writes next to GPU data in flight no longer wait
  for the GPU; scalar copy shaders run on the CPU; GPU readbacks are asynchronous.
- **Uncharted 2: torch follows Nate** (launcher option, Advanced tab). *In test.*
- **Update 1.02** used automatically when it sits next to the game folder.
- **Render resolution above 1080p:** Uncharted 2 and 3 up to 4K, Drake's Fortune up to 1620p with
  FSR 1 (its memory layout); cutscene movies scaled to the render size.
- Keyboard and mouse: mouse look and remappable controls per game; display modes fixed.

## 14 October 2026

- **Grey/black ground and unlit Nate fixed:** constants the GPU writes are synced back to the CPU.
- **Sound effects** (gunfire, footsteps, ambience) fixed.
- **Dark interiors:** fire and flashlight light fixed; bright hair and cobwebs fixed (MUBUF ADDR64
  loads).
- Uncharted 2 and 3 start from the Collection menu; the Uncharted 2 new-game freeze fixed.
- The Uncharted launcher: per-game settings, quality presets, render resolution.

## 13 October 2026

- Gameplay reached: the Collection's game code runs natively on Windows.
