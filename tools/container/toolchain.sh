#!/bin/bash
# The PS5 toolchain: mihawk-99's payload SDK and RADV (Mesa's Vulkan driver for the console),
# plus the tool that links and signs a title. About 1 to 2 hours the first time, then cached.
source "$(dirname "$0")/common.sh"
stamp=$VOL/.toolchain-done
want="$PS5_VULKAN_COMMIT $PS5_PAYLOADSDK_COMMIT $PS5_MESA_COMMIT"
[ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ] && { echo "toolchain: already built"; exit 0; }

log "sources"
cd "$VOL"
clone_at "$PS5_VULKAN_REPO" PS5_Vulkan "$PS5_VULKAN_COMMIT"
clone_at "$PS5_PAYLOADSDK_REPO" PS5_PayloadSDK "$PS5_PAYLOADSDK_COMMIT"
clone_at "$PS5_MESA_REPO" PS5_Mesa "$PS5_MESA_COMMIT"

log "payload SDK and native dependencies"
cd "$VK"
bash tools/setup-native-dependencies.sh > "$VOL/setup-native.log" 2>&1 || { tail -20 "$VOL/setup-native.log"; exit 1; }

log "RADV, release build (long)"
bash tools/build-radv.sh release > "$VOL/build-radv-release.log" 2>&1 || { tail -30 "$VOL/build-radv-release.log"; exit 1; }

# BC7 encoder for the tile and backgrounds (art/).
log "asset tools"
(cd "$VK" && bash tools/setup-asset-dependencies.sh) > "$VOL/setup-assets.log" 2>&1 || { tail -20 "$VOL/setup-assets.log"; exit 1; }

# The arm64 host lacks Clang's x86-64 builtins: take them from Fedora's x86-64 package.
log "x86-64 compiler-rt"
dnf -q -y install cpio dnf-plugins-core > /dev/null 2>&1 || true
mkdir -p "$VOL/crt-x86" && cd "$VOL/crt-x86"
if [ "$(uname -m)" != x86_64 ]; then
    dnf -q download --forcearch x86_64 compiler-rt > /dev/null 2>&1
    rpm2cpio compiler-rt-*.x86_64.rpm | cpio -idm --quiet
    lib=$(find . -name 'libclang_rt.builtins*.a' -path '*x86_64*' | head -1)
    [ -n "$lib" ] || { echo "no x86-64 libclang_rt.builtins in the compiler-rt package" >&2; exit 1; }
    cp "$lib" "$VOL/crt-x86/libclang_rt.builtins-x86_64.a"
fi
install_crt

# The tool that links and signs a title, and the libc module every title ships.
log "link and signing tool, libc module"
cd "$VK"
z=$(find .deps/native/zlib/root -name libz.a | head -1)
mkdir -p build/host
clang++ -std=c++20 -O2 -I .deps/native/zlib/root/usr/include tooling/native/native_app_builder.cpp \
    tooling/native/self_container.cpp tooling/native/elf_object.cpp tooling/native/sce_module_writer.cpp \
    "$z" -o build/host/ps5-native-tool > "$VOL/native-tool.log" 2>&1 || { tail -20 "$VOL/native-tool.log"; exit 1; }
bash tools/rebuild-libc.sh > "$VOL/rebuild-libc.log" 2>&1 || { tail -20 "$VOL/rebuild-libc.log"; exit 1; }

# PS5_Vulkan's own first title: a smoke test of the driver archive and tools.
log "title tools"
cd "$VK"
RADV_ARCHIVE=$VK/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a bash tools/build-radv-title.sh \
    > "$VOL/build-radv-title.log" 2>&1 || { tail -30 "$VOL/build-radv-title.log"; exit 1; }
for f in .deps/native/radv-release/lib/libvulkan_radeon.ps5.a build/host/ps5-native-tool runtime/libc.prx; do
    [ -e "$VK/$f" ] || { echo "toolchain incomplete: missing $f" >&2; exit 1; }
done
echo "$want" > "$stamp"
echo "toolchain: done"
