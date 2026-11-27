# Star Trek: Voyager — The Arcade Game: native port plan

The goal is a native port of the arcade game to macOS (Apple silicon) first, then iOS / iPadOS.
It follows the method of `outrun2-decomp` and `daytona-3-nse-decomp`:

- The repository holds **code only**. The player supplies their own game folder.
- Binaries are verified against SHA-256 manifests and read in place, never changed.
- The game code runs natively. It is first machine-translated from x86, then rewritten as C.
- Arcade services (I/O board, sound board, coin, settings) are replaced by native host services.
  Anything not understood yet **fails closed** (it is logged and counted, never guessed).

Game folder (read-only): `../Star Trek Voyager (2004-04-02)[Team Play PC][TP]/`.
It is called **the dump** below.

---

## 1. What discovery found

### TL;DR: GO. This is a much easier target than OutRun or Daytona.

| | OutRun 2006 / Daytona | **Voyager** |
|---|---|---|
| Format | PE32, MSVC, stripped | **ELF32 i386, GCC 2.95.3, not stripped** |
| Debug info | none (RTTI only) | **full STABS: 81 source files, every function's prototype, every struct, enum and global, 26K line records** |
| Game code | 4–20K functions, several MB | **≈ 1,690 functions, 340 KB `.text`** |
| Graphics | D3D9 / D3D11 + HLSL | **OpenGL 1.x immediate mode + GLU + GLUT** (no shaders) |
| OS layer | Win32, DirectX, threads, SEH | **plain libc, single-threaded GLUT main loop** |
| Protection | dongle / shell handshake | **none seen.** `/dev/mjf` and `/dev/p37c` are I/O hardware |
| Middleware | CRI, OpenCV, NRT … | **none** |

The STABS info includes struct layouts such as `SPlayer` and `SPlayerInput`, and enums such as
`Sounds` (306 named sound IDs), `ESegFlags`, `ESwapItem` and `EAudit`. The source file list is
`ai.c borg.c camera.c coin.c gameloop.c jamma.c mj_stv.c player.c segment.c shot.c sndnew.c
stv_io.c …`. In effect we can rebuild the original headers. That makes a true source-level C
decompilation realistic, which is the best outcome for iOS.

### The dump

| File(s) | What it is |
|---|---|
| `trek` (= `trek.e4`, sha256 `d7b1d006…`) | **The game.** 1.55 MB ELF. Build path `/home/fred/games/stvexp/src-e4/`. Imports libc, libm, GL, GLU and GLUT. Uses `iopl`, `ioctl`, `/dev/mjf` and `/dev/p37c`. |
| `trek.e1 .e3 .f .d2 .010d` | Other builds: e3 and e4 are the same size with different hashes; d2 and 010d are smaller and older. **Target `trek` (e4).** |
| `stvserv` (= `.e4`) | The **operator / service menu**: audits, display test, sound test, gun calibration, settings. Same GL/GLUT stack, 26 source files. |
| `ssv` | Supervisor. It `popen`s the game, restarts it on a crash and runs `stvserv` when the game exits normally. Replaced natively. |
| `stvsnd` | Sound tool (`stv_snd_bsmt.c`, `stv_samples.c`, `loadhex.c`). It imports only printf/sscanf, so it is a host-side builder for the sound board program, not a runtime process. **Its tables map sound IDs to samples.** |
| `stv/arom/voyager0-3.bin` (4 × 1 MB), `cpuarom.hex` | Sound board: sample ROMs (BSMT2000-style, per `stv_snd_bsmt.c`) and the sound CPU firmware. |
| `mixer diprd ibck factset showset ft` | Small utilities: volume, DIP read, backup, factory settings. Not needed. |
| `stv.settings`, `stv.audits`, `stv.hiscores`, `audits.dat` | Cabinet state. The original lives in `/var/stv/`. Port copies go to the app container. |
| `depthtable.tab` (512 KB) | Depth lookup for compositing (`segGetDepth16`). |
| `stv/segments/*.seg` (107 files, **4.7 GB**) | **Pre-rendered background video with depth.** Each frame is camera data plus color plus depth, read uncompressed by `LoadColorDepth`. Read sizes 0x30300 (likely 512×384 8-bit + 768-byte palette) and 0x30000 (likely 512×384 8-bit depth); `rle_fread` exists too. 3D enemies are drawn into the video's depth. |
| `stv/segments/*.evt / *.seq / *.inp` | Per-segment spawn events, segment sequences and recorded demo input (`demo.inp`: a gift for headless tests). |
| `stv/models` (342 `.mdl`), `anims` (467 `.anm`), `behaviors` (1,179 `.bhs`), `textures` (1,216 `.grx`, 40 MB) | Characters, animations, AI scripts and textures. Loaded with plain `fopen` / `fread` from `/data/stv/...`. |

### Arcade hardware that the code talks to

| Device | Code | Port replacement |
|---|---|---|
| **MegaJamma I/O board** (`/dev/mjf`, `jamma.c`, `mj_stv.c`) | `mj_read_packet`, `mj_update_guns`, `mj_get_latest_switches`, `jammaCoinIn`, `jammaServiceCred`, `jammaVolume` | A native MegaJamma service that answers packets from the host input state: gun X/Y and trigger for P1/P2, start, coin, service and test. |
| `/dev/p37c` + `iopl` (`stv_io.c`) | Port I/O. The game says "must exist or user must be root". | Stub that reads back a sensible idle value; log each port touched. |
| Sound board (through MegaJamma, `sndnew.c`, `sndPlay`, `snd_code_always`) | Sound commands sent to the board's CPU, which plays from the BSMT2000 ROMs | **Native:** catch `sndPlay(id)` and play samples decoded from `voyager*.bin`, using `stvsnd`'s tables, mixed to Core Audio. Fallback: emulate the board (CPU + BSMT2000; MAME's BSMT2000 core is BSD-3). |
| Vertical blank (`ps_nvvbl.c`, `wait_for_vblank`) | NVIDIA vblank ioctl | The host frame pacer (60 Hz tick, display link). |
| Keyboard (`ps_keyboard.c`, termios raw tty) | Debug keys | Native keys. |

**Useful surprises in the code:** there is already a mouse-as-gun path (`glut_mouse`,
`process_mouse`, `mj_process_mouse_as_a_gun_shot`, `calibrate_mouse_to_gun_posn`) and a free play
mode (`coinFreePlay`, "FREE PLAY" string, `SCoinState.FreePlay_who`). **Reload and remodulate are
real mechanics:** the textures are `p1_reload.grx`, `p1_remodulate.grx` and
`p1_shoot_outside.grx`, there is a `plyrReload` function, and the sound list has `SND_REMODULATE`.
On the cabinet you reload by shooting off-screen. So a touch build needs an on-screen
"reload / remodulate" button that sends an off-screen shot. Phase 0 must confirm whether remodulate
is the same off-screen shot or a different input.

---

## 2. Approach

**A hybrid, the same as OutRun: translate first, decompile second.**

1. **Machine translation for first light.** Reuse Daytona's `tools/x86tr`
   (discover → CFG → C++ with x87 / SSE semantics). It needs:
   - an **ELF32 loader** in place of the PE one (`image.py` is about 130 lines). Base 0x08048000,
     no relocations, `.bss` is 15 MB;
   - **import thunks for libc / libm / GL / GLU / GLUT** in place of Win32 / D3D. There are
     about 100 libc functions and about 70 GL / GLUT functions, all listed in §1;
   - the **cdecl GCC 2.95 ABI** and heavy **x87** code (`fistp` with `fldcw`, `fucomp` /
     `fnstsw`). x86tr already has `x87.py`.

   The game is single-threaded, so the Daytona cooperative-thread layer isn't needed.
   STABS gives the **exact function boundaries and names**, so discovery has ground truth.
2. **Rebuilt headers.** A tool (`tools/stabs2h`) turns the STABS records into C headers: structs
   with checked offsets and sizes, enums, prototypes and globals with their addresses. Generated
   once from the pristine binary and committed. These are our own declarations, not game data.
3. **Hand decompilation to C, file by file**, following the original 81-file layout.
   Each function is checked against the translated one with differential tests (same inputs, same
   guest memory, same outputs), which is OutRun's `tests/` pattern. Line-number records let
   you diff by source line. Start with the leaf math (`3dmath.c`, `ps_math_3d.c`, `spline.c`),
   then loaders (`model.c`, `anim.c`, `ps_tga.c`, `ps_grx.c`, `segment.c`), then game logic.
   When `trek` is 100% C, the guest memory image is gone and the port is 64-bit clean.
   That matters most on iOS.

### Graphics

The game uses fixed-function GL 1.x: `glBegin`/`glEnd`, matrices, lights, materials, fog, 2D
textures, blending, scissor and polygon offset. The plan:

- **First light on the Mac:** forward the calls to the system's legacy OpenGL 2.1 compatibility
  context. It still works on Apple silicon, although it is deprecated. This is quick and gives a
  reference image.
- **The real renderer is a small fixed-function GL emulator on Metal (`src/gpu/glfixed`).** It
  records immediate-mode vertices into buffers and implements the matrix stacks, lighting,
  texenv, fog and blend state as a few MSL shader permutations. iOS has no OpenGL, so this is
  required there, and the Mac uses it too. The API surface is about 70 functions, all known.
- **Background video:** the `.seg` frames are uploaded as a texture plus a depth texture
  (via `depthtable.tab`) every frame. Streaming 4.7 GB at about 400 KB a frame is easy for
  NVMe; a background reader thread keeps it off the main loop.
- Later: resolution scale, MSAA, widescreen (the 4:3 video gets pillarboxed or extended).

### Audio

- Phase 0: read `sndnew.c` (trek side) and `stv_snd*.c` / `stv_samples.c` (stvsnd) to learn the
  command protocol and the sample table format. Decode `voyager0-3.bin`.
- Native mixer: sound ID → sample(s), with volume, pan, loops and music. Core Audio on Mac and
  iOS through the same mixer (Daytona's `ds_mixer` pattern).

### Input mapping

| Action | Keyboard + mouse | Gamepad | iOS touch (free play) |
|---|---|---|---|
| Aim P1 | mouse position | left stick moves a crosshair (with acceleration); gyro optional later | the tap point |
| Fire | left click | right trigger / A | tap on screen |
| Reload / remodulate | right click (off-screen shot) | B / left trigger | on-screen button |
| Start P1 | `1` or Enter | Start / Menu | tap during attract |
| Coin | `5` (P2: `6`) | Back / View | none: free play is forced |
| P2 | second pad, or `2` + keyboard aim | second controller | none at first |
| Service / test | `F2` / `F1` | none | none |
| Pause / quit | Esc / Cmd-Q | none | the system |

Mouse and pad both feed the MegaJamma service as calibrated gun coordinates, so the game's own
calibration and HUD work unchanged. The mouse cursor is hidden; the game's crosshair shows.

On iOS, free play is forced through `stv.settings` / `coinFreePlay`. A tap on the attract screen
sends Start. Each tap aims and fires one shot at that point. Hold-to-autofire is optional. A
translucent reload / remodulate button sits in a corner.

---

## 3. Phases

Each phase ends with something **checked in a run**, not only built. Phase documents
(`PHASE0.md`, `PHASE1.md`, …) record the evidence, like Daytona's.

### Phase 0 — Inventory and go / no-go (`PHASE0.md`)
- SHA-256 manifest of every needed file (`tools/make_manifests.py`); pick the `e4` build.
- STABS dump: file → function → address table; struct sizes; global map.
- Confirm the `.seg` frame layout (resolution, palette, depth, `rle_fread`, per-frame header,
  shared depth) by decoding a few frames to PNG in a scratch tool. Output stays out of the repo.
- Confirm the `.grx`, `.mdl`, `.anm` and `.bhs` formats from the loaders (format notes only).
- The MegaJamma packet format (`mj_read_packet`, `mj_do_packet`), the gun register layout and
  the sound command path.
- What `/dev/p37c` and the port I/O do.
- The command line (`parsargs.c`), environment variables (`getenv`) and every path the game opens.
- The remodulate mechanic: which input and which code path.
- Optional, as a reference: run the original under a 32-bit Linux VM or Docker on x86 emulation,
  with stub `/dev` nodes. That gives reference frames and sound. Worth it if cheap.

### Phase 1 — Skeleton
CMake + presets (`mac-host`, later `ios-sim` / `ios-device`), the same layout as Daytona
(`src/system`, `src/runtime`, `src/platform/{macos,ios}`, `tools/`, `tests/`).
`vgrun --content <dump>` finds the dump, verifies the manifest, maps `/data/stv/...` and
`/var/stv/...` onto the dump and the container, and builds a prepared cache.

### Phase 2 — Translator
- ELF loader, STABS-seeded discovery, libc / libm thunks, x87 coverage. Translate `trek`.
- `tools/stabs2h` headers.
- Exit test: `vgrun --headless` reaches `GameLoop` and loads the attract segment, models and
  textures with no unknown import calls.

### Phase 3 — Graphics on the Mac
- GLUT replaced by a native window and loop (`glutMainLoop`, idle and display callbacks driven by
  the host).
- First the legacy OpenGL pass-through, then the Metal fixed-function emulator; compare the two.
- Segment video plus depth compositing.
- Exit test: attract mode plays in a window and matches the reference frames by eye
  (and by a diff, if Phase 0 got a reference run).

### Phase 4 — Services
- MegaJamma: mouse, keyboard and pad as guns, coin, start, service.
- Sound board replacement; music and effects.
- Settings, audits and high scores persist in the app container.
- Free play option; vblank pacing at 60 Hz.
- `stvserv` translated too, so the operator menu works (gun calibration, settings).
- The `ssv` supervisor logic in the host: game exit → service menu → game.
- Exit test: a full game is playable with mouse and with a pad, with sound.

### Phase 5 — Automated checks
- CTest: `demo.inp` / `space_alt_demo.inp` replays run headless and compare frame hashes or
  checksums of game state; input-script smoke tests (coin, start, fire, reload); differential
  tests for decompiled functions.

### Phase 6 — Decompilation
- Replace translated functions with C file by file, each passing its differential test, until
  `trek` has no translated code left. Track progress with a `decomp_status.py`, as in OutRun.
- Then remove guest memory and become 64-bit clean.

### Phase 7 — iOS / iPadOS
- Same game and Metal renderer. A UIScene app; the dump goes in Documents (Files app), as in
  OutRun.
- Touch layer: tap to start, tap to shoot, a reload / remodulate button, free play forced.
- Controllers work as on the Mac.
- Memory: the game's own heap is small; segment frames are streamed, never fully loaded.
- 4.7 GB of segments is large for a phone. An optional repack to a compressed format
  (for example HEVC color plus lossless depth, or zstd) made on the Mac would be a
  `vgprepare` step. Raw is read first, though.

### Later
Widescreen, higher internal resolution, two players on one device, visionOS, Linux / Steam Deck
(the same SDL3 + Vulkan route as Daytona).

---

## 4. Risks and open questions

| Risk | Notes |
|---|---|
| The sound board protocol is unclear | The native sample path depends on `stvsnd`'s tables being complete. Fallback: board emulation. |
| The `.seg` format has surprises (RLE, palettes per segment) | Phase 0 decodes frames before any engine work. |
| GL behaviour details (polygon offset, fog, `glTexImage1D`, alpha test) | Compare against the legacy GL path on the same Mac, then against a Linux reference if we have one. |
| x87 precision differences | The translator keeps 80-bit semantics where the game depends on it (flagged by differential tests). |
| Which build is final | `trek` = `e4` is the installed one; e1/e3/f/d2/010d stay as references. |
| Legal | Code only, no game data, no dump-derived assets in the repo. Players need their own dump. |
| iOS storage and streaming | Raw segments are 4.7 GB; optional repack (Phase 7). |

## 5. Immediate next steps

1. Write `PHASE0.md`: the manifest, a STABS dump tool and a function / struct inventory.
2. Decode a few `.seg` frames and one `.grx` texture to confirm the formats.
3. Trace `mj_*` and `sndnew.c` to document the I/O and sound protocols.
4. Port `x86tr` to ELF and try a first translation of `trek`.
