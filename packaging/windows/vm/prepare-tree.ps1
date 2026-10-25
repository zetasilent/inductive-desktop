# Set up, or refresh, the Windows build tree from make-overlay.sh's output.
#
#   powershell -ExecutionPolicy Bypass -File prepare-tree.ps1 [-Root C:\b]
#
# Expects $Root\overlay.patch and $Root\inductive.tar (packaging/windows/make-overlay.sh).
# The first run clones KiCad's Windows builder (pinned to the commit our patch is made
# against), KiCad 10.0.7 and its 10.0.7 libraries. Later runs reset KiCad's tree to stock
# 10.0.7 and apply the new overlay, keeping the build folder and its vcpkg dependencies,
# so the next build only recompiles what changed.
param(
    [string]$Root = 'C:\b',
    [string]$BuilderCommit = '1b7a908'
)
$ErrorActionPreference = 'Continue'   # git reports progress on stderr
$env:Path = 'C:\Program Files\Git\cmd;' + $env:Path
function Fail([string]$Message) { Write-Host "FAILED: $Message"; exit 1 }

foreach ($f in 'overlay.patch', 'inductive.tar') {
    if (-not (Test-Path "$Root\$f")) { Fail "no $Root\$f (make it with packaging/windows/make-overlay.sh)" }
}
git config --global core.autocrlf false
git config --global core.longpaths true

$builder = "$Root\kicad-win-builder"
if (-not (Test-Path $builder)) {
    git clone --quiet https://gitlab.com/kicad/packaging/kicad-win-builder.git $builder
    git -C $builder checkout --quiet $BuilderCommit
    if ($LASTEXITCODE -ne 0) { Fail "kicad-win-builder has no commit $BuilderCommit" }
}

$build = "$builder\.build"
New-Item -ItemType Directory -Force $build | Out-Null
$kicad = "$build\kicad"
if (-not (Test-Path $kicad)) {
    git clone --quiet --depth 1 --branch 10.0.7 https://gitlab.com/kicad/code/kicad.git $kicad
}
else {
    git -C $kicad checkout -- .
    git -C $kicad clean -fdq -e build/
}
git -C $kicad apply --whitespace=nowarn "$Root\overlay.patch"
if ($LASTEXITCODE -ne 0) { Fail 'the overlay does not apply to KiCad 10.0.7' }

foreach ($lib in 'kicad-symbols', 'kicad-footprints', 'kicad-templates') {
    if (-not (Test-Path "$build\$lib")) {
        git clone --quiet --depth 1 --branch 10.0.7 "https://gitlab.com/kicad/libraries/$lib.git" "$build\$lib"
    }
}
New-Item -ItemType Directory -Force "$build\kicad-packages3D" | Out-Null   # Inductive ships no 3D models

# This repository's build scripts and installer, fresh each time.
if (Test-Path "$Root\inductive") { Remove-Item -Recurse -Force "$Root\inductive" -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force "$Root\inductive" | Out-Null
tar -xf "$Root\inductive.tar" -C "$Root\inductive"

"ready: KiCad 10.0.7 with $((git -C $kicad status --short | Measure-Object).Count) paths changed by the overlay"
if (-not (Test-Path "$Root\cloud.env")) { "note: put the release settings in $Root\cloud.env before the prepare step" }
