#!/bin/bash
# Packages the port for sharing, WITHOUT any game files: run ./build.sh title first, then this.
# Result: dist/release/Motorhead-PS5-<date>.zip  (the title, the default profile, an install note).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
source "$HERE/tools/pins.env"
SRC=$HERE/dist/$TITLE_ID
[ -f "$SRC/eboot.bin" ] || { echo "error: run ./build.sh title first" >&2; exit 1; }
OUT=$HERE/dist/release/$TITLE_ID
rm -rf "$HERE/dist/release"; mkdir -p "$OUT/Motorhead" "$OUT/data"
cp -R "$SRC/eboot.bin" "$SRC/sce_sys" "$SRC/sce_module" "$OUT/"
rsync -a --exclude '*.log' --exclude 'verbose' --exclude 'renderer' --exclude 'output' "$SRC/data/" "$OUT/data/"
cp "$HERE/LICENSE" "$OUT/LICENSE.txt"
cp "$HERE/NOTICE.md" "$OUT/NOTICE.md"
cat > "$OUT/Motorhead/PUT-YOUR-GAME-FILES-HERE.txt" <<'TXT'
Copy your own Motorhead (1998) game folder contents into this folder, so that these sit directly in it:
  Cars, Data, Demos, Game, League, Movies, Music, Sounds, Tracks, AddressBook, motor.txt
(the installed game with the 3.0 update; WAV soundtrack in Music as track02.wav ... track11.wav).
Nothing from the game is included in this package.
TXT
cat > "$OUT/README.txt" <<'TXT'
Motorhead (1998) for PS5 (title PPSA99780)
Based on the S95Sedan reconstruction (non-commercial; see LICENSE.txt). Changes are marked: PS5 port, GPU scene renderer, lens flares, on-screen keyboard.

Install: copy this folder to /data/homebrew/PPSA99780, put your game files in Motorhead/ (see the note there), rescan with ShadowMountPlus.
Settings: data/motorhead.cfg (default: every car and track unlocked via PlayerName "R Pettersson" + TeamName "Swe"; change either for normal progression; SFXVolume 10, MusicVolume 100).
Log: data/motorhead.log lists errors only. Create an empty file data/verbose for full diagnostics. data/renderer containing "software" forces the CPU renderer. data/output with 1080, 1440 or 2160 forces the display mode.
Controls: X select (opens the on-screen keyboard on name fields), Triangle cycles cameras (the fifth is the cinematic track camera).
TXT
cd "$HERE/dist/release" && zip -qr "Motorhead-PS5-$(date +%Y%m%d).zip" "$TITLE_ID" && ls -la
