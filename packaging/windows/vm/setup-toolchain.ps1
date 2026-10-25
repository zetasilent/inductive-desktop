# One-time setup of a Windows build machine: Git for Windows and Visual Studio 2022
# Build Tools with the C++ workload, the Windows SDK and CMake. Run as an administrator.
#
#   powershell -ExecutionPolicy Bypass -File setup-toolchain.ps1 [-Root C:\b]
#
# Both installers come from their publishers and their signatures are printed before
# they run. Defender's real-time scan skips $Root only: scanning every compiler output
# makes the build several times slower.
param([string]$Root = 'C:\b')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
New-Item -ItemType Directory -Force $Root | Out-Null
Set-Location $Root
Add-MpPreference -ExclusionPath $Root

$release = Invoke-RestMethod https://api.github.com/repos/git-for-windows/git/releases/latest
$asset = $release.assets | Where-Object { $_.name -match '^Git-[\d.]+-64-bit\.exe$' } | Select-Object -First 1
Invoke-WebRequest $asset.browser_download_url -OutFile git-setup.exe
"git: $($asset.name), signed by $((Get-AuthenticodeSignature .\git-setup.exe).SignerCertificate.Subject)"
Start-Process .\git-setup.exe -ArgumentList '/VERYSILENT', '/NORESTART', '/NOCANCEL', '/SP-', '/SUPPRESSMSGBOXES' -Wait

Invoke-WebRequest https://aka.ms/vs/17/release/vs_BuildTools.exe -OutFile vs_BuildTools.exe
"Visual Studio Build Tools, signed by $((Get-AuthenticodeSignature .\vs_BuildTools.exe).SignerCertificate.Subject)"
$vs = Start-Process .\vs_BuildTools.exe -Wait -PassThru -ArgumentList '--quiet', '--wait', '--norestart', '--nocache',
    '--add', 'Microsoft.VisualStudio.Workload.VCTools',
    '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.22621',
    '--add', 'Microsoft.VisualStudio.Component.VC.CMake.Project',
    '--add', 'Microsoft.VisualStudio.Component.VC.ATL'
"Visual Studio Build Tools exit code: $($vs.ExitCode) (0 or 3010 is fine)"
& 'C:\Program Files\Git\cmd\git.exe' --version
