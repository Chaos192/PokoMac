#!/bin/bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Builds and packs the PokoMac player zip for macOS on Apple silicon, the twin of make-release.ps1 (release model b: no game code or data
# in it; the player's Mac lifts the game from their own dump and links the native `pkp` against libpkp_rt.dylib). Run on a Mac:
#   cmake -DPKP_VERSION=$(cat VERSION) build/mac && cmake --build build/mac --target pkp_rt   -> build/mac/libpkp_rt.dylib
#     (PKP_VERSION: what pkp.log's "boot: pkp <version> (PokoMac)" line says; checked below against VERSION, as release.yml sets it)
#   tools/shaders/scripts/setup_ryujinx.sh <ryubing checkout> &&
#     dotnet publish -c Release -r osx-arm64 --self-contained true -p:DebugType=none tools/shaders/harness/SpirvHarness.csproj -o build/mac-harness
#     (DebugType=none: the package ships no .pdb, and a DLL built with one names its path on the build Mac)
#   game-build/scripts/make-release-mac.sh [--no-build] [--keep-app] [--app <PokoMac.app>] [--rt <libpkp_rt.dylib>] [--harness <dir>]
#       [--vulkan-sdk <SDK>/macOS] [--kbm-icons <dir>] [--out <dir>] [--cache <dir>]
#
# The version comes from one place, VERSION in the repository root. The script stamps it into launcher/src-tauri/tauri.conf.json and
# Cargo.toml as .github/workflows/release.yml does for Windows (the launcher compares it with the releases, update.rs; the checkout's
# 0.1.1 is a dev build that never updates), builds the app (npm run tauri build -- --bundles app) and puts the two files and Cargo.lock
# back as they were. --no-build packs an app built before, which must carry the same version.
#
# Everything the launcher reads lives inside the app, so the in-app update (launcher update.rs) replaces one bundle and the game folder
# (~/Library/Application Support/PokoMac by default) stays outside it. The package:
#   PokoMac-<v>-macos-arm64/
#     PokoMac.app/Contents/
#       MacOS/pokopia-launcher                  the launcher (Tauri)
#       Resources/tools/                        as in the Windows package, read only (host.rs; downloads go to Application Support):
#         build/pkp_build.py, dump_hashes.json  the first-run build (mods.py, apply_ips.py: the mod manager, docs/MODS.md;
#                                               cheats_fetch.py: the cheat file fetcher, docs/CHEATS.md)
#         build/recomp/...                      the lifter (nxrecomp, include/nxrecomp, games/pkp, LICENSE, lifter_cli.py)
#         build/sdk/libpkp_rt.dylib, pkp_stub.cpp, pkp_rt_api.h   what pkp is linked from
#         spirv_harness/spirv_harness (+ .NET)  the shader translator, osx-arm64 self-contained
#         shaders/extract_corpus.py             the shader extractor
#       Resources/runtime/                      copied next to pkp in the game folder before each run (host.rs sync_runtime_files):
#         libpkp_rt.dylib                       the runtime (the same file as tools/build/sdk/)
#         libvulkan.1.dylib                     the Khronos Vulkan loader (from the Vulkan SDK; arm64 slice)
#         libvulkan_kosmickrisp.dylib           KosmicKrisp, the default Vulkan driver (on Metal)
#         libMoltenVK.dylib                     MoltenVK, [gfx] vulkan_driver = moltenvk (arm64 slice)
#         vulkan/icd.d/libkosmickrisp_icd.json, MoltenVK_icd.json   their manifests, library_path "../../<dylib>" (relative to the
#                                               json, so they name the dylibs beside pkp; runtime/platform/mac/gfx/vulkan_mac.cpp)
#         fonts/DejaVuSans.ttf (+ licence)      the shared-font fallback beside pkp; also assets/fonts/, which the runtime finds
#                                               through the working directory (services/pl.cpp)
#         assets/prompts/                       button prompt icons (prompt_icons.pak, Kenney CC0)
#         assets/kbm_icons/                     keyboard and mouse prompt icons (runtime/assets/kbm_icons, Kenney CC0; pkp_kbm.cpp)
#         pipeline_seed.nxpk                    the pipelines of the opening (hashes and state), compiled ahead by pkp --prepare-pipelines
#       Resources/licenses/, LICENSE.txt        every licence (game-build/release_b/macos/BUNDLED_TOOLS.txt lists them)
#     README.txt, LICENSE.txt, licenses/
#   PokoMac-<v>-macos-arm64.zip (+ .sha256, the line for SHA256SUMS.txt)
# Nothing in it names the Mac it was built on (only the copyright line): the launcher is built with its source paths
# remapped (--remap-path-prefix), the runtime's copies are stripped of debug stabs (strip -S: the object paths of static libraries
# built with -g), and the finished package is searched for the build Mac's home folder and host name before it is signed.
# The app is signed again ad hoc once its Resources are complete (no Apple developer account: players open it with right-click, Open).
# Python and Zig are not shipped: the launcher downloads them pinned on the first Set up; their licences come from the same pinned files,
# fetched into --cache (default build/mac-release-cache) and checked by SHA-256.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
launcher="$root/launcher"
build=1 app="$launcher/src-tauri/target/release/bundle/macos/PokoMac.app" rt="$root/build/mac/libpkp_rt.dylib"
harness="$root/build/mac-harness" sdk="${VULKAN_SDK:-$HOME/VulkanSDK/1.4.363.1/macOS}" kbm="$root/runtime/assets/kbm_icons"
out="$root/release" cache="$root/build/mac-release-cache"
while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) build=0; shift;;
        --keep-app) keep_app=1; shift;;
        --app) app="$2"; shift 2;;
        --rt) rt="$2"; shift 2;;
        --harness) harness="$2"; shift 2;;
        --vulkan-sdk) sdk="$2"; shift 2;;
        --kbm-icons) kbm="$2"; shift 2;;
        --out) out="$2"; shift 2;;
        --cache) cache="$2"; shift 2;;
        *) echo "unknown argument $1 (see the header of $0)" >&2; exit 2;;
    esac
done
fail() { echo "make-release-mac: $*" >&2; exit 1; }
[ "$(uname -s)" = Darwin ] || fail "run this on a Mac (it builds and signs the app and thins the Vulkan libraries)"
version="$(tr -d ' \t\r\n' < "$root/VERSION")"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] || fail "VERSION holds '$version', not x.y.z or x.y.z-pre"
[ -f "$rt" ] || fail "no runtime at $rt (cmake --build build/mac --target pkp_rt)"
# the runtime names the release in pkp.log (pkp_main.cpp PKP_VERSION): a runtime from a build folder must have been configured for it
if [ -f "$(dirname "$rt")/CMakeCache.txt" ]; then
    rt_version=$(sed -n 's/^PKP_VERSION:STRING=//p' "$(dirname "$rt")/CMakeCache.txt")
    [ "$rt_version" = "$version" ] || fail "the runtime was built as version '$rt_version', VERSION says $version: cmake -DPKP_VERSION=$version $(dirname "$rt"), then build pkp_rt again"
fi
[ -x "$harness/spirv_harness" ] || fail "no osx-arm64 shader translator in $harness (dotnet publish ... -r osx-arm64)"
[ -f "$sdk/lib/libvulkan.1.dylib" ] || fail "no Vulkan SDK at $sdk (--vulkan-sdk <VulkanSDK>/<version>/macOS)"
[ -d "$kbm" ] || fail "no keyboard and mouse icons at $kbm (--kbm-icons <dir>)"
dotnet_root="${DOTNET_ROOT:-$HOME/.dotnet}"

# the launcher, with the version stamped in for the build only
if [ "$build" = 1 ]; then
    conf="$launcher/src-tauri/tauri.conf.json" toml="$launcher/src-tauri/Cargo.toml" lock="$launcher/src-tauri/Cargo.lock"
    keep="$(mktemp -d)"
    cp "$conf" "$toml" "$lock" "$keep/"
    trap 'cp "$keep/tauri.conf.json" "$conf"; cp "$keep/Cargo.toml" "$toml"; cp "$keep/Cargo.lock" "$lock"; rm -rf "$keep"' EXIT
    sed -E -i '' "s/\"version\": \"[^\"]*\"/\"version\": \"$version\"/" "$conf"
    sed -E -i '' "s/^version = \"[^\"]*\"/version = \"$version\"/" "$toml"
    grep -q "\"version\": \"$version\"" "$conf" && grep -q "^version = \"$version\"" "$toml" || fail "could not stamp the version"
    # panic locations name every crate's source file: the cargo registry under the home folder and this checkout (the later map wins)
    remap="--remap-path-prefix=$HOME=/build --remap-path-prefix=$root=/pokomac"
    (cd "$launcher" && { [ -d node_modules ] || npm ci --no-audit --no-fund; } && RUSTFLAGS="${RUSTFLAGS:+$RUSTFLAGS }$remap" npm run tauri build -- --bundles app) ||
        fail "the launcher build failed"
fi
[ -f "$app/Contents/Info.plist" ] || fail "no launcher app at $app"
app_version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app/Contents/Info.plist" 2>/dev/null || true)
[ "$app_version" = "$version" ] || fail "the launcher app says version '$app_version', VERSION says $version: build it again (without --no-build)"

name="PokoMac-$version-macos-arm64"
pkg="$out/$name"
APP="PokoMac.app"
res="$pkg/$APP/Contents/Resources"
rm -rf "$pkg" "$out/$name.zip" "$out/$name.zip.sha256"
mkdir -p "$pkg"

put() {   # put <from> <to, relative to the package>
    [ -f "$1" ] || fail "missing $1"
    mkdir -p "$(dirname "$pkg/$2")"
    cp -fL "$1" "$pkg/$2"
}
mirror() {   # mirror <from dir> <to dir, relative to the package> [find -not -path patterns...]
    local from="$1" to="$pkg/$2"; shift 2
    [ -d "$from" ] || fail "missing $from"
    rm -rf "$to"; mkdir -p "$to"
    local excl=()
    for p in "$@"; do excl+=(-not -path "$p"); done
    (cd "$from" && find . -type f ${excl[@]+"${excl[@]}"} -print0 | cpio -0pdmL --quiet "$to")   # bash 3.2: an empty array is unset
}
# pinned <file> <url> <sha256>: a download checked like the launcher's (host.rs Pinned), kept in the cache
pinned() {
    local f="$cache/$1"
    mkdir -p "$cache"
    if [ ! -f "$f" ] || [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" != "$3" ]; then
        curl -fsSL --retry 3 -o "$f.part" "$2" || fail "could not download $2"
        [ "$(shasum -a 256 "$f.part" | cut -d' ' -f1)" = "$3" ] || { rm -f "$f.part"; fail "$1 does not match its pinned SHA-256"; }
        mv "$f.part" "$f"
    fi
    echo "$f"
}

# the app, then its read-only payload
ditto "$app" "$pkg/$APP"
R="$APP/Contents/Resources"
# the runtime as shipped (both copies the same file): without debug stabs, signed again ad hoc (strip breaks the linker's signature)
rt_pkg="$pkg/.libpkp_rt.dylib"
cp -fL "$rt" "$rt_pkg" && strip -S "$rt_pkg" && codesign --force --sign - "$rt_pkg" || fail "could not strip and sign the runtime"
rt="$rt_pkg"
put "$rt" "$R/tools/build/sdk/libpkp_rt.dylib"
put "$root/game-build/src/pkp_rt_api.h" "$R/tools/build/sdk/pkp_rt_api.h"
put "$root/game-build/src/pkp_stub.cpp" "$R/tools/build/sdk/pkp_stub.cpp"
for f in pkp_build.py apply_ips.py mods.py cheats_fetch.py dump_hashes.json; do put "$root/game-build/release_b/$f" "$R/tools/build/$f"; done
put "$root/recomp/LICENSE" "$R/tools/build/recomp/LICENSE"
put "$root/recomp/lifter_cli.py" "$R/tools/build/recomp/lifter_cli.py"   # the lifter's own command line (pkp_build.py runs it)
mirror "$root/recomp/nxrecomp" "$R/tools/build/recomp/nxrecomp" '*__pycache__*' '*.pyc'
mirror "$root/recomp/include/nxrecomp" "$R/tools/build/recomp/include/nxrecomp"
for f in CMakeLists.txt config.py main.sites; do put "$root/recomp/games/pkp/$f" "$R/tools/build/recomp/games/pkp/$f"; done
mirror "$harness" "$R/tools/spirv_harness" '*.pdb'
put "$root/tools/shaders/ampere/extract_corpus.py" "$R/tools/shaders/extract_corpus.py"

# the game folder's run-time files (the runtime copies of the dylib and the SDK files, arm64 only: the port is Apple silicon only)
put "$rt" "$R/runtime/libpkp_rt.dylib"
rm -f "$rt_pkg"
for lib in libvulkan.1.dylib libvulkan_kosmickrisp.dylib libMoltenVK.dylib; do
    dst="$res/runtime/$lib"
    if lipo -archs "$sdk/lib/$lib" | grep -qw x86_64; then
        lipo -thin arm64 "$sdk/lib/$lib" -output "$dst"   # each slice keeps its own signature
    else
        cp -fL "$sdk/lib/$lib" "$dst"
    fi
    codesign --verify "$dst" 2>/dev/null || codesign --force --sign - "$dst"
done
mkdir -p "$res/runtime/vulkan/icd.d"
for icd in libkosmickrisp_icd.json:libvulkan_kosmickrisp.dylib MoltenVK_icd.json:libMoltenVK.dylib; do
    json="${icd%%:*}" lib="${icd##*:}"
    [ -f "$sdk/share/vulkan/icd.d/$json" ] || fail "missing $sdk/share/vulkan/icd.d/$json"
    sed -E "s#(\"library_path\"[[:space:]]*:[[:space:]]*)\"[^\"]*\"#\\1\"../../$lib\"#" "$sdk/share/vulkan/icd.d/$json" > "$res/runtime/vulkan/icd.d/$json"
    grep -q "\"../../$lib\"" "$res/runtime/vulkan/icd.d/$json" || fail "could not rewrite library_path in $json"
done
put "$root/runtime/assets/fonts/DejaVuSans.ttf" "$R/runtime/fonts/DejaVuSans.ttf"
put "$root/runtime/assets/fonts/LICENSE-DejaVu.txt" "$R/runtime/fonts/LICENSE-DejaVu.txt"
mirror "$root/runtime/assets/fonts" "$R/runtime/assets/fonts"
mirror "$root/runtime/assets/prompts" "$R/runtime/assets/prompts"
mirror "$kbm" "$R/runtime/assets/kbm_icons"
# the pipeline seed: the prewarm keys (shader hashes and pipeline state, no shader code) of the title, a new game, the character creator
# and the first area, recorded on this port; the setup compiles them for Metal ahead of play (pkp --prepare-pipelines)
put "$root/game-build/release_b/macos/pipeline_seed.nxpk" "$R/runtime/pipeline_seed.nxpk"

# licences: inside the app (an update brings its own) and beside it. game-build/release_b/macos/licenses holds the texts of the Windows
# package's licenses/ that apply here (taken from its base package, as make-release.ps1 ships them; the Windows-only FidelityFX,
# Streamline, NVIDIA RTX SDKs and MSVC runtime ones are left out), libvpx's LICENSE and PATENTS (linked into libpkp_rt.dylib), and the
# Vulkan loader's, MoltenVK's and KosmicKrisp's.
L="$R/licenses"
mirror "$root/game-build/release_b/macos/licenses" "$L"
put "$root/game-build/release_b/macos/BUNDLED_TOOLS.txt" "$L/BUNDLED_TOOLS.txt"
put "$root/runtime/THIRD_PARTY_NOTICES.md" "$L/THIRD_PARTY_NOTICES.md"
# the overlay's fonts are compiled into libpkp_rt.dylib: their licence texts also go with the other licences (as on Windows)
put "$root/runtime/assets/fonts/LICENSE-Inter.txt" "$L/LICENSE.Inter.OFL-1.1.txt"
put "$root/runtime/assets/fonts/LICENSE-CascadiaCode.txt" "$L/LICENSE.CascadiaCode.OFL-1.1.txt"
put "$root/tools/shaders/licenses/LICENSE.mesa-nak.MIT" "$L/LICENSE.mesa-nak.MIT.txt"
put "$dotnet_root/LICENSE.txt" "$L/LICENSE.dotnet.MIT.txt"
put "$dotnet_root/ThirdPartyNotices.txt" "$L/THIRD-PARTY-NOTICES.dotnet.txt"
put "$sdk/../Licenses/LICENSE.txt" "$L/LICENSE.VulkanSDK.txt"
# what Set up downloads (host.rs PYTHON, WHEELS, ZIG for macOS): the licences from the very files it fetches
py=$(pinned cpython-3.13.16-aarch64-apple-darwin.tar.gz "https://github.com/astral-sh/python-build-standalone/releases/download/20261013/cpython-3.13.16%2B20261013-aarch64-apple-darwin-install_only_stripped.tar.gz" 16a5332fb0dcb77f163fc8f8cbccd95be1644cc53e68e5d2a79f50afd6680a0e)
zig=$(pinned zig-aarch64-macos-0.17.0.tar.xz "https://ziglang.org/download/0.17.0/zig-aarch64-macos-0.17.0.tar.xz" b607e9b9234790a008116ae5bdb71c6243b84b9fb42a53a9e70fde41c06c536a)
lz4=$(pinned lz4-4.4.5-cp313-cp313-macosx_11_0_arm64.whl "https://files.pythonhosted.org/packages/8f/3f/ea3334e59de30871d773963997ecdba96c4584c5f8007fd83cfc8f1ee935/lz4-4.4.5-cp313-cp313-macosx_11_0_arm64.whl" b424df1076e40d4e884cfcc4c77d815368b7fb9ebcd7e634f937725cd9a8a72a)
cap=$(pinned capstone-5.0.9-py3-none-macosx_11_0_arm64.whl "https://files.pythonhosted.org/packages/42/9a/4eecfbc94961cda70301fd13de69042be1943bee456e37bc1369840601b1/capstone-5.0.9-py3-none-macosx_11_0_arm64.whl" fc269c5082e17d7f9f265cd6980e2fdc6ab572df913951d76db0357e06b8001d)
tar -xOf "$py" python/lib/python3.13/LICENSE.txt > "$res/licenses/LICENSE.Python.PSF.txt"
tar -xOf "$zig" zig-aarch64-macos-0.17.0/LICENSE > "$res/licenses/LICENSE.zig.MIT.txt"
unzip -p "$lz4" 'lz4-4.4.5.dist-info/licenses/LICENSE' > "$res/licenses/LICENSE.lz4.txt"
unzip -p "$cap" 'capstone-5.0.9.dist-info/LICENSE.TXT' > "$res/licenses/LICENSE.capstone.txt"
for f in "$res"/licenses/*; do [ -s "$f" ] || fail "empty licence file $f"; done
put "$root/LICENSE" "$R/LICENSE.txt"
put "$root/NOTICE" "$R/NOTICE.txt"
put "$root/LICENSE.GPL-3.0.txt" "$L/LICENSE.GPL-3.0.txt"   # the lifter (RELICENSING.md)

# beside the app
sed "s/@VERSION@/$version/g" "$root/game-build/release_b/macos/README.txt" > "$pkg/README.txt"
put "$root/LICENSE" "LICENSE.txt"
mirror "$res/licenses" "licenses"

# never ship anything from a game dump, a build or shader cache made from the game, saves, keys or settings
# (the lifter's own sources have a lift/ package and a games/pkp/ folder: the build-folder names are not looked for inside it)
bad=$(cd "$pkg" && find . \( -iname '*.nso' -o -iname '*.nro' -o -iname '*.nca' -o -iname '*.nsp' -o -iname '*.xci' -o -iname '*.nup' \
    -o -iname '*.spv' -o -iname '*.keys' -o -iname '*.bin' -o -name exefs -o -name romfs -o -name shader_cache -o -name save -o -name saves \
    -o -name main -o -type f -name pkp -o -name pkp.exe -o -name pkp.ini -o -name controls.ini -o -name nup_dump -o -name done.json \
    -o -path '*/updates/*' \) -print; \
    find . -path "./$R/tools/build/recomp" -prune -o \( -name lift -o -name obj -o -name objcache -o -name zig-cache \) -print; \
    find . -type d -name build -not -path "./$R/tools/build" -print)
[ -z "$bad" ] || fail "game-derived or private files in the package:
$bad"
# the licence switch (b42a7cb): the texts of code PokoMac no longer has stay out, its own notices are in (inside the app and beside it)
for d in "$R/licenses" "licenses"; do
    for f in LICENSE.ScarletNX.GPL-3.0.txt LICENSE.yuzu-Eden.GPL-2.0.txt; do [ ! -e "$pkg/$d/$f" ] || fail "the package still has $d/$f (its code is gone: licence switch)"; done
    [ -f "$pkg/$d/LICENSE.GPL-3.0.txt" ] || fail "the package has no $d/LICENSE.GPL-3.0.txt"
done
for f in "$R/NOTICE.txt" "$R/LICENSE.txt" LICENSE.txt "$L/LICENSE.Inter.OFL-1.1.txt" "$L/LICENSE.CascadiaCode.OFL-1.1.txt" "$L/LICENSE.DejaVu.txt"; do
    [ -f "$pkg/$f" ] || fail "the package has no $f"
done
# nothing names the build Mac: its home folder (and so the user name in it) and its host names, in any file, text or binary
needles=(-e "$HOME")
for h in "$(hostname -s 2>/dev/null)" "$(scutil --get LocalHostName 2>/dev/null)" "$(scutil --get ComputerName 2>/dev/null)"; do
    [ ${#h} -ge 4 ] && needles+=(-e "$h")
done
leak=$(grep -rlaF "${needles[@]}" "$pkg" || true)
[ -z "$leak" ] || fail "files in the package name the build Mac (its home folder or host name):
$leak"

# sign the finished bundle again (ad hoc): its resources changed, and a broken seal would stop Gatekeeper's right-click Open
xattr -cr "$pkg/$APP"
codesign --force --sign - --timestamp=none "$pkg/$APP"
codesign --verify --strict "$pkg/$APP" || fail "the signed app does not verify"

ditto -c -k --keepParent "$pkg" "$out/$name.zip"
hash=$(shasum -a 256 "$out/$name.zip" | cut -d' ' -f1)
echo "$hash *$name.zip" > "$out/$name.zip.sha256"
echo "wrote $out/$name.zip ($(( $(stat -f %z "$out/$name.zip") / 1048576 )) MiB), sha256 $hash, launcher version $app_version"

# The zip is the product: the .app copies the build left behind (Tauri's bundle, the packing folder's) are removed and taken out of
# Launch Services, so Launchpad and Spotlight show only the installed app. --keep-app keeps both (a later --no-build needs Tauri's).
lsreg=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
for left in "$launcher/src-tauri/target/release/bundle/macos/$APP" "$pkg/$APP"; do
    [ "${keep_app:-0}" = 1 ] && continue
    [ -d "$left" ] || continue
    "$lsreg" -u "$left" >/dev/null 2>&1 || true
    rm -rf "$left"
done
