#!/bin/bash
# Cross-compiles the game (shared code, SDL3 and the PS5 platform layer) as static libraries.
source "$(dirname "$0")/common.sh"
install_crt
SRC=$TREE/motorhead-repo/project
RADV=$VK/.deps/native/radv-release
B=$OUT/cmake
mkdir -p "$B"
log "shaders"
SHADER_DIR=/work/ps5/shaders
INC=$SRC/motorhead-ps5/src/renderer/hardware/vk_shaders.inc
: > "$INC"
for s in scene.vert scene.frag proj.vert proj.frag; do
    name=${s/./_}_spv
    glslangValidator -V --vn "$name" "$SHADER_DIR/$s" -o "$OUT/$s.h" > "$OUT/$s.log" 2>&1 || { cat "$OUT/$s.log"; exit 1; }
    cat "$OUT/$s.h" >> "$INC"
done
log "configure"
cmake -S "$SRC/motorhead-ps5" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$PS5_SDK_DIR/toolchain/prospero.cmake" \
    -DCMAKE_CXX_FLAGS="-I$RADV/include" -DMH_WARNINGS_AS_ERRORS=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF \
    -DSDL_DUMMYVIDEO=ON -DSDL_OFFSCREEN=OFF -DSDL_DUMMYAUDIO=ON -DSDL_DISKAUDIO=OFF \
    -DSDL_OSS=OFF -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF \
    -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF -DSDL_HIDAPI=OFF -DSDL_VIRTUAL_JOYSTICK=ON -DSDL_LIBUSB=OFF -DSDL_CAMERA=OFF \
    -DSDL_DIALOG=OFF -DSDL_TRAY=OFF \
    > "$OUT/configure.log" 2>&1 || { tail -40 "$OUT/configure.log"; exit 1; }
log "build"
cmake --build "$B" -- -k 0 > "$OUT/build.log" 2>&1 || { grep -E 'error|FAILED' "$OUT/build.log" | head -40; echo "see $OUT/build.log"; exit 1; }
find "$B" -name '*.a' | sed "s|$B/||"
