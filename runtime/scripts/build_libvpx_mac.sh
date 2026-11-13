#!/bin/bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Chaos192
# macOS: builds libvpx 1.16.0 (the version the Windows build takes from vcpkg) as a static arm64 library with the VP8 / VP9 decoders only,
# into ~/Library/Caches/nxrt/libvpx (or $1), where cmake/media_deps.cmake finds it. The runtime links it into libpkp_rt.dylib, so players
# need nothing; without it the Switch 2 movies (WebM, VP9) are left out (NXRT_HAVE_LIBVPX=0).
set -euo pipefail
prefix="${1:-$HOME/Library/Caches/nxrt/libvpx}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
git clone -q --depth 1 --branch v1.16.0 https://chromium.googlesource.com/webm/libvpx "$work/src"
mkdir -p "$work/build"
cd "$work/build"
"$work/src/configure" --target=arm64-darwin23-gcc --prefix="$prefix" --disable-examples --disable-docs --disable-unit-tests --disable-tools \
    --disable-vp8-encoder --disable-vp9-encoder --enable-vp8-decoder --enable-vp9-decoder --enable-static --disable-shared --enable-pic \
    --extra-cflags=-mmacosx-version-min=14.4
make -j"$(sysctl -n hw.ncpu)"
make install
ls -la "$prefix/lib/libvpx.a"
