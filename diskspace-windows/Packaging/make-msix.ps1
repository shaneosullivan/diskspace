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
    [switch]$Pack,
    [switch]$Sign,
    [switch]$Register,
    # Partner Center assigns these for a Store submission; they default to what
    # local testing needs and are overridden by the export script.
    [string]$IdentityName = "ChofterApps.DiskSpace",
    [string]$Publisher = "CN=Chofter Apps",
    [string]$PublisherDisplayName = "Chofter Apps",
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

# The manifest is stamped rather than copied: the identity a Store submission
# needs comes from Partner Center, and the version has to match the executable
# inside the package. Both are read from one place - src/app/product.h - so a
# package can never claim a version its own binary does not report.
$versionHeader = Get-Content (Join-Path $root "src/app/product.h") -Raw
function VersionPart([string]$name) {
    if ($versionHeader -match "#define\s+DISKSPACE_VERSION_$name\s+(\d+)") { return $Matches[1] }
    throw "could not read DISKSPACE_VERSION_$name from product.h"
}
$version = "{0}.{1}.{2}.{3}" -f (VersionPart "MAJOR"), (VersionPart "MINOR"),
                                (VersionPart "PATCH"), (VersionPart "REVISION")

# The displayed name comes from the same header as the version, so the Store
# listing, the window title and the executable's version resource all agree.
if ($versionHeader -match '#define\s+DISKSPACE_APP_NAME\s+"([^"]+)"') {
    $appName = $Matches[1]
} else {
    throw "could not read DISKSPACE_APP_NAME from product.h"
}

[xml]$manifest = Get-Content (Join-Path $PSScriptRoot "AppxManifest.xml")
$manifest.Package.Identity.Name = $IdentityName
$manifest.Package.Identity.Publisher = $Publisher
$manifest.Package.Identity.Version = $version
$manifest.Package.Properties.DisplayName = $appName
$manifest.Package.Properties.PublisherDisplayName = $PublisherDisplayName
$visual = $manifest.Package.Applications.Application.VisualElements
$visual.DisplayName = $appName
$manifest.Save((Join-Path $layout "AppxManifest.xml"))
"name       $appName"
"identity   $IdentityName $version"
"publisher  $Publisher"

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
"layout     $layout"

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
