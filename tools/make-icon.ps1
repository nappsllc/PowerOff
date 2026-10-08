# Generates poweroff.ico from the design's app icon (icons\svg\app.svg: power symbol with
# a #29C3FF -> #2052CB gradient), 16..256 px, rendered by headless Edge.
#   pwsh tools\make-icon.ps1   (writes poweroff.ico next to poweroff.c)
Add-Type -AssemblyName System.Drawing

$root  = Split-Path $PSScriptRoot -Parent
$svg   = Join-Path $root 'icons\svg\app.svg'
$out   = Join-Path $root 'poweroff.ico'
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$edge  = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
           "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found' }

# all sizes side by side on one transparent sheet, then crop
$xs = @(); $x = 0
foreach ($s in $sizes) { $xs += $x; $x += $s + 8 }
$uri  = ([Uri]$svg).AbsoluteUri
$html = '<!doctype html><html><body style="margin:0;background:transparent">'
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $html += "<img src='$uri' style='position:absolute;left:$($xs[$i])px;top:0;width:$($sizes[$i])px;height:$($sizes[$i])px'>"
}
$html += '</body></html>'
$tmp = Join-Path $env:TEMP 'poweroff-appicon'
New-Item -ItemType Directory -Force $tmp | Out-Null
$page = Join-Path $tmp 'sheet.html'; $shot = Join-Path $tmp 'sheet.png'
Set-Content $page $html -Encoding utf8
Remove-Item $shot -ErrorAction SilentlyContinue
& $edge --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 `
    --default-background-color=00000000 --window-size="$x,256" `
    --user-data-dir="$tmp\profile" --screenshot="$shot" ([Uri]$page).AbsoluteUri 2>$null | Out-Null
for ($t = 0; $t -lt 50 -and -not (Test-Path $shot); $t++) { Start-Sleep -Milliseconds 200 }
if (-not (Test-Path $shot)) { throw 'Edge did not produce a screenshot' }

$sheet = [System.Drawing.Bitmap]::FromFile($shot)
$images = for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $bmp = $sheet.Clone((New-Object System.Drawing.Rectangle $xs[$i], 0, $s, $s), [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $ms = New-Object System.IO.MemoryStream
    if ($s -gt 32) {
        # PNG above 32 px; 16-32 px (the tray and title-bar sizes at 100-200%) as plain 32-bit
        # DIBs, so showing the tray icon never loads the PNG decoder (windowscodecs) into the process
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    } else {
        $w = New-Object System.IO.BinaryWriter $ms
        $w.Write([UInt32]40); $w.Write([Int32]$s); $w.Write([Int32]($s * 2))   # height x2: XOR + AND
        $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]0)
        $w.Write([UInt32]($s * $s * 4)); $w.Write([Int32]0); $w.Write([Int32]0); $w.Write([UInt32]0); $w.Write([UInt32]0)
        for ($y = $s - 1; $y -ge 0; $y--) {                                    # bottom-up BGRA
            for ($x = 0; $x -lt $s; $x++) { $c = $bmp.GetPixel($x, $y); $w.Write([byte]$c.B); $w.Write([byte]$c.G); $w.Write([byte]$c.R); $w.Write([byte]$c.A) }
        }
        $stride = [Math]::Ceiling($s / 32) * 4                                 # AND mask: unused with alpha, all 0
        $w.Write((New-Object byte[] ($stride * $s)))
        $w.Flush()
    }
    $bmp.Dispose()
    , $ms.ToArray()
}
$sheet.Dispose()

# ICO container: DIB entries up to 32 px, PNG above (supported since Windows Vista)
$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $images[$i].Length
    $bw.Write([byte]($s % 256)); $bw.Write([byte]($s % 256))   # 256 is stored as 0
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$len); $bw.Write([UInt32]$offset)
    $offset += $len
}
foreach ($img in $images) { $bw.Write($img) }
$bw.Close()
"wrote $out ($((Get-Item $out).Length) bytes)"
