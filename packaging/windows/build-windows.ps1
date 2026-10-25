# Build Inductive's Windows installer with KiCad's own Windows builder.
#
#   powershell -ExecutionPolicy Bypass -File build-windows.ps1 -Steps build,prepare,package
#
# Expects, on a Windows machine with Visual Studio 2022 Build Tools (C++ workload) and Git:
#   $Builder            a clone of https://gitlab.com/kicad/packaging/kicad-win-builder
#                       (made against 1b7a908), whose .build\kicad is KiCad 10.0.7 with this
#                       repo's patches and sources applied (tools/install-into-kicad.sh, with
#                       the links replaced by copies), and .build\kicad-{symbols,footprints,
#                       templates} shallow clones of tag 10.0.7
#   $Inductive          this repo (the public export is enough)
#   $CloudEnv           the release cloud.env: the server address and update channel the
#                       installed app uses (the same file the macOS bundle carries)
#
# Steps: init (the builder's tools), vcpkg, build, prepare, package. The first build compiles
# KiCad's dependencies with vcpkg and takes hours; later ones reuse them. The installer and
# its update description land in $Builder\.out:
#   Inductive-<version>-windows-x86_64-setup.exe and .json (for tools/publish-update.sh)
#
# -Sign signs every program and library, then the installer, with Microsoft's Artifact
# Signing (Trusted Signing): $SigningMetadata names the account and certificate profile,
# $SigningDlib is Microsoft's signtool plug-in, and the signing identity is whoever is
# signed in with the Azure CLI on this machine (az login).
param(
    [string]$Steps = 'build,prepare,package',
    [string]$Builder = 'C:\b\kicad-win-builder',
    [string]$Inductive = 'C:\b\inductive',
    [string]$CloudEnv = 'C:\b\cloud.env',
    [switch]$Sign,
    [string]$SigningMetadata = 'C:\b\signing\metadata.json',
    [string]$SigningDlib = 'C:\b\signing\Azure.CodeSigning.Dlib.dll'
)
$ErrorActionPreference = 'Continue'
$env:Path = 'C:\Program Files\Git\cmd;' + $env:Path

function Fail([string]$Message) { Write-Host "FAILED: $Message"; exit 1 }

$version = (Select-String -Path "$Inductive\src\inductive_version.h" -Pattern '#define INDUCTIVE_VERSION "([0-9.]+)"').Matches[0].Groups[1].Value
if (-not $version) { Fail 'no INDUCTIVE_VERSION in src\inductive_version.h' }
Write-Host "Inductive $version"

# KiCad's builder, with Inductive's build options and without 3D models.
$patch = "$Inductive\packaging\windows\kicad-win-builder.patch"
git -C $Builder apply --check --reverse $patch 2>$null
if ($LASTEXITCODE -ne 0) {
    # Not applied, or an older version of it is: start from the builder's own script.
    git -C $Builder checkout -- build.ps1
    git -C $Builder apply $patch
    if ($LASTEXITCODE -ne 0) { Fail 'kicad-win-builder.patch does not apply' }
}

# The installer script, its text and its pictures.
Copy-Item "$Inductive\packaging\windows\inductive.nsi" "$Builder\nsis\" -Force
Copy-Item "$Inductive\packaging\windows\lang\Inductive-English.nsh" "$Builder\nsis\lang\" -Force
Copy-Item "$Inductive\packaging\windows\inductive-welcome.bmp", "$Inductive\packaging\windows\inductive-header.bmp" "$Builder\nsis\" -Force
Copy-Item "$Inductive\brand\inductive.ico" "$Builder\nsis\" -Force

# The build: KiCad's tree as prepared (no refs, so the builder neither fetches nor resets it).
@"
{
    "name": "Inductive $version",
    "output_prefix": "Inductive-",
    "package_version": "$version",
    "train": "stable",
    "build_mode": "Release",
    "nsis": { "file": "inductive.nsi" },
    "sources": { "kicad": {}, "symbols": {}, "footprints": {}, "3dmodels": {}, "templates": {} },
    "vcpkg": { "manifest_mode": true }
}
"@ | Set-Content "$Builder\build-configs\inductive.json"

# Visual Studio's own CMake and Ninja, and vcpkg beside the builder. Written before the builder
# first loads: it makes session-wide tool aliases from these settings as it starts.
$settingsFile = "$Builder\settings.json"
$settings = if (Test-Path $settingsFile) { Get-Content $settingsFile -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
$settings | Add-Member -Force NoteProperty VcpkgPath "$Builder\vcpkg"
$settings | Add-Member -Force NoteProperty UseMsvcCmake $true
$settings | ConvertTo-Json -Compress | Set-Content $settingsFile
Remove-Item alias:cmake, alias:ninja -ErrorAction SilentlyContinue

if ($Sign) {
    if (-not (Test-Path $SigningMetadata)) { Fail "signing needs $SigningMetadata" }
    if (-not (Test-Path $SigningDlib)) { Fail "signing needs $SigningDlib" }
    # Read by the builder's Sign-File (kicad-win-builder.patch).
    $env:INDUCTIVE_SIGNING_METADATA = $SigningMetadata
    $env:INDUCTIVE_SIGNING_DLIB = $SigningDlib
}

Set-Location $Builder

$out = "$Builder\.out\x64-windows-Release"
foreach ($step in $Steps.Split(',')) {
    Write-Host "=== $step $(Get-Date -Format HH:mm:ss) ==="
    $global:LASTEXITCODE = 0
    switch ($step) {
        'init'    { & .\build.ps1 -Init }
        'vcpkg'   { & .\build.ps1 -Vcpkg -Latest -Arch x64 -BuildConfigName inductive }
        'build'   {
            & .\build.ps1 -Build -Arch x64 -BuildConfigName inductive
            # The builder reports success even when CMake did not run: check for the programs.
            foreach ($exe in 'kicad\inductive.exe', 'pcbnew\inductive-pcb.exe', 'eeschema\inductive-schematic.exe') {
                if (-not (Test-Path "$Builder\.build\kicad\build\x64-windows-Release\$exe")) { Fail "the build made no $exe" }
            }
        }
        'prepare' {
            & .\build.ps1 -PreparePackage -Arch x64 -BuildConfigName inductive -Sign:$Sign
            if (-not (Test-Path "$out\share\kicad\inductive\web\sidebar.html")) { Fail 'the package has no sidebar' }
            # The release settings: without them the app cannot reach Inductive or update itself.
            if (-not (Select-String -Path $CloudEnv -Pattern '^INDUCTIVE_API_BASE=https://' -Quiet)) { Fail "$CloudEnv has no INDUCTIVE_API_BASE" }
            if (-not (Select-String -Path $CloudEnv -Pattern '^INDUCTIVE_UPDATE_CHANNEL=' -Quiet)) { Fail "$CloudEnv has no INDUCTIVE_UPDATE_CHANNEL" }
            Copy-Item $CloudEnv "$out\share\kicad\inductive\cloud.env" -Force
        }
        'package' {
            & .\build.ps1 -Package -Arch x64 -BuildConfigName inductive -Prepare $False -Sign:$Sign
            $built = Get-ChildItem "$Builder\.out\Inductive-$version-x86_64*.exe" | Sort-Object LastWriteTime | Select-Object -Last 1
            if (-not $built) { Fail 'no installer was made' }
            $file = "Inductive-$version-windows-x86_64-setup.exe"
            Move-Item $built.FullName "$Builder\.out\$file" -Force
            if ($Sign) {
                foreach ($signed in "$Builder\.out\$file", "$out\bin\inductive.exe", "$out\bin\inductive-pcb.exe", "$out\bin\inductive-schematic.exe") {
                    if ((Get-AuthenticodeSignature $signed).Status -ne 'Valid') { Fail "$signed is not validly signed" }
                }
            }
            $hash = (Get-FileHash "$Builder\.out\$file" -Algorithm SHA256).Hash.ToLower()
            $size = (Get-Item "$Builder\.out\$file").Length
            # What tools/publish-update.sh publishes: the same shape as the macOS build's.
            "{`"version`":`"$version`",`"build`":`"windows-x86_64`",`"file`":`"$file`",`"sha256`":`"$hash`",`"size`":$size}" |
                Set-Content -NoNewline "$Builder\.out\Inductive-$version-windows-x86_64-setup.json"
            Write-Host "installer: $Builder\.out\$file ($size bytes, sha256 $hash)"
        }
    }
    $code = if ($LASTEXITCODE) { $LASTEXITCODE } else { 0 }
    Write-Host "=== $step exit $code $(Get-Date -Format HH:mm:ss) ==="
    if ($code -ne 0) { exit $code }
}
