# Star Trek: Voyager — The Arcade Game: native port

A native port of the 2002 arcade game (Team Play / Monaco Entertainment,
build e4 of 2 April 2004) to macOS on Apple silicon and iOS / iPadOS.

The cabinet was a Linux PC: the game (`trek`), its software sound synthesizer
(`stvsnd`) and its service menu (`stvserv`) are 32-bit x86 ELF programs using
OpenGL 1.x and GLUT. The port translates all three to C++ for arm64
(`tools/x86tr`, adapted from the Daytona port), runs them on a small Linux /
glibc / OpenGL host (`src/host`, `src/gl`), and replaces the cabinet's
hardware natively: the MegaJamma light-gun I/O board, the iButton key check,
the NVIDIA vblank counter and the OSS sound card. Game functions are then
rewritten as C++ one by one (`src/decomp`), each checked against its
translation.

See [PLAN.md](PLAN.md) for the plan and [PHASE0.md](PHASE0.md) for what the
binaries and data turned out to be.

**This repository holds code only.** You need your own copy of the game
folder (the e4 build: `trek` sha256 `d7b1d006…`). Nothing from it is
committed; the translated code is generated from your files at build time.

## Status

| Phase | State |
|---|---|
| 0 Inventory | Done ([PHASE0.md](PHASE0.md)) |
| 1–2 Skeleton, translator | Done: 1,745 + 208 + 642 functions translate; nothing traps outside the replaced key check |
| 3 Graphics | Done: OpenGL 1.x on the CPU (transform, lighting, culling), Metal renderer, 4× MSAA, 4:3 letterbox |
| 4 Services | Done: guns (mouse, touch, controllers), coins, start, test switch → service menu, sound (stvsnd → /dev/dsp → AudioQueue), settings / audits / scores in the container |
| 5 Tests | Done: CTest unit tests, deterministic attract replay, scripted play, service menu |
| 6 Decompilation | Started: framework, differential test, 17 functions of `ps_math_3d.c` |
| 7 iOS | Runs in the Simulator: attract, free play, tap to start; not yet tried on a device |

## Building

Requirements: Xcode 16+ (tested with Xcode 27.2), CMake 3.24+, Ninja, Python 3.
The first step translates your copy of the game (it installs Capstone into
`build/pylib`):

```bash
tools/prepare.sh "<game folder>"
```

```bash
cmake --preset mac-host
```

```bash
cmake --build --preset mac-host
```

That gives `build/mac/Voyager Arcade.app` and `build/mac/vgrun`.

## Playing on the Mac

```bash
open "build/mac/Voyager Arcade.app" --args --content "<game folder>"
```

Without `--content` the app asks for the folder once and remembers it. It
checks the folder against `manifests/voyager_e4.txt` first. Settings, audits,
high scores and logs live in `~/Library/Application Support/VoyagerArcade`.

| Action | Keyboard and mouse | Game controller |
|---|---|---|
| Aim and shoot | point and click (hold to repeat) | left (or right) stick moves the crosshair; A or right trigger fires |
| Reload / remodulate | right click, Space, or click outside the picture | B, left trigger or right shoulder |
| Start P1 / P2 | 1 or Return / 2 | Menu |
| Coin | 5 (P1), 6 (P2) | Options / View |
| Service menu (test switch) | F2 (then Start 1 changes item, Start 2 selects) | none |
| Pause / resume (the game's debug keys) | P / O | none |
| Full screen, quit | Ctrl-Cmd-F, Cmd-Q | none |

A second controller is player 2. Free play: set it in the service menu
(Coin Options), or `defaults write local.voyagerarcade.mac FreePlay -bool true`.

## iPhone and iPad

```bash
cmake --preset ios-sim
```

```bash
cmake --build --preset ios-sim
```

That gives `build/ios-sim/VoyagerArcade.app`, signed ad hoc for the
Simulator. Copy the game folder into the app's Documents folder (the Files
app, or Finder with a device connected). For the Simulator, a clone takes no
space:

```bash
cp -cR "<game folder>" "$(xcrun simctl get_app_container booted local.voyagerarcade.ios data)/Documents/Voyager"
```

Free play is forced on iOS. When player 1 is not in a game, a tap starts one;
in a game, a tap shoots where it lands. The **RELOAD** button (bottom right),
a two-finger tap or a tap on the black bars reloads and remodulates.
Controllers work as on the Mac, and the RELOAD button hides while one is
connected.

For a device, generate an Xcode project signed for your team. The app needs
the extended virtual addressing entitlement (each guest program reserves a
4 GB address space; `src/platform/ios/VoyagerArcade.entitlements`):

```bash
cmake --preset ios-device -DVOY_IOS_TEAM=<team id>
```

```bash
xcodebuild -project build/ios-device/VoyagerArcade.xcodeproj -target VoyagerArcadeIOS -configuration RelWithDebInfo -allowProvisioningUpdates
```

## Testing

```bash
VOY_GAME_DIR="<game folder>" ctest --preset mac-host
```

`unit` needs no game files. `game_attract` runs 1,500 frames of attract twice
and requires the same final frame hash, `game_play` runs
`tests/scripts/play.txt` (coin, start, shots, reload), `game_service` presses
the test switch, and `decomp_diff` compares every decompiled function with
its translation on random inputs. Without `VOY_GAME_DIR` the game tests are
skipped.

## Settings and debugging

| Setting | Where | Effect |
|---|---|---|
| `--content <dir>` / `VOY_CONTENT` | Mac app | the game folder |
| `FreePlay` (defaults) / `VOY_FREE_PLAY=1` | Mac app | free play regardless of the coin settings |
| `VOY_STDOUT=1` | Mac and iOS apps | the programs' printf output on stderr |
| `VOY_SCRIPT=<file>` | Mac app | scripted input (format in `src/host/machine.hpp`) |
| `VOY_CAPTURE=<prefix>` | Mac app | every 300th frame as a PNG |
| `VOY_GL_TRACE=<frame>` | all | logs that frame's GL calls |
| `VOY_DECOMP=0` | all | runs the translations instead of the decompiled functions |

`vgrun` runs the cabinet without a window, on a virtual 60 Hz clock (as fast
as the CPU allows, and deterministic):

```bash
build/mac/vgrun --content "<game folder>" --frames 1500 --hash-every 150 --frame-dump /tmp/f --script tests/scripts/play.txt
```

Options: `--container <dir>`, `--frames N`, `--sound` (run stvsnd too),
`--stdout`, `--free-play`, `--script <file>`, `--hash-every N`,
`--frame-dump <prefix>` (every Nth frame as PNG, with `--hash-every`).

## Layout

| Path | What |
|---|---|
| `tools/x86tr` | x86 → C++ translator (`discover.py` from the symbol table, `translate.py`) |
| `tools/stabs` | STABS inventory (`stabs.py`) and reference header (`stabs2h.py`) |
| `tools/formats/segdump.py` | walks a `.seg` video file and writes frames as PNG |
| `tools/prepare.sh`, `tools/make_manifests.py` | translation step, manifest |
| `src/recomp` | guest CPU state, memory, dispatch, natives |
| `src/host` | the Linux host: processes, libc, stdio, devices, GLUT, the supervisor (`machine.cpp`) |
| `src/gl` | OpenGL 1.x emulation |
| `src/gpu` | Metal renderer |
| `src/decomp` | decompiled game functions |
| `src/app` | content check, headless runner |
| `src/platform` | Mac app, iOS app, shared Apple pieces (audio, controllers, game thread) |
| `tests` | CTest suite |
