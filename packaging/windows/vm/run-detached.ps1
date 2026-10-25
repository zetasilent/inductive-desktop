# Run build-windows.ps1 detached from this session, logging to $Root\logs.
#
#   powershell -ExecutionPolicy Bypass -File run-detached.ps1 -Steps init,vcpkg,build,prepare,package [-Sign]
#   type C:\b\logs\status.txt
#
# An SSH session ends its child processes when it disconnects, and a first build takes
# hours, so the build is started through WMI instead: it keeps running on its own, as
# the same user. status.txt says "running ..." until the steps end, then "done ... exit N".
param(
    [string]$Steps = 'build,prepare,package',
    [string]$Root = 'C:\b',
    [switch]$Sign,
    [switch]$Child
)
if (-not $Child) {
    $command = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Child -Steps $Steps -Root $Root"
    if ($Sign) { $command += ' -Sign' }
    $started = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $command; CurrentDirectory = $Root }
    "started (process $($started.ProcessId)); progress: type $Root\logs\status.txt"
    exit 0
}

New-Item -ItemType Directory -Force "$Root\logs" | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
Set-Content "$Root\logs\status.txt" "running $Steps since $stamp log build-$stamp.log"
$arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$Root\inductive\packaging\windows\build-windows.ps1",
               '-Steps', $Steps, '-Builder', "$Root\kicad-win-builder", '-Inductive', "$Root\inductive", '-CloudEnv', "$Root\cloud.env")
if ($Sign) { $arguments += '-Sign' }
& powershell @arguments *> "$Root\logs\build-$stamp.log"
Set-Content "$Root\logs\status.txt" "done $Steps exit $LASTEXITCODE $(Get-Date -Format HH:mm:ss) log build-$stamp.log"
