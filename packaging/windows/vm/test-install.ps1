# Install an Inductive installer silently into a test folder and check what it put there.
# Works without a screen (a build server); the real test is a person using the app.
#
#   powershell -ExecutionPolicy Bypass -File test-install.ps1 [-Installer path\to\setup.exe]
param(
    [string]$Installer = (Get-ChildItem 'C:\b\kicad-win-builder\.out\Inductive-*-windows-x86_64-setup.exe' |
                          Sort-Object LastWriteTime | Select-Object -Last 1).FullName,
    [string]$Dest = 'C:\itest\Inductive'
)
$ErrorActionPreference = 'Continue'
"installer: $Installer"
"signature: $((Get-AuthenticodeSignature $Installer).Status) $((Get-AuthenticodeSignature $Installer).SignerCertificate.Subject)"
Remove-Item -Recurse -Force (Split-Path $Dest) -ErrorAction SilentlyContinue
$run = Start-Process -FilePath $Installer -ArgumentList '/S', '/currentuser', "/D=$Dest" -Wait -PassThru
"installer exit code: $($run.ExitCode)"

foreach ($file in 'bin\inductive.exe', 'bin\inductive-pcb.exe', 'bin\inductive-schematic.exe', 'bin\kicad-cli.exe',
                  'bin\python.exe', 'uninstall.exe', 'share\kicad\inductive\web\sidebar.html',
                  'share\kicad\inductive\cloud.env',
                  'share\kicad\inductive\examples\circuits\led.json') {
    "{0,-56} {1}" -f $file, (Test-Path (Join-Path $Dest $file))
}
"symbol libraries:    $((Get-ChildItem "$Dest\share\kicad\symbols" -Filter *.kicad_sym -ErrorAction SilentlyContinue).Count)"
"footprint libraries: $((Get-ChildItem "$Dest\share\kicad\footprints" -Directory -ErrorAction SilentlyContinue).Count)"
"cloud.env keys:      $((Get-Content "$Dest\share\kicad\inductive\cloud.env" | ForEach-Object { ($_ -split '=')[0] }) -join ' ')"
"kicad-cli:           $(& "$Dest\bin\kicad-cli.exe" version 2>&1)"
"python:              $(& "$Dest\bin\python.exe" -c 'import ssl, sqlite3, ctypes; print(\"ok\", ssl.OPENSSL_VERSION)' 2>&1)"
"inductive.exe:       $((Get-Item "$Dest\bin\inductive.exe").VersionInfo.FileDescription), signature $((Get-AuthenticodeSignature "$Dest\bin\inductive.exe").Status)"
"start menu:          $((Get-ChildItem "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Inductive" -ErrorAction SilentlyContinue).Name -join ', ')"
"uninstall entry:     $((Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Inductive' -ErrorAction SilentlyContinue).DisplayName)"
