# Motorhead (1998) for PS5

A PS5 port of the [S95Sedan Motorhead reconstruction](https://github.com/S95Sedan/motorhead), built with the
PS5 homebrew toolchain by mihawk-99 (PS5_Vulkan, PS5_PayloadSDK, RADV). Title ID `PPSA99780`.

**Bring your own game files.** This repository contains no game data and no reconstruction source; the build
fetches the reconstruction at a pinned commit and applies the changes in `ps5/` (see `NOTICE.md` and `LICENSE`).

## What you get
- Runs at **1080p, 60 fps** on a jailbroken PS5: the 3D scene is drawn by a Vulkan renderer on the GPU (sky and
  fog included); menus, HUD and text stay on the original 2D path.
- DualSense controller (and the game's own control layouts), sound through the console's audio output, race
  music from the soundtrack WAVs.
- 3dfx-style looks: round lamp glows (street lamps, headlights, brake lights), lens-flare ghosts, extra haze,
  brighter cars.
- All five race cameras, including the cinematic track camera (Triangle cycles them).
- On-screen keyboard: press X on the player-name or team field.
- Proper exit to the console home screen, errors-only log.

## Requirements
- A PS5 that runs homebrew titles (this port is installed like the other PPSA homebrew titles, e.g. via ShadowMountPlus).
- Your own copy of Motorhead (1998), installed, with the 3.0 update. The community 3.5 data works. The WAV
  soundtrack goes in `Music/` as `track02.wav` ... `track11.wav`.
- To build: macOS or Linux with Docker (the build host is a Fedora container; Apple silicon works), plenty of free disk
  and 1 to 2 hours for the first toolchain build.

## Build
```bash
./build.sh toolchain      # once: PS5 payload SDK, RADV, signing tools (cached in a Docker volume)
./build.sh compile        # fetch the reconstruction, apply the port, cross-compile
./build.sh title          # link, sign, package into dist/PPSA99780 (with your game files)
tools/release.sh          # optional: game-free package + zip in dist/release/
```
Options (environment): `GAME_DIR=<folder>` your assembled game folder (the installed game; the retail folder
layout with `Cars`, `Data`, `Music`, `Tracks`, ...); `NO_GAME=1` leaves the game files out; `BUILD_VOLUME=<name>`
Docker volume with the toolchain (default `dirt2ps5-build-data`, so it can be shared with other ports built the same way).

`GAME_DIR` defaults to a folder next to this repository; set it explicitly.

## Install
Copy `dist/PPSA99780` to `/data/homebrew/PPSA99780` and rescan with ShadowMountPlus.

```
PPSA99780/
  eboot.bin, sce_sys/, sce_module/   the title
  Motorhead/                          your game files (read only)
  data/                               everything the port writes or configures
    motorhead.cfg                     the game's profile and options
    Input/                            control layouts
    motorhead.log                     errors only (see below)
```

## Configuration (`data/motorhead.cfg`)
- **Unlocks.** The shipped profile uses `PlayerName R Pettersson` with `TeamName Swe`, which unlocks every car and
  track. Change either value for normal progression. You can edit the name in game with the on-screen keyboard.
- **Volume.** `SFXVolume 10`, `MusicVolume 100` by default so the soundtrack leads.
- **Display.** `ScreenDimension 1920 1080`, 16:9.
- Optional files in `data/`: `verbose` (any content) for full diagnostics in the log; `renderer` containing
  `software` forces the CPU renderer; `output` containing 1080, 1440 or 2160 forces the display mode.
- Copying a new build over an existing install keeps your old `data/motorhead.cfg`: delete it to get new defaults.

## Controls
X select (opens the keyboard on text fields), Circle back, Triangle changes camera, D-pad/left stick menus and
steering. The game's remapping screens work as in the PC game.

## How the port works
- `ps5/patches/motorhead/` patches to the reconstruction, applied in order:

| Patch | Purpose |
|---|---|
| 0001 to 0007 | PS5 build variant, SDL hooks (dummy video/audio/virtual joystick), file-system quirks |
| 0008 | Lens-flare ghosts, GPU-drawn sky and fog pass |
| 0009 | Graphics page: every row highlights its chosen value |
| 0010 | Cinematic camera as the fifth camera in a live race |
| 0011 | Music level, mist, brighter lit objects |
| 0012, 0013 | Round lamp glows, size cap so close lamps cannot flood the screen |
| 0014 | Errors-only log |

- `ps5/overlay/` new files copied into the reconstruction: platform layer (`ps5-platform/`: Vulkan presenter,
  pad and sound, on-screen keyboard, launch glue) and the Vulkan scene renderer (`motorhead-ps5/`).
- `ps5/shaders/` the scene shaders; `tools/container/` the build steps that run inside the container.

## Troubleshooting
- Black screen or crash at start: add an empty `data/verbose` file, run again and read `data/motorhead.log`.
- Slow or odd rendering: put `software` in `data/renderer` to compare with the CPU renderer.
- Music missing: check `Music/track02.wav` ... `track11.wav` exist; with `verbose` the log shows `Race music: <file>`.

## Credits and license
Reconstruction by S95Sedan. PS5 toolchain by mihawk-99. This port's changes are under the same non-commercial
license, see `LICENSE` and `NOTICE.md`. No game assets are distributed here; no commercial use.
