# Working on this repository

A port of the Star Trek: Voyager arcade game (Linux x86 ELF, GCC 2.95, full
STABS) to macOS and iOS by static translation plus a native Linux / GL host.
Read [README.md](README.md), [PLAN.md](PLAN.md) and [PHASE0.md](PHASE0.md).

## Rules

- Nothing from the game folder is committed: no binaries, frames, textures,
  audio, generated translations (`build/x86tr`) or the STABS header
  (`build/decomp`). Hashes and file names in `manifests/` are fine.
- Never hand-edit generated code. Fixes go in `tools/x86tr`, in
  `OVERRIDES_BY_NAME` (native replacements, `src/host/natives.cpp`), or in
  `src/decomp` (decompiled functions, `VOY_DECOMP(program, name)`).
- After changing the translator or adding a decompiled function, run
  `tools/prepare.sh "<game folder>"` before building.
- Unported behaviour fails closed (`host::fail_closed`) or is noted once
  (`host::note_once`); do not guess.
- A decompiled function must pass `decomp_diff`; add its signature to
  `tests/decomp_diff.cpp`.
- Settings (env vars, app defaults, vgrun options) are documented in the
  README "Settings and debugging" table; update it in the same commit.
- Check changes headless first: `vgrun --frames N --frame-dump` writes PNGs
  without a window; `ctest --preset mac-host` with `VOY_GAME_DIR` set.

## Things learned the hard way

- The cabinet ran X at **512 × 384**; the game refuses 640 × 480
  (`mj_set_numlines`). Gun coordinates are in the same space, y down.
- Headless runs use virtual time, and the guest's `rdtsc` must follow it
  (`host::clock_ns`), or the game's timers stand still.
- The dump's file names do not match the case the game asks for; lookups are
  case-insensitive (`Machine::map_read`). Case-insensitive Mac volumes hide
  this, iOS does not.
- Hand-written assembly labels (`get_curr_cpu_time`) are untyped symbols; the
  discovery includes them.
- GCC 2.95 did not know `bailing()` never returns, so code after its calls is
  real: only libc's own no-return functions are marked.
- The Metal vertex struct must be packed (40 bytes) to match `gl::Vertex`.

## Next steps

1. Gameplay polish on the Mac: play a full game with mouse and controller;
   compare sound levels with the cabinet; check remodulate prompts.
2. Decompile more of `src/decomp`, file by file (3dmath.c, spline.c, then the
   loaders), each with `decomp_diff` signatures.
3. iOS on a device: signing, memory footprint, frame pacing.
4. Widescreen and higher internal resolution (the renderer's `scale`).
