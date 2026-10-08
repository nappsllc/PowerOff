# Rasterizes the Fluent color icons in icons\svg\ to the PNGs embedded in poweroff.exe.
# Uses headless Edge (always present on Windows 11) so SVG gradients render exactly as
# in the design. Re-run only when an SVG changes; the PNGs are committed.
#   pwsh tools\make-icons.ps1
Add-Type -AssemblyName System.Drawing
$root  = Split-Path $PSScriptRoot -Parent
$svgs  = Join-Path $root 'icons\svg'
$pngs  = Join-Path $root 'icons'
$edge  = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
           "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found' }

$jobs = @()
# One 48 px master per icon; the app scales it (WIC, Fant filter) to the exact pixel size
# it needs at the current DPI: 20 px rows, 32 px hero, x1.25 / x1.5 / x2 scaling.
Get-ChildItem $svgs -Filter *.svg | Where-Object BaseName -ne 'app' | ForEach-Object { $jobs += @{ name = $_.BaseName; size = 48 } }

# One sheet, one icon per 56x56 cell, then crop: avoids per-file browser launches.
$cell = 56; $cols = 8; $rows = [Math]::Ceiling($jobs.Count / $cols)
$html = '<!doctype html><html><body style="margin:0;background:transparent">'
for ($i = 0; $i -lt $jobs.Count; $i++) {
    $j = $jobs[$i]; $x = ($i % $cols) * $cell; $y = [Math]::Floor($i / $cols) * $cell
    $uri = ([Uri](Join-Path $svgs "$($j.name).svg")).AbsoluteUri
    $html += "<img src='$uri' style='position:absolute;left:${x}px;top:${y}px;width:$($j.size)px;height:$($j.size)px'>"
}
$html += '</body></html>'
$tmp = Join-Path $env:TEMP 'poweroff-icons'
New-Item -ItemType Directory -Force $tmp | Out-Null
$page = Join-Path $tmp 'sheet.html'; $shot = Join-Path $tmp 'sheet.png'
Set-Content $page $html -Encoding utf8
Remove-Item $shot -ErrorAction SilentlyContinue
& $edge --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 `
    --default-background-color=00000000 --window-size="$($cols * $cell),$($rows * $cell)" `
    --user-data-dir="$tmp\profile" --screenshot="$shot" ([Uri]$page).AbsoluteUri 2>$null | Out-Null
for ($t = 0; $t -lt 50 -and -not (Test-Path $shot); $t++) { Start-Sleep -Milliseconds 200 }
if (-not (Test-Path $shot)) { throw 'Edge did not produce a screenshot' }

$sheet = [System.Drawing.Bitmap]::FromFile($shot)
for ($i = 0; $i -lt $jobs.Count; $i++) {
    $j = $jobs[$i]; $x = ($i % $cols) * $cell; $y = [Math]::Floor($i / $cols) * $cell
    $bmp = $sheet.Clone((New-Object System.Drawing.Rectangle $x, $y, $j.size, $j.size), [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bmp.Save((Join-Path $pngs "$($j.name)_$($j.size).png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
$sheet.Dispose()
"wrote $($jobs.Count) PNGs to $pngs"
