# Downhill Revived for Nintendo Switch

*Downhill Domination* (PlayStation 2, 2003) as homebrew for the Nintendo Switch: a fork of
[Downhill Revived](https://github.com/skuza-oss/downhill-revived), the native PC port.

**[Download](https://github.com/marmota94/downhill-revived/releases/latest)** (the
`downhill-revived.nro` file)

## How to play on the Switch

You need:

- a modded Switch with Atmosphère and a Homebrew Menu (hbmenu or sphaira);
- your own dump of the USA disc, **SCUS-97177** (other regions and modified images don't work).

Steps:

1. Download `downhill-revived.nro` from the
   [latest release](https://github.com/marmota94/downhill-revived/releases/latest).
2. Copy it and your disc image to the SD card, into this folder, with these exact names:

       /switch/downhill-revived/downhill-revived.nro
       /switch/downhill-revived/SCUS-97177.iso

   Use the card in a PC, or sphaira's FTP server (port 5000):
   `curl -T SCUS-97177.iso ftp://CONSOLE_IP:5000/switch/downhill-revived/`
3. Hold **R** while starting any game to open the Homebrew Menu, then pick Downhill Revived. Don't
   start it from the album: it has too little memory there, and the game says so.

The first start makes `dhd.ini` in that folder; `dhd.log` and the saves go there too.

**Smoother play:** races run at about 46-54 fps, not yet a steady 60. For a steady, even 30, add
`fps = 30` to `dhd.ini`.

**After a crash**, look in `/atmosphere/crash_reports/`, `/atmosphere/fatal_reports/` and
`/switch/downhill-revived/dhd.log`, and attach them to an issue.

## What works

- The title, the menus, whole races and the game's memory card saves.
- Button rebinding: in the game, OPTIONS > REVIVED SETTINGS > Controls.
- EXIT GAME returns to the Homebrew Menu.

Not on the Switch: online play and texture packs.

Speed, measured docked (CPU/GPU/memory at 1785/768/1600 MHz): races about 46-54 fps, menus about
45. Work toward a steady 60 (the mesh renderer path, a profile-guided build, threads, the sound
underruns) is tracked in [finding 270](docs/findings/270-switch-performance-plan.md) and in
[docs/BUILDING.md](docs/BUILDING.md#switch-releases-the-fork-only).

## Building it yourself

Building needs the original game executable from your own disc; nothing here builds without it.
The Switch build (devkitPro with libnx and deko3d):
[docs/BUILDING.md](docs/BUILDING.md#nintendo-switch). Everything Switch-specific is in `port/nx/`,
the `switch-arm64` target of `port/tools/build_host64.py` and the console test
`port/tools/nx_fulltest.py`; the port itself is finding 272. How to help:
[docs/CONTRIBUTING.md](docs/CONTRIBUTING.md).

## About the upstream project

Downhill Revived is a native port of the game to Windows, macOS, Linux, Android and iOS, built on a
matching decompilation: C/C++ source that compiles back to the original game program byte for
byte, with the PS2 hardware replaced by native code. This fork is Downhill Revived 0.8.10 plus the
Switch port; upstream has no Switch support. For the PC and phone versions, use upstream:
[downloads](https://github.com/skuza-oss/downhill-revived/releases/latest),
[players' guide](docs/PLAYING.md), [Discord](https://discord.gg/XrSeTyzfr2),
[Ko-fi](https://ko-fi.com/skuza).

### Decompilation progress

<!-- progress:start -->
| Region | What | Code matched | Functions |
|---|---|---|---|
| crt0 | startup code | 0.00% | 0 / 5 |
| game | game code | 98.17% | 3439 / 3474 |
| libgcc | compiler runtime (library, not decompiled) | 0.00% | 0 / 145 |
| engine | engine (same toolchain as the game) | 92.76% | 1088 / 1119 |
| sdk | Sony SDK + C/math libraries (not decompiled) | 0.00% | 0 / 802 |
| handwritten | game/engine functions written in assembly (not decompilable) | 0.00% | 0 / 7 |
| remnant | dead-strip leftovers of removed functions (not decompilable) | 0.00% | 0 / 138 |
| **target** | **game + engine: everything there is to decompile** | **97.03%** | **4527 / 4593** |
| **total** | whole executable, libraries included | **85.76%** | **4527 / 5690** |
<!-- progress:end -->

A function counts only when a tool confirms its compiled bytes are identical to retail; what
"matching" means and how it is checked: [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md).

## Legal notice

- This is a fan project. It is not made, endorsed or supported by Sony Interactive
  Entertainment, Incognito Entertainment, Nintendo or anyone who holds rights to the game.
- This repository and its releases contain **no game data**: no disc image, no game executable
  or data from it, no IOP modules, no textures, sound, music or movies, and no BIOS. The NRO is
  the translated code only; at every start it reads the game's data from your disc image, after
  checking it is the unchanged original.
- To play you need your own dump of the USA disc. Other regions and modified images are not
  supported (why: [docs/findings/100-other-releases.md](docs/findings/100-other-releases.md)).
- The project sells nothing; every release is free.

## Credits and licence

Built on the work of PCSX2, ps2sdk, SDL, wgpu, WABT, libmpeg2, OpenGOAL, splat, objdiff,
decomp.me, devkitPro (libnx, deko3d) and many others: the full list is in
[docs/CREDITS.md](docs/CREDITS.md). The Switch port started from a contributor's
`switch-homebrew` branch (finding 272).

[GPL-3.0-or-later](LICENSE). The original game, its data, names and trademarks belong to their
owners and are not covered by this licence. Code adapted from other projects keeps its own
notice (see [Credits](docs/CREDITS.md)).
