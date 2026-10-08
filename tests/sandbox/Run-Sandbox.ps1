# Runs a PowerOff test script in a fresh Windows Sandbox and prints its results.
#
#   powershell -File tests\sandbox\Run-Sandbox.ps1                     # end-to-end suite (~11 min)
#   powershell -File tests\sandbox\Run-Sandbox.ps1 -Suite msix         # Store package (~1 min)
#   powershell -File tests\sandbox\Run-Sandbox.ps1 -Suite screenshots  # README screenshots
#
# Needs the Windows Sandbox feature. Build first: build.bat and makensis (main, screenshots),
# or tools\make-msix.ps1 (msix). Files are staged in %TEMP%\poweroff-sandbox and mapped into
# the sandbox as C:\Test; results (results.md, screenshots) land in its results folder.
# The main suite ends with a real forced shutdown of the sandbox, run by PowerOff itself.
param([ValidateSet('main', 'msix', 'screenshots')][string]$Suite = 'main', [int]$TimeoutMinutes = 25)
$ErrorActionPreference = 'Stop'
$root  = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$stage = Join-Path $env:TEMP 'poweroff-sandbox'
$script = @{ main = 'run-tests.ps1'; msix = 'msix-tests.ps1'; screenshots = 'screenshots.ps1' }[$Suite]

if (Get-Process WindowsSandboxRemoteSession -ErrorAction SilentlyContinue) { throw 'Windows Sandbox is already running: close it first' }
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage 'results') | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'helpers.ps1'), (Join-Path $PSScriptRoot $script) $stage
if ($Suite -eq 'msix') {
    $layout = Join-Path $root 'build\msix'
    if (-not (Test-Path (Join-Path $layout 'AppxManifest.xml'))) { throw 'build\msix not found: run tools\make-msix.ps1 first' }
    Copy-Item -Recurse $layout (Join-Path $stage 'layout')
} else {
    foreach ($f in 'poweroff.exe', 'PowerOff-setup.exe') {
        if (-not (Test-Path (Join-Path $root $f))) { throw "$f not found: run build.bat and makensis first" }
        Copy-Item (Join-Path $root $f) $stage
    }
}

$wsb = Join-Path $stage 'poweroff.wsb'
@"
<Configuration>
  <VGpu>Enable</VGpu>
  <Networking>Disable</Networking>
  <MappedFolders>
    <MappedFolder>
      <HostFolder>$stage</HostFolder>
      <SandboxFolder>C:\Test</SandboxFolder>
      <ReadOnly>false</ReadOnly>
    </MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File C:\Test\$script</Command>
  </LogonCommand>
</Configuration>
"@ | Set-Content -Encoding UTF8 $wsb

Start-Process $wsb
$results = Join-Path $stage 'results'
$t0 = Get-Date
Write-Host "Windows Sandbox running $script ..."
while (-not (Test-Path (Join-Path $results 'DONE'))) {
    if (((Get-Date) - $t0).TotalMinutes -gt $TimeoutMinutes) { throw "no result after $TimeoutMinutes minutes" }
    Start-Sleep -Seconds 10
}
Get-Content (Join-Path $results 'results.md')
Write-Host "`nResults and screenshots: $results"
