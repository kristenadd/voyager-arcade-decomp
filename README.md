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

The game, its sound synthesizer and its service menu run on the Mac, on
iPhone and iPad, and on Apple Vision Pro, with sound, light-gun input from
the mouse, touch, gaze and pinch, a game controller or PlayStation VR2 Sense
controllers, and the cabinet's settings, audits and high scores. Decompilation
has begun with the vector math of `ps_math_3d.c`.

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

Optionally, make the app icon from your copy's title screen (it renders the
attract mode to the title logo with `vgrun`; like the translations, it is
never committed), then configure and build again. The Mac, iOS and visionOS apps use it
when `build/icon` exists:

```bash
python3 tools/make_icon.py "<game folder>"
```

## Playing on the Mac

```bash
open "build/mac/Voyager Arcade.app" --args --content "<game folder>"
```

To install it in `/Applications` with the game inside the app, so it needs
nothing else (the game is cloned in, taking no extra space on the same disk;
the app then holds your game files, so keep it to yourself):

```bash
tools/install_mac.sh "<game folder>"
```

The app uses only macOS's own libraries and frameworks (no Homebrew); the
script refuses to install a build that links anything else.

Without `--content` or a game inside the app, it asks for the folder once and
remembers it. It
checks the folder against `manifests/voyager_e4.txt` first. Settings, audits,
high scores and logs live in `~/Library/Application Support/VoyagerArcade`.

| Action | Keyboard and mouse | Game controller |
|---|---|---|
| Aim and shoot | point and click (hold to repeat) | left (or right) stick moves the crosshair; A or right trigger fires |
| Reload / remodulate | right click, Space, or click outside the picture | B, left trigger or right shoulder |
| Start P1 / P2 | 1, Return or keypad Enter / 2 | Menu (Start) |
| Coin | 5 (P1), 6 (P2) | Options / View (Select) |
| Service menu (test switch) | F2 (then Start 1 changes item, Start 2 selects) | none |
| Pause / resume (the game's debug keys) | P / O | none |
| Full screen, quit | Ctrl-Cmd-F, Cmd-Q | none |

Two controllers give a two-player game: the first is player 1 (red
crosshair), the second player 2 (blue crosshair, Start joins as player 2).
The crosshairs show once a controller is used and hide when you click the
mouse. The Mac app runs in free play; to use the
coin settings of the service menu instead,
`defaults write local.voyagerarcade.mac FreePlay -bool false`.

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

Free play is forced on iOS. The **START** button (bottom right) starts a
game. A tap on the picture shoots where it lands; a tap anywhere outside it
(the black bars) or a two-finger tap reloads and remodulates. Controllers
work as on the Mac, two of them for two players (Menu also starts). Using one shows its crosshair and hides
the START button; touching the screen hides the crosshair and brings the
button back.

For a device, generate an Xcode project signed for your team. The app needs
the extended virtual addressing entitlement (each guest program reserves a
4 GB address space; `src/platform/ios/VoyagerArcade.entitlements`):

```bash
cmake --preset ios-device -DVOY_IOS_TEAM=<team id>
```

```bash
xcodebuild -project build/ios-device/VoyagerArcade.xcodeproj -target VoyagerArcadeIOS -configuration RelWithDebInfo -allowProvisioningUpdates
```

Install it and copy the game folder (4.8 GB) into the app over the cable;
`xcrun devicectl list devices` gives the device id:

```bash
xcrun devicectl device install app --device <device id> build/ios-device/RelWithDebInfo-iphoneos/VoyagerArcade.app
```

```bash
xcrun devicectl device copy to --device <device id> --domain-type appDataContainer --domain-identifier local.voyagerarcade.ios --source "<game folder>" --destination Documents/Voyager
```

Reinstalling keeps the game files; deleting the app removes them.

## Apple Vision Pro

The iOS app also builds natively for visionOS. It opens in a floating
window that you can move and resize (it keeps its 16:10 shape, the 4:3
picture between black bars):

```bash
cmake --preset visionos-sim
```

```bash
cmake --build --preset visionos-sim
```

For the headset, as for an iPhone (the same entitlements, the same bundle
id):

```bash
cmake --preset visionos-device -DVOY_IOS_TEAM=<team id>
```

```bash
xcodebuild -project build/visionos-device/VoyagerArcade.xcodeproj -target VoyagerArcadeIOS -configuration RelWithDebInfo -allowProvisioningUpdates
```

Then install it and copy the game folder with `devicectl` as for an iPhone
(the app is `build/visionos-device/RelWithDebInfo-xros/VoyagerArcade.app`).

| Action | Eyes and hands | Game controller | PlayStation VR2 Sense |
|---|---|---|---|
| Shoot | look and pinch | right trigger or A | point at the picture and pull the trigger (untracked: aim the crosshair with the thumbstick) |
| Reload / remodulate | look at a black bar and pinch, or pinch with both hands | left trigger, B or right shoulder | grip, or point off the picture and pull the trigger |
| Start | START (right bar) | Menu | Cross or Square (or Options / Create) |
| Insert coin | | Options / View | Circle or Triangle |

Each Sense controller is a light gun: the right-hand one is player 1 and the
left-hand one player 2 (a lone one is player 1); gamepads take the places
left. The crosshair follows where a Sense controller points once the app has
the accessory tracking permission it asks for on first use (Settings,
Privacy & Security, if it was declined).

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
| `--content <dir>` / `VOY_CONTENT` | Mac app | the game folder (before `Contents/Resources/Game` in the app, then the remembered folder) |
| `FreePlay` (defaults, on unless set to false) / `VOY_FREE_PLAY=1` | Mac app | free play regardless of the coin settings |
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
| `tools/make_icon.py` | app icon from the game's title screen (`build/icon`) |
| `tools/install_mac.sh` | installs the Mac app with the game inside it |
| `src/recomp` | guest CPU state, memory, dispatch, natives |
| `src/host` | the Linux host: processes, libc, stdio, devices, GLUT, the supervisor (`machine.cpp`) |
| `src/gl` | OpenGL 1.x emulation |
| `src/gpu` | Metal renderer |
| `src/decomp` | decompiled game functions |
| `src/app` | content check, headless runner |
| `src/platform` | Mac app, iOS and visionOS app (`ios`, with `visionos/Info.plist.in`), shared Apple pieces (audio, controllers, game thread) |
| `tests` | CTest suite |

## License

The port's own code is under the MIT License ([LICENSE](LICENSE)). It covers
none of the game: Star Trek: Voyager arcade's program, art, sound and data are
not in this repository and stay under their owners' rights; you need your own
copy of the game files.
