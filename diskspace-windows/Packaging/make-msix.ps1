# Builds the MSIX package for Store distribution.
#
#   .\Packaging\make-msix.ps1              build a loose layout and register it
#                                          (needs Developer Mode; no signing)
#   .\Packaging\make-msix.ps1 -Pack        also produce DiskSpace.msix
#   .\Packaging\make-msix.ps1 -Pack -Sign  sign it too, for installing without
#                                          Developer Mode
#
# The Store signs submissions itself, so -Sign exists only for testing an install
# the way a user would see it.

param(
    [string]$Configuration = "release",
    [switch]$Pack,
    [switch]$Sign,
    [switch]$Register,
    [string]$CertSubject = "CN=Chofter Apps"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$layout = Join-Path $build "msix"
$exe = Join-Path $build "DiskSpace.exe"

if (-not (Test-Path $exe)) { throw "$exe not found — run build.cmd first" }

# The SDK's packaging tools, from whichever kit is newest.
$kits = "C:\Program Files (x86)\Windows Kits\10\bin"
$kitVersion = (Get-ChildItem $kits -Directory |
    Where-Object { $_.Name -like "10.*" } | Sort-Object Name -Descending |
    Select-Object -First 1).Name
$tools = Join-Path $kits "$kitVersion\x64"
$makeappx = Join-Path $tools "makeappx.exe"
$signtool = Join-Path $tools "signtool.exe"

# ---- Layout ---------------------------------------------------------------

if (Test-Path $layout) { Remove-Item -Recurse -Force $layout }
New-Item -ItemType Directory -Force -Path (Join-Path $layout "Assets") | Out-Null

Copy-Item $exe (Join-Path $layout "DiskSpace.exe")
Copy-Item (Join-Path $PSScriptRoot "AppxManifest.xml") (Join-Path $layout "AppxManifest.xml")

# Tile images, resized from the shared artwork. Every size the manifest names has
# to exist or the package is rejected at registration.
Add-Type -AssemblyName System.Drawing
$artworkPath = Join-Path $root "..\assets\images\icon_1024.png"
$artwork = [System.Drawing.Image]::FromFile((Resolve-Path $artworkPath))
$tiles = @{
    "Square44x44Logo.png"   = @(44, 44)
    "Square71x71Logo.png"   = @(71, 71)
    "Square150x150Logo.png" = @(150, 150)
    "Square310x310Logo.png" = @(310, 310)
    "Wide310x150Logo.png"   = @(310, 150)
    "StoreLogo.png"         = @(50, 50)
}
foreach ($name in $tiles.Keys) {
    $w, $h = $tiles[$name]
    $bitmap = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.InterpolationMode = "HighQualityBicubic"
    $g.PixelOffsetMode = "HighQuality"
    # The wide tile is not square, so the artwork is centred at its own aspect
    # rather than stretched across it.
    $side = [Math]::Min($w, $h)
    $g.DrawImage($artwork, [int](($w - $side) / 2), [int](($h - $side) / 2), $side, $side)
    $g.Dispose()
    $bitmap.Save((Join-Path $layout "Assets\$name"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
}
$artwork.Dispose()
"layout built at $layout"

# ---- Package --------------------------------------------------------------

$msix = Join-Path $build "DiskSpace.msix"
if ($Pack -or $Sign) {
    if (Test-Path $msix) { Remove-Item -Force $msix }
    & $makeappx pack /d $layout /p $msix /o | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "makeappx failed" }
    "packed $msix ($([int]((Get-Item $msix).Length / 1KB)) kB)"
}

if ($Sign) {
    # A local test certificate, so the package can be installed the way a user
    # would install it. The Store signs real submissions; this never leaves the
    # machine it was made on.
    $cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $CertSubject } | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject `
            -CertStoreLocation Cert:\CurrentUser\My -KeyUsage DigitalSignature `
            -FriendlyName "Disk Space local test signing" -NotAfter (Get-Date).AddYears(5)
        "created test certificate $($cert.Thumbprint)"
    }
    & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $msix
    if ($LASTEXITCODE -ne 0) { throw "signtool failed" }
    "signed $msix"
}

# ---- Register -------------------------------------------------------------

if ($Register) {
    # Loose registration from the layout: no signature needed, which keeps the
    # question being tested (does this behave correctly as a package?) separate
    # from the question of certificates. Needs Developer Mode.
    Get-AppxPackage -Name "ChofterApps.DiskSpace" | Remove-AppxPackage -EA SilentlyContinue
    Add-AppxPackage -Register (Join-Path $layout "AppxManifest.xml")
    $package = Get-AppxPackage -Name "ChofterApps.DiskSpace"
    "registered $($package.PackageFullName)"
    "family    $($package.PackageFamilyName)"
}
