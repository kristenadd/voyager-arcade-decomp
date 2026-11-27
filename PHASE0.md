# Phase 0 — inventory and go / no-go

Source folder (read-only, nothing copied): `../Star Trek Voyager (2004-04-02)[Team Play PC][TP]/`,
called **the dump**.

Method: ELF headers and dynamic symbols (`llvm-readelf`, `llvm-nm`), a STABS reader
(`tools/stabs/stabs.py`), disassembly (`llvm-objdump`), Ghidra pseudocode through REA for
the hardware-facing functions, and a frame walker for the background video
(`tools/formats/segdump.py`).

## Verdict: GO

Every program the cabinet runs is a dynamically linked **ELF32 i386** built with
**GCC 2.95.3** on Slackware (glibc 2.2.3). None of them is stripped, and each carries full
**STABS** debug info.

| Program | Size | Functions (with STABS) | Source files | Imports | Role |
|---|---|---|---|---|---|
| `trek` (= `trek.e4`) | 1,553,802 | 1,739 (1,590) | 79 | libc, libm, GL, GLU, GLUT | The game |
| `stvsnd` | 122,358 | 208 (183) | 8 | libc only | **Software sound synthesizer**: reads sound codes from stdin and writes PCM to OSS `/dev/dsp` |
| `stvserv` (= `stvserv.e4`) | 367,613 | 642 (521) | 25 | libc, libm, GL, GLU, GLUT | Operator menu: audits, display, sound and gun tests, settings |
| `ssv` | 37,846 | 18 | 2 | libc | Supervisor: runs `trek`, restarts it after a crash, runs `stvserv` when it exits normally |

Hashes are in `manifests/voyager_e4.txt` (`tools/make_manifests.py`). Of the alternative builds,
`.e3` has the same size as e4 but a different hash; `.e1`, `.f`, `.d2` and `.010d` are older.
The port targets **e4** only.

`trek` layout: base 0x08048000, `.text` 0x0804A4E0–0x0809D6BC (340 KB), `.rodata`, `.data`,
and a **15 MB `.bss`**. There are no relocations beyond the PLT / GOT, no threads, no signals,
no `dlopen`, no self-modifying code and no SSE. The FPU is x87 only.
The only privileged instructions are one `in al, dx` / `out dx, al` pair in `ibck.c`.

## Execution model

```
ssv ──popen──> trek ──popen("./stvsnd /data/stv/arom/", "w")──> stvsnd ──write──> /dev/dsp (OSS)
                 │   writes "%d\n" sound codes (sndPlay → snd_code_always)
                 ├── open("/dev/mjf"): MegaJamma I/O board, read() 0xA8-byte packets, ioctl 0x40044D04 / 0x40044D07
                 ├── verify_ib(): Dallas iButton keys through port I/O (iopl or /dev/p37c)
                 ├── open("/proc/interrupts"): the NVIDIA IRQ count is the vblank counter
                 ├── tcsetattr / read(0): raw keyboard on the console
                 └── GLUT main loop: idle → glutPostRedisplay → glut_draw → run_all_tasks → wait_for_vblank → glutSwapBuffers
```

- `main` (main.c): `process_args` → open `/var/stv/stv.log` → `ibok = verify_ib()` →
  `init_all_static_data` → `init_keyboard` → `init_hw_specific_video_stuff` (GLUT window or
  game mode, callbacks) → system textures → `yield_to_infinite_loop(run_all_tasks,
  process_key, process_mouse, 3 or 2)`, which calls `glutMainLoop` and never returns.
- `glut_draw` (ps_loop.c): clear → `run_all_tasks` → `glFlush` → `wait_for_vblank(min_period)`
  → `glutSwapBuffers`. The game ticks once per drawn frame at 20 Hz (`min_period` 3) or
  30 Hz in turbo mode (2), against a 60 Hz vblank.
- `wait_for_vblank` busy-waits on `get_frame_number`, which parses `/proc/interrupts`.
  **The port replaces both natively** with a host 60 Hz counter and a real wait.

## Arcade services and their replacements

| Original | Evidence | Port |
|---|---|---|
| **MegaJamma** I/O board, `/dev/mjf` | `mj_read_packet` reads 0xA8-byte packets. `mj_do_packet`: word 0 is shot bits (`shotmask`), word 1 the switches, words 4–11 the raw gun samples (x in bits 0–11, y in bits 12–23, a 4-deep ring per gun, count in `pulsemask` / `pulseshift` bits of word 0), word 3 `irqcnt` (must increase), words 12–13 the coin counters, word 14 `mjver`. The ioctls are iButton read (0x40044D04) and error status (0x40044D07). Mouse emulation exists: `mj_process_mouse_as_a_gun_shot` → `gun_handle_func(gun, x, y)` | Native device: `open("/dev/mjf")` returns a host fd; `read` returns one packet per tick built from host input; ioctls succeed with no error. Shots call the game's own `gun_handle_func` path through the packet. |
| iButton dongle (`ibck.c`, `verify_ib`, `keyok`) | Reads two keys with port I/O. The result is kept in `ibok`. | `verify_ib` is replaced by a native function that returns 1 (an authenticated cabinet), as TeknoParrot-style loaders do. The `in` / `out` instructions are never reached. |
| Sound board | `snd_init_new` popens `stvsnd` and writes codes. `stvsnd` is a full sequencer (`stv_snd_opc.c`, 100+ opcodes) and a BSMT2000-style sample player (`stv_snd_bsmt.c`) mixing in software from `voyager0-3.bin` into **OSS `/dev/dsp`** (`setup_dsp`, "Sound Card Supported Formats") | Translate `stvsnd` as a second guest program in its own guest memory and thread; the `popen` pipe becomes an in-process queue; `/dev/dsp` writes go to Core Audio. **No sound board emulation is needed.** |
| Vblank (`ps_nvvbl.c`) | `/proc/interrupts` "nvidia" line | Native `get_frame_number` / `wait_for_vblank` from the host clock. |
| Keyboard (`ps_keyboard.c`) | termios raw mode on fd 0, `read(0, &c, 1)` | `tcgetattr` / `tcsetattr` succeed; `read(0)` returns host keys (debug keys only). |
| Coins / credits (`coin.c`, `jamma.c`) | Coin counters in packets; `coinFreePlay`; `SCoinState.FreePlay_who` | Coin keys bump the packet counters; free play is a setting. |
| Settings | `/var/stv/stv.settings`, `stv.audits`, `stv.hiscores`, `stv.log`, `audits.dat` | Paths are mapped to the app container (first run copies the dump's files). |
| Data | `/data/stv/{anims,behaviors,models,segments,textures,arom}/` | Mapped read-only onto the dump. |

## Background video (`.seg`), confirmed

`segFileUpdate` and `LoadColorDepth` (segment.c). A segment is a stream of frames, each:

| Field | When | Size |
|---|---|---|
| `u32 flags` | always | 4 |
| camera position `float[3]` + rotation `float[4]` | flags & 1 | 28 |
| palette (256 × RGB, 6-bit) + four index tiles 256×256, 256×256, 256×128, 256×128 (= 512×384) | flags & 2 | 0x30300 |
| depth: "tiny" 0xC71 words (flags & 0x10) or full 512×384 bytes | flags & 4 | 12,740 / 0x30000 |
| event count + events (`segLoadFrameEvent`) | flags & 8 | variable |

`attract.seg` walks to exactly its end: 659 frames, flags 0x12 (color only) after a
first frame with a camera. A decoded frame shows the expected attract
screen, saved to a scratch folder only. The tiles are uploaded as four 8-bit textures through
`replace_tex_color_index` with the palette, so the video goes through GL like everything else.
**No video decoder is needed.**

## Formats the port does not need to understand

Models (`.mdl`), animations (`.anm`), behaviours (`.bhs`), events (`.evt`, `.seq`) and textures
(`.grx`, TGA with `rle_fread`) are all loaded by the game's own code with `fopen` / `fread`. The
translated loaders handle them; the port only maps paths.

## Graphics API surface

OpenGL 1.x immediate mode: `glBegin` / `glEnd`, `glVertex2f/3f`, `glNormal3f`, `glTexCoord2f`,
`glColor3f/4f/4ubv`, the matrix stack (`glFrustum`, `glOrtho`, `glMultMatrixd/f`, …),
`glLightfv`, `glLightModelfv`, `glMaterialfv`, `glTexImage1D/2D`, `glTexSubImage2D`,
`glTexEnvf`, `glBlendFunc`, `glDepthFunc/Mask`, `glColorMask`, `glPolygonOffset`, `glScissor`,
`glCullFace`, `glShadeModel`, `glGetFloatv/Integerv`; GLU `gluPerspective`, `gluLookAt`,
`gluBuild2DMipmaps`; GLUT window and callbacks. There are about 70 entry points and no
extensions are called through `glXGetProcAddress`. `fsh_hack.c` is a Glide (3dfx) shim on top
of GL, so it is translated as ordinary game code.

## Inputs, remodulate

- The P1 and P2 guns are `gun_handle_func(which, x, y)` with game coordinates (640×480 space
  after calibration). A shot outside the screen reports x = y = -1 (no pulse), which is the reload.
- HUD art `p1_reload.grx`, `p1_remodulate.grx`, `p1_shoot_outside.grx`, `SND_REMODULATE`, and
  `plyrReload`. Remodulate is reached through the reload path; confirmed in play in Phase 4.

## Open unknowns (carried into later phases)

1. The exact packet bits for start / service / test / volume (`switch_is_closed`, `io_key`
   masks): read while wiring input (Phase 4).
2. How the calibration constants in `stv.settings` map raw gun samples to screen; the port
   inverts `get_gun_calibrated_x_y` (Phase 4).
3. Whether `trek` checks `ibok` anywhere besides the log (`getibok_who`).
