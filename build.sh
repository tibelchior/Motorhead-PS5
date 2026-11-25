#!/bin/bash
# Builds Motorhead (1998) for the PS5 from the reconstruction source and your own game files.
#
#   ./build.sh toolchain   check/build the PS5 toolchain (cached in the shared Docker volume)
#   ./build.sh compile     fetch sources, apply the PS5 port, cross-compile the game libraries
#   ./build.sh title       link and package the title, with the game files (NO_GAME=1 to leave them out)
#
# Options (environment):
#   GAME_DIR=<folder>      the assembled game folder (default: the motorhead-work/game next to the CD image)
#   BUILD_VOLUME=<name>    Docker volume with the PS5 toolchain (default dirt2ps5-build-data, shared with DiRT 2/3)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/tools/pins.env"
STEP=${1:-compile}
IMAGE=dirt2ps5-build VOLUME=${BUILD_VOLUME:-dirt2ps5-build-data} NAME=motorheadps5
GAME=${GAME_DIR:-$HERE/../../pc-source/Motorhead 1998/motorhead-work/game-community}
die() { echo "error: $*" >&2; exit 1; }
command -v docker > /dev/null || die "Docker is not installed."
docker info > /dev/null 2>&1 || die "Docker is not running."
docker volume inspect "$VOLUME" > /dev/null 2>&1 || docker volume create "$VOLUME" > /dev/null
docker image inspect "$IMAGE" > /dev/null 2>&1 || docker build -q -t "$IMAGE" "$HERE/docker" > /dev/null
docker rm -f "$NAME" > /dev/null 2>&1 || true
mkdir -p "$HERE/dist" "$HERE/art"
docker run -d --name "$NAME" -v "$VOLUME":/build -v "$HERE":/work -v "$GAME":/game:ro "$IMAGE" sleep infinity > /dev/null
trap 'docker rm -f "$NAME" > /dev/null 2>&1 || true' EXIT
run() { docker exec -e VOL=/build "$NAME" bash "/work/tools/container/$1.sh"; }
case $STEP in
    toolchain) run toolchain ;;
    compile) run toolchain; run sources; run compile ;;
    title) run toolchain; run sources; run compile; run title ;;
    *) die "unknown step '$STEP' (toolchain, compile, title)" ;;
esac
if [ "$STEP" = title ] && [ "${NO_GAME:-0}" != 1 ]; then
    OUTDIR=$HERE/dist/$TITLE_ID
    [ -f "$OUTDIR/eboot.bin" ] || die "the build did not produce $OUTDIR/eboot.bin"
    echo "== copying the game files into the title (about 0.6 GB)"
    # PC game files and the port's own files stay apart: Motorhead/ and data/.
    mkdir -p "$OUTDIR/Motorhead" "$OUTDIR/data"
    rsync -a --exclude Motorhead --exclude User --exclude '.DS_Store' "$GAME/" "$OUTDIR/Motorhead/"
    rsync -a --exclude '*.log' --exclude '*.bak' --exclude '*.before-unlock' "$GAME/User/" "$OUTDIR/data/"
    # PS5 defaults for the shipped profile: the display renderer at 1080p 16:9, and the retail
    # "unlock everything" identity (all cars and tracks; the community cars sit in higher divisions).
    if [ -f "$OUTDIR/data/motorhead.cfg" ]; then
        sed -i.orig -E \
            -e 's|^(Renderer[[:space:]]+)[^[:space:]]+[[:space:]]*|\1display                  |' \
            -e 's|^(ScreenDimension[[:space:]]+)[0-9]+ [0-9]+ *|\11920 1080                |' \
            -e 's|^(AspectRatio[[:space:]]+)[^[:space:]]+ *|\116:9                     |' \
            -e 's|^SFXVolume.*|SFXVolume               10                       // Sound-effect volume in percent [0,100]. Default 10 so the soundtrack leads|' \
            -e 's|^MusicVolume.*|MusicVolume             100                      // Music volume in percent [0,100]|' \
            -e 's|^PlayerName.*|PlayerName              R Pettersson            // Player name. "R Pettersson" with team "Swe" unlocks every car and track; change either one for normal progression|' \
            -e 's|^TeamName.*|TeamName                Swe                     // Team abbreviation (fewer than 5 characters). See PlayerName: "Swe" keeps everything unlocked|' \
            "$OUTDIR/data/motorhead.cfg" && rm -f "$OUTDIR/data/motorhead.cfg.orig"
    fi
    echo "Done: $OUTDIR  ->  copy to /data/homebrew/$TITLE_ID on the console"
fi
