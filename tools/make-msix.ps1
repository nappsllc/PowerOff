# Packages poweroff.exe as an MSIX for the Microsoft Store.
#
#   powershell -File tools\make-msix.ps1 -Name <Package/Identity/Name> -Publisher "CN=<...>" `
#       -PublisherDisplayName <...> [-Version 1.0.0.0]
#
# Take Name, Publisher and PublisherDisplayName from Partner Center (your app > Product
# management > Product identity); the defaults are placeholders for local testing.
# Output: PowerOff.msix, unsigned - the Store signs it on submission. For a local install,
# sign it with a certificate whose subject equals -Publisher (-CertPfx), or register the
# layout folder with Developer Mode on (Add-AppxPackage -Register build\msix\AppxManifest.xml).
#
# The tile and taskbar assets are generated from poweroff.ico into assets\msix the first time
# (or with -RegenerateAssets) and committed, so the build itself needs no image tools.
param(
    [string]$Name = 'PowerNapps.PowerOff',
    [string]$Publisher = 'CN=PowerNapps',
    [string]$PublisherDisplayName = 'PowerNapps',
    [string]$Version = '1.0.1.0',          # the Store requires the last part to be 0
    [string]$CertPfx = '',
    [string]$CertPassword = '',
    [switch]$RegenerateAssets
)
$ErrorActionPreference = 'Stop'
$root   = Split-Path $PSScriptRoot -Parent
$assets = Join-Path $root 'assets\msix'
$layout = Join-Path $root 'build\msix'
$out    = Join-Path $root 'PowerOff.msix'
$exe    = Join-Path $root 'poweroff.exe'
if (-not (Test-Path $exe)) { throw 'poweroff.exe not found: run build.bat first' }

# newest Windows SDK that has the packaging tools
$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\makeappx.exe" |
    Sort-Object { [version]$_.Directory.Parent.Name } | Select-Object -Last 1 | ForEach-Object { $_.DirectoryName }
if (-not $sdk) { throw 'Windows SDK packaging tools (makeappx.exe) not found' }

# ------------------------------------------------------------------ assets (from poweroff.ico)
if ($RegenerateAssets -or -not (Test-Path $assets)) {
    Add-Type -AssemblyName System.Drawing
    New-Item -ItemType Directory -Force $assets | Out-Null
    # every frame of the .ico by size: PNG frames load directly, 32-bit DIB frames via Icon
    $ico = [IO.File]::ReadAllBytes((Join-Path $root 'poweroff.ico'))
    $frames = @{}
    for ($i = 0; $i -lt [BitConverter]::ToUInt16($ico, 4); $i++) {
        $e = 6 + 16 * $i; $w = if ($ico[$e]) { $ico[$e] } else { 256 }
        $len = [BitConverter]::ToInt32($ico, $e + 8); $off = [BitConverter]::ToInt32($ico, $e + 12)
        $data = New-Object byte[] $len; [Array]::Copy($ico, $off, $data, 0, $len)
        if ($data[0] -eq 0x89) { $frames[$w] = [Drawing.Bitmap]::FromStream((New-Object IO.MemoryStream (, $data))) }
        else {
            $one = New-Object byte[] (22 + $len)   # single-frame .ico around this DIB
            [Array]::Copy($ico, 0, $one, 0, 6); $one[4] = 1; $one[5] = 0
            [Array]::Copy($ico, $e, $one, 6, 12); [Array]::Copy([BitConverter]::GetBytes(22), 0, $one, 18, 4)
            [Array]::Copy($data, 0, $one, 22, $len)
            $frames[$w] = (New-Object Drawing.Icon (New-Object IO.MemoryStream (, $one)), $w, $w).ToBitmap()
        }
    }
    function Save-Icon([string]$file, [int]$canvas, [int]$size) {
        # the exact frame if the .ico has it, else the 256 px frame scaled down; centered
        $src = if ($frames.ContainsKey($size)) { $frames[$size] } else { $frames[256] }
        $bmp = New-Object Drawing.Bitmap $canvas, $canvas, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'; $g.SmoothingMode = 'HighQuality'
        $o = [int](($canvas - $size) / 2)
        $g.DrawImage($src, (New-Object Drawing.Rectangle $o, $o, $size, $size))
        $g.Dispose(); $bmp.Save((Join-Path $assets $file), [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    }
    # Start menu / taskbar: plated (scale) and unplated (targetsize) variants
    foreach ($s in 100, 200) { $c = [int](44 * $s / 100); Save-Icon "Square44x44Logo.scale-$s.png" $c ([int]($c * 0.84)) }
    foreach ($t in 16, 24, 32, 48, 256) {
        Save-Icon "Square44x44Logo.targetsize-$t.png" $t $t
        Save-Icon "Square44x44Logo.targetsize-${t}_altform-unplated.png" $t $t
    }
    # tiles and the Store logo
    foreach ($s in 100, 200) {
        $c = [int](150 * $s / 100); Save-Icon "Square150x150Logo.scale-$s.png" $c ([int]($c * 0.5))
        $c = [int](50 * $s / 100);  Save-Icon "StoreLogo.scale-$s.png" $c $c
    }
    if (Test-Path (Join-Path $root 'tools\optimize-png.py')) {
        $py = Get-Command python -ErrorAction SilentlyContinue
        if ($py) { & $py.Source (Join-Path $root 'tools\optimize-png.py') @(Get-ChildItem $assets -Filter *.png | ForEach-Object FullName) | Out-Null }
    }
    Write-Host "assets -> $assets"
}

# ------------------------------------------------------------------ layout
if (Test-Path $layout) { Remove-Item -Recurse -Force $layout }
New-Item -ItemType Directory -Force (Join-Path $layout 'Assets') | Out-Null
Copy-Item $exe $layout
Copy-Item (Join-Path $assets '*.png') (Join-Path $layout 'Assets')

$esc = { param($s) [Security.SecurityElement]::Escape($s) }
@"
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         xmlns:uap3="http://schemas.microsoft.com/appx/manifest/uap/windows10/3"
         xmlns:desktop="http://schemas.microsoft.com/appx/manifest/desktop/windows10"
         xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
         IgnorableNamespaces="uap uap3 desktop rescap">
  <Identity Name="$(& $esc $Name)" Publisher="$(& $esc $Publisher)" Version="$Version" ProcessorArchitecture="x64" />
  <Properties>
    <DisplayName>PowerNapps PowerOff</DisplayName>
    <PublisherDisplayName>$(& $esc $PublisherDisplayName)</PublisherDisplayName>
    <Logo>Assets\StoreLogo.png</Logo>
    <Description>Shut down, restart or sleep your PC on a schedule.</Description>
  </Properties>
  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.26100.0" />
  </Dependencies>
  <Resources>
    <Resource Language="en-us" />
  </Resources>
  <Applications>
    <Application Id="PowerOff" Executable="poweroff.exe" EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements DisplayName="PowerNapps PowerOff" Description="Shut down, restart or sleep your PC on a schedule."
          BackgroundColor="transparent" Square150x150Logo="Assets\Square150x150Logo.png" Square44x44Logo="Assets\Square44x44Logo.png" />
      <Extensions>
        <!-- Task Scheduler can't start an exe inside WindowsApps: the app's tasks run this alias -->
        <uap3:Extension Category="windows.appExecutionAlias" Executable="poweroff.exe" EntryPoint="Windows.FullTrustApplication">
          <uap3:AppExecutionAlias>
            <desktop:ExecutionAlias Alias="poweroff.exe" />
          </uap3:AppExecutionAlias>
        </uap3:Extension>
      </Extensions>
    </Application>
  </Applications>
  <Capabilities>
    <rescap:Capability Name="runFullTrust" />
  </Capabilities>
</Package>
"@ | Set-Content -Encoding UTF8 (Join-Path $layout 'AppxManifest.xml')

# resource index, so Windows picks the scale / targetsize variants
$pricfg = Join-Path $root 'build\priconfig.xml'
& "$sdk\makepri.exe" createconfig /cf $pricfg /dq en-US /pv 10.0.0 /o | Out-Null
if ($LASTEXITCODE) { throw 'makepri createconfig failed' }
& "$sdk\makepri.exe" new /pr $layout /cf $pricfg /mn (Join-Path $layout 'AppxManifest.xml') /of (Join-Path $layout 'resources.pri') /o | Out-Null
if ($LASTEXITCODE) { throw 'makepri new failed' }

# ------------------------------------------------------------------ pack (and optionally sign)
& "$sdk\makeappx.exe" pack /o /d $layout /p $out | Select-Object -Last 1
if ($LASTEXITCODE) { throw 'makeappx pack failed' }
if ($CertPfx) {
    & "$sdk\signtool.exe" sign /fd SHA256 /f $CertPfx /p $CertPassword $out
    if ($LASTEXITCODE) { throw 'signtool failed' }
}
Get-Item $out | Select-Object Name, Length
