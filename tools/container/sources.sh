#!/bin/bash
# The Motorhead reconstruction at its pinned commit, with this repository's PS5 port applied
# (ps5/patches/motorhead and ps5/overlay).
source "$(dirname "$0")/common.sh"
mkdir -p "$TREE" && cd "$TREE"
log "upstream sources"
clone_at "$MOTORHEAD_REPO" motorhead-repo "$MOTORHEAD_COMMIT"
cd motorhead-repo
want=$( { find /work/ps5/overlay -type f | sort | xargs cat; cat /work/ps5/patches/motorhead/*.patch; } | sha256sum | cut -c1-16)
want="$MOTORHEAD_COMMIT $want"
if [ ! -f .ps5-port-stamp ] || [ "$(cat .ps5-port-stamp)" != "$want" ]; then
    git reset -q --hard "$MOTORHEAD_COMMIT"
    git clean -fdq -e .ps5-port-stamp
    log "applying the PS5 port"
    for p in /work/ps5/patches/motorhead/*.patch; do git apply --whitespace=nowarn "$p"; done
    cp -R /work/ps5/overlay/project/. project/
    echo "$want" > .ps5-port-stamp
fi
