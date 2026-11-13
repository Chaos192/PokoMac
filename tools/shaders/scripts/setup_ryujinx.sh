#!/bin/bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Chaos192
# macOS and Linux twin of setup_ryujinx.ps1: stage the three Ryujinx projects the translator needs into tools/shaders/third_party/ryubing
# (git-ignored). Source: a read-only checkout of the Ryubing fork (default refs/ryubing in the repository root; the release pins commit
# a82350bb774f70fcbd41c9987bf67a3775409963, MIT). The build writes obj/bin next to the sources, so it builds from this copy and never
# touches the checkout.
#   git clone --filter=blob:none https://git.ryujinx.app/ryubing/ryujinx.git refs/ryubing && git -C refs/ryubing checkout a82350bb77...
#   tools/shaders/scripts/setup_ryujinx.sh [<ryubing checkout>]
#   dotnet publish -c Release -r osx-arm64 --self-contained true tools/shaders/harness/SpirvHarness.csproj -o <out>
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="${1:-$here/../../../refs/ryubing}"
dst="$here/../third_party/ryubing"
[ -d "$src/src/Ryujinx.Graphics.Shader" ] || { echo "no Ryubing checkout at $src" >&2; exit 1; }
mkdir -p "$dst/src"
for p in Ryujinx.Common Spv.Generator Ryujinx.Graphics.Shader; do
    # a mirror like robocopy /MIR /XD obj bin: what the checkout no longer has goes, build output is never copied
    rm -rf "${dst:?}/src/$p"
    mkdir -p "$dst/src/$p"
    (cd "$src/src/$p" && find . -type d \( -name obj -o -name bin \) -prune -o -type f -print0 | cpio -0pdm --quiet "$dst/src/$p")
done
for f in Directory.Build.props Directory.Packages.props global.json nuget.config LICENSE.txt; do
    cp -f "$src/$f" "$dst/$f"
done
echo "staged Ryujinx sources in $dst"
