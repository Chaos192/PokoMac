# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Packs the player zip (release model b: no game code or data in it; the player's PC lifts and links pkp.exe from their own dump).
# Everything built from this repo is taken from the given build outputs and the checkout; the unchanged third-party payload (Zig, embedded
# Python, FSR 3 / Streamline DLLs, astcdec, fonts, input icons, licence texts) comes from a base package (the upstream v0.1.1 zip).
# ponytail: base zip for the third-party payload; fetch each piece from its own source when one of them needs a new version.
#   pwsh game-build\scripts\make-release.ps1 -Version 0.2.0 -Base PokopiaRecomp-0.1.1-win64.zip -RtDir game-build\build\rtdll `
#        -Launcher launcher\src-tauri\target\release\pokopia-launcher.exe -Harness work\rel\harness -Out work\rel
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Base,       # upstream player zip
    [Parameter(Mandatory)][string]$RtDir,      # folder with pkp_rt.dll + pkp_rt.lib (game-build, PKP_RT_DLL=ON PKP_RELEASE=ON)
    [Parameter(Mandatory)][string]$Launcher,   # pokopia-launcher.exe (launcher, npm run tauri build)
    [Parameter(Mandatory)][string]$Harness,    # self-contained spirv_harness publish folder
    [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$name = "PokoMac-$Version"
$stage = Join-Path $Out 'stage'
$pkg = Join-Path $stage $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# base payload, its top folder renamed
[IO.Compression.ZipFile]::ExtractToDirectory((Resolve-Path $Base).Path, $stage)
$top = Get-ChildItem $stage -Directory | Select-Object -First 1
Rename-Item $top.FullName $name
# the upstream base carries its own launcher under the old name; this package's is PokoMac.exe (Put below)
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $pkg 'PokopiaLauncher.exe')
# the base still carries the licence texts of code PokoMac no longer has (rewritten clean-room before the licence switch, RELICENSING.md)
foreach ($f in 'LICENSE.ScarletNX.GPL-3.0.txt', 'LICENSE.yuzu-Eden.GPL-2.0.txt') { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $pkg "licenses\$f") }

function Put([string]$from, [string]$to) {
    $dst = Join-Path $pkg $to
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item -Force $from $dst
}
function Mirror([string]$from, [string]$to, [string[]]$exclude = @()) {
    $dst = Join-Path $pkg $to
    if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
    New-Item -ItemType Directory -Force $dst | Out-Null
    $src = (Resolve-Path $from).Path
    Get-ChildItem $src -Recurse -File | Where-Object {
        $rel = $_.FullName.Substring($src.Length + 1)
        -not ($exclude | Where-Object { $rel -like $_ })
    } | ForEach-Object {
        $t = Join-Path $dst $_.FullName.Substring($src.Length + 1)
        New-Item -ItemType Directory -Force (Split-Path -Parent $t) | Out-Null
        Copy-Item $_.FullName $t
    }
}

# built here
Put (Join-Path $RtDir 'pkp_rt.dll') 'pkp_rt.dll'
Put (Join-Path $RtDir 'pkp_rt.lib') 'tools\build\sdk\pkp_rt.lib'
Put $Launcher 'PokoMac.exe'
Mirror $Harness 'tools\spirv_harness' @('*.pdb')
# from the checkout
Put "$root\game-build\src\pkp_rt_api.h" 'tools\build\sdk\pkp_rt_api.h'
Put "$root\game-build\src\pkp_stub.cpp" 'tools\build\sdk\pkp_stub.cpp'
Put "$root\game-build\res\pkp.manifest" 'tools\build\sdk\pkp.manifest'
Put "$root\game-build\res\pkp.rc" 'tools\build\sdk\pkp.rc'
foreach ($f in 'pkp_build.py', 'apply_ips.py', 'mods.py', 'cheats_fetch.py', 'dump_hashes.json', 'fetch_dlss.py') { Put "$root\game-build\release_b\$f" "tools\build\$f" }
(Get-Content -Raw "$root\game-build\release_b\README.txt").Replace('@VERSION@', $Version) | Set-Content -NoNewline (Join-Path $pkg 'README.txt')
Put "$root\game-build\release_b\BUNDLED_TOOLS.txt" 'licenses\BUNDLED_TOOLS.txt'
Put "$root\launcher\release\Play.bat" 'Play.bat'
Put "$root\LICENSE" 'LICENSE.txt'
Put "$root\NOTICE" 'NOTICE.txt'
Put "$root\LICENSE.GPL-3.0.txt" 'licenses\LICENSE.GPL-3.0.txt'   # the lifter (RELICENSING.md)
Put "$root\runtime\THIRD_PARTY_NOTICES.md" 'licenses\THIRD_PARTY_NOTICES.md'   # what is compiled into pkp_rt.dll, with its licence texts
Put "$root\CHANGELOG.md" 'CHANGELOG.txt'
Put "$root\tools\shaders\ampere\extract_corpus.py" 'tools\shaders\extract_corpus.py'
Mirror "$root\runtime\assets\prompts" 'assets\prompts'   # button prompt icon pack prompt_icons.pak (Kenney CC0, licence in the folder)
Put "$root\recomp\LICENSE" 'tools\build\recomp\LICENSE'
Put "$root\recomp\lifter_cli.py" 'tools\build\recomp\lifter_cli.py'   # the lifter's own command line (pkp_build.py runs it)
Mirror "$root\recomp\nxrecomp" 'tools\build\recomp\nxrecomp' @('*__pycache__*', '*.pyc')
Mirror "$root\recomp\include\nxrecomp" 'tools\build\recomp\include\nxrecomp'
foreach ($f in 'CMakeLists.txt', 'config.py', 'main.sites') { Put "$root\recomp\games\pkp\$f" "tools\build\recomp\games\pkp\$f" }

# never ship anything from a game dump, a shader cache built from the game, saves or keys
$bad = Get-ChildItem $pkg -Recurse -Force | Where-Object {
    $_.Name -match '\.(nso|nro|nca|nsp|xci|nup|spv|keys|bin)$' -or $_.Name -in 'exefs', 'romfs', 'shader_cache', 'save', 'saves', 'main', 'pkp.exe', 'pkp.ini' -or
    $_.FullName -match '\\updates\\'
}
if ($bad) { $bad | ForEach-Object { Write-Error "game-derived or private file in the package: $($_.FullName)" -ErrorAction Continue }; throw 'package check failed' }

$zip = Join-Path $Out "$name-win64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
[IO.Compression.ZipFile]::CreateFromDirectory($pkg, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
Write-Host "wrote $zip ($([math]::Round((Get-Item $zip).Length / 1MB)) MiB)"
