# 270: Switch speed: bring the port up to main, then cut CPU work

Status: in progress

Branch `perf/switch-270` from `main` bbe4acc5 (release 0.8.10), planned 2026-11-16, step 0 done, the rest waits for a console.
This file is the lane's plan while it runs and its finding when it merges. Someone who has only
this file and the git log must be able to pick the lane up.

## Goal and acceptance

The Switch homebrew build (a `switch-arm64` target, one commit `58402d62` on a contributor's
`switch-homebrew` branch, based on 0.7.6 and not yet in this repository) holds 60 fields a second
in a race. Today finding 127 on that branch measures 47.9 docked at scale 2 and 54.0 handheld at
scale 1.

Acceptance, each number measured on a named console with the clocks written down:

- Race fields 6000-7000 on `bench_race.py`'s route: fields/s, p50, p99 and max field time, before
  and after every kept change.
- Boot (600 fields) and race (5000 fields) state traces equal to the base build's. The branch's
  reference trace comes from a test-only Intel Mac build, so a match shows agreement with that
  build, not with retail; say so in the result.
- `python tools/check.py quick`, `port` and `docs`.
- A change is kept only when the whole race gains about 3 % or more, clearly outside the runs'
  spread (the rule of findings 247 and 249).

Out of scope: gameplay, the picture's content, online, Discord and texture packs on the Switch
(left out of the first port), a release of the Switch build.

## Where the port stands (finding 127 on the branch)

Built so far only on an Intel Mac. The target uses devkitA64's GCC and libnx, a deko3d renderer in
C (`port/nx/src/dhd_render_nx.c`, a copy of 0.7.6's Rust native renderer), an SDL3 shim on libnx,
and `port/nx/src/settings_nx.c` in place of the Rust settings crate. `port/tools/nx_fulltest.py`
runs an unattended test; it has never run on a console.

| Case | CPU / GPU / memory (MHz) | Scale | Fields/s | p50 (ms) | Max (ms) |
|---|---|---|---|---|---|
| Docked or official charger | 1785 / 768 / 1600 | 2 | 47.9 | 20.7 | 73.1 |
| Handheld | 1785 / 460 / 1600 | 1 | 54.0 | 17.0 | 58.3 |

Per field in a race (profile-guided build, 1785/921/1600 MHz): game thread 17.3 ms, `vu` 17.6 ms,
`gs` 14.1 ms, against a 16.7 ms budget. At scale 1 even a 460 MHz GPU does not limit; at scale 2
even 768 MHz does. The profile build logged 275 sound underruns. Homebrew has three usable CPU
cores (to check on the console), so the game, `vu`, `gs`, render, sound and main threads compete.

## Steps

0. **Bring the branch up to `main`.** Merge it on a new branch, resolve the conflicts (a dry-run
   merge on 2026-11-16 conflicts in `docs/STATE.md`, `port/gs/native_core.inc`,
   `port/gs/present.c`, `port/hle/host.c`, `port/tools/build_host64.py`,
   `port/tools/check_rust_lib.py` and `port/tools/rustgs.py`) and renumber the branch's finding
   (its 127 is taken here by `127-phone-t612-throttle.md`). This brings Native VU (249, 253,
   264), the frame-budget profile (257), stutter fixes (256) and the high-fps sweep (255).
   Native VU's effect on the Switch is unmeasured. Check the other targets still build and the
   state traces still match.
1. **A trustworthy baseline on the console.** Run `nx_fulltest.py` on real hardware, with the
   release deko3d library (`-ldeko3d`, not the debug one), fixed clocks and `bench_race.py`'s
   route. Report per-thread CPU, waits and p99/max field times, as finding 257 does on Android.
2. **Cut CPU work and fix the core layout.** Pin and prioritise threads for three cores. Move
   sound mixing (about 7 % of the game thread) off it and explain the underruns. Confirm the VU
   kernels use NEON. Extend profile-guided optimisation past `core_*.c` and `vu0_interp.c` (an
   instrumented host hung at start; find why). Try LTO and `-mcpu=cortex-a57`.
3. **Port main's renderer gains to `dhd_render_nx.c`.** Main has since added fewer render passes
   (112), a framebuffer cache (109), presenting from the display surface (247), a cheaper mesh
   path (249) and an early-Z probe change (111). The shared C parts in `port/gs` arrive with step
   0; the Rust-only parts must be hand-ported, one at a time. deko3d has no pipelines, Horizon has
   no `mprotect` for guest pages, and dual-source blending is always on.
4. **Switch device defaults** (the pattern of finding 263). Handheld starts at scale 1 with
   Performance Mode on (shadows, lens flare and particles off); docked starts at scale 2 only if
   measured to hold 60. An existing `dhd.ini` is never changed. The rule lives where the phone
   rules do, or in `settings_nx.c` if the Rust crate stays off this target.
5. **Pacing.** A steady 30 fields/s option or fallback, so play is smooth when 60 is out of reach,
   instead of swinging between 47 and 54 with 60-70 ms spikes. The three longest fields in the
   branch's runs (6223, 6557, 6647) came from the scripted race, so check what they are first.

## Investigation results (2026-11-16, read from the merged tree; nothing measured)

**B. CPU and build flags.** Threads (`sdl_nx.c` `placements`): game 0x2C on core 0 only, vu 0x2C on
core 1 only, native 0x2D and gs 0x2E on core 2 with all three cores allowed, `nx audio` 0x2A on
core 2 only, movie 0x3B. So the layout step 2 asks for already exists in the branch; what is
unknown is whether it helps (no per-thread waits are logged). The VU0 interpreter uses
`<arm_neon.h>` with SIMDe's SSE mapping, the same file as Android, so NEON is in use for it; the
emulated VU1 kernels on this target were not checked for NEON (the Switch flags are
`-march=armv8-a+crc+crypto -mtune=cortex-a57`, no `-mcpu`, no LTO). PGO is gcc's, limited to
`core_*.c` and `vu0_interp.c` (`build_host64.py`). Sound: the game's mixer writes into a 64 KiB
ring through `SDL_PutAudioStreamData`; `nx audio` only drains it, so the mixing stays on the game
thread, as the plan says.

**Native VU is not active on the Switch, and the dhd.ini table is behind.** `dhd_render_nx.c` has
no mesh hook (`gs_mesh_hook`, `gs_mesh_mode`, finding 249 and `gs_mesh.h`), so the translated mesh
loops never run and VU1 stays emulated: that is the 17.6 ms `vu` thread. A mesh path in deko3d is
therefore the largest CPU lever and belongs to step 3 first, not last. Also `settings_nx.c`'s table
lacks `native_vu`, `shadows`, `lens_flare`, `particles`, the `touch_*` keys, `language`,
`room_region` and `stick_dead_zone`, so Performance Mode cannot be set from dhd.ini on the console
until they are added.

**C. Renderer gap** (main's gains against `dhd_render_nx.c`, a copy of 0.7.6's Rust):

| Change | Applies to deko3d | Where | Effort | Risk |
|---|---|---|---|---|
| Native VU mesh draws (249, 253, 264) | yes, the biggest: removes most `vu` thread time | Rust mesh path to port; `gs_mesh.h` and `vu1_mesh.inc` are shared C | high | high: new vertex format, near-bound clipping |
| Fewer render passes (112) | yes | Rust-only | medium | medium |
| Framebuffer cache (109) | partly: Vulkan-specific, deko3d has no render-pass objects | Rust-only | medium | medium |
| Early-Z probe change (111) | yes, if the probe is used at all (the exact front end is a stub) | shared C and Rust | low | low |
| Present from the display surface (247) | to check | `present.c` is shared; the image path is Rust-only | medium | medium |

**D. Defaults and pacing.** Handheld detection is done once in `nx_host.c` (`appletGetOperationMode`
forces `DHD_SCALE=1` for that run, dhd.ini untouched), so scale already follows the mode. The
phone rule of finding 263 is Rust (`phone_defaults.rs`, `dhd_settings::phone`), which this target
does not link, so a Switch rule goes in `settings_nx.c` after the table is brought up to date, and
writes Performance Mode only to a dhd.ini that has none of `shadows`, `lens_flare`, `particles`.
A 30 fields/s option exists as the game's `fps` setting (`fps.c`, default 60, `DHD_FPS`); whether
30 paces evenly on this target needs a console run.

## Work split (subagents)

Pieces A to D are done (A: the merge, B to D: the results above). Only E remains, and it needs a console. Step 0 came first and alone, because every other piece reads the merged tree. After
it, steps 2 to 5's investigations are read-only and can run in parallel; changes follow one at a
time, each measured on the console, which only a person with the hardware can do.

| Piece | Kind | Brief | Depends on |
|---|---|---|---|
| A. Merge | writes, in its own worktree | Step 0: merge, resolve conflicts, renumber the finding, run the checks that run offline. Cannot build the NRO or run on a console; must say so | none |
| B. CPU and build flags | read-only | Every thread the Switch build creates with priority, stack and affinity; where sound mixes; whether the VU kernels use NEON; compile flags and PGO scope; whether Native VU is on | A |
| C. Renderer gap | read-only | A table of main's renderer and GS changes since 0.7.6: gain, whether it applies to deko3d, shared versus Rust-only, effort, risk, and `dhd_render.h` API changes the nx file must meet | A |
| D. Defaults and pacing | read-only | Where a Switch device rule would live, how docked/handheld is detected, files and tests to change; what a fixed 30 fields/s option needs | A |
| E. Measurement | person with a console | Steps 1 and every before/after number | A |

A first attempt at starting A to D was stopped before it produced a result; A left an unfinished
merge in a throwaway worktree. Nothing from it is used here.

## Progress

- [x] 2026-11-16: plan written (this file)
- [x] 2026-11-16: step 0 merged on branch `perf/switch-270` (first `switch/rebase-0.8.9`, on 0.8.9; worktree `.claude/worktrees/agent-a08dc94462a426a91`,
  `git merge`, the contributor's commit kept). The branch's finding became 272. Conflicts resolved:
  `host.c` (main's `host_threads.c` now carries the Switch's thread handles and CPU ticks; the SDL
  shim gained `SDL_GetTLS`/`SDL_SetTLS`), `present.c` (the Switch keeps its swapchain in the
  background but takes main's render hold, `gs_render_leave`/`gs_render_release`),
  `build_host64.py`, `rustgs.py` and `check_rust_lib.py` (macOS x64 and iOS targets both kept,
  Switch PGO beside main's clang PGO), `native_core.inc` and `STATE.md` (no markers left).
  Run here: `check.py docs` OK. NOT run: `check.py quick` stops at a missing `asm/` (no ISO on
  this machine); the NRO cannot be built (no devkitPro). The merged Switch code is uncompiled.
- [x] 2026-11-16: investigations B, C, D written above
- [x] 2026-11-16: step 4, code part: `settings_nx.c` has main's missing keys (native_vu stays off until
  the mesh path exists); a new dhd.ini starts with shadows, lens flare and particles off. Neither is
  compiled or measured; a docked scale 2 default is not set, as it is not yet shown to hold 60.
- [x] 2026-11-17: main's 0.8.10 merged in (`0287436c`); the Switch findings renumbered to 270 (plan) and 272 (port), because 267 and 268 went to the iOS A12 and touch left/right findings. `check.py docs` OK; `quick` fails here only on a missing `llvm-nm` and a wasi-sdk path for another OS. The branch is pushed to the contributor's fork, not to the main repository.
- [ ] step 1: baseline on the console (needs a console)
- [ ] steps 2 to 5

## Decisions

- The merge keeps the original commit (`git merge`, not a rebase), so the contributor's authorship
  stays and the branch can be compared with the first commit.
- Clocks, the deko3d library and the route are fixed before any number counts, because finding 127
  changed descriptors and clocks together and could not split their effects.

## Surprises

- Native VU was assumed to be a thing to measure on the Switch; it is not present there at all (the
  renderer has no mesh hook). Step 3's first job is the mesh path.
- The thread placement of step 2 was already in the contributor's branch.

## Result

Filled in at the merge.
