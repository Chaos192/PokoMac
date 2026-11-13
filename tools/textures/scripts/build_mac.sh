#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Chaos192
# macOS build of tools/textures: fetches astc-encoder 5.7.0, bc7enc_rdo and ISPC 1.32.0 into third_party, then builds
# build/release/libastc2bc7.a and libbc7e_ispc.a (what runtime/cmake/gfx.cmake links).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
tp="$here/third_party"
mkdir -p "$tp"
[ -d "$tp/astc-encoder" ] || { git clone -q https://github.com/ARM-software/astc-encoder.git "$tp/astc-encoder"; git -C "$tp/astc-encoder" checkout -q 5.7.0; }
[ -d "$tp/bc7enc_rdo" ] || { git clone -q https://github.com/richgel999/bc7enc_rdo.git "$tp/bc7enc_rdo"; git -C "$tp/bc7enc_rdo" checkout -q b9438627eef73a1157e84201b6fa6eb2ffd6d9f0; }
if [ ! -x "$tp/ispc/ispc-v1.32.0-macOS.universal/bin/ispc" ]; then
    mkdir -p "$tp/ispc"
    curl -fL --retry 5 -o "$tp/ispc/ispc.tar.gz" https://github.com/ispc/ispc/releases/download/v1.32.0/ispc-v1.32.0-macOS.universal.tar.gz
    tar xzf "$tp/ispc/ispc.tar.gz" -C "$tp/ispc"
    rm "$tp/ispc/ispc.tar.gz"
    xattr -dr com.apple.quarantine "$tp/ispc" 2>/dev/null || true
fi
cmake -S "$here" -B "$here/build/release" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.4
cmake --build "$here/build/release"
ls -la "$here/build/release/libastc2bc7.a" "$here/build/release/libbc7e_ispc.a"
