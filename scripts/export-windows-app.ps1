# =============================================================================
# export-windows-store.ps1
#
# Builds and packages a Microsoft Store submission of Disk Space.
#
# Usage:
#   .\scripts\export-windows-store.ps1
#   .\scripts\export-windows-store.ps1 -Sign      also sign it, to test locally
#
# The counterpart of export-macos-app.sh, and rather shorter than it, because
# the Store does the two expensive parts: it signs the package with a
# certificate every Windows machine already trusts, and it distributes and
# updates it. Nothing here needs a certificate, a notary account, or credentials
# of any kind.
#
# Prerequisites:
#   - The Visual Studio "Desktop development with C++" workload
#   - The Windows SDK (makeappx.exe, and signtool.exe only for -Sign)
#   - Identity values from Partner Center, once the app is reserved there
# =============================================================================

param(
    # Signs the package with a local test certificate so it can be installed the
    # way a user would, to check the built article before submitting it. A Store
    # submission must NOT be signed - Microsoft signs it, and an existing
    # signature is rejected.
    [switch]$Sign,
    # Skips the build and packages whatever is already in build\.
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

# The paths are needed by the configuration below, so they come first.
$repoRoot = Split-Path -Parent $PSScriptRoot
$windowsDir = Join-Path $repoRoot "diskspace-windows"
$buildDir = Join-Path $windowsDir "build"
$releaseDir = Join-Path $repoRoot "build\release"

# =============================================================================
# CONFIGURATION
# =============================================================================
#
# These three come from Partner Center: reserve the app name, then read them off
# the Product identity page. Until the app is reserved the defaults below are
# fine for producing and testing a package; they are simply not what the Store
# will accept at submission.
#
# Identity/Name and Publisher must match the reservation exactly or the upload is
# rejected, so they are overridable from the environment rather than edited here.

# Read from Packaging\store-identity.txt if it exists, which is the intended way
# to set them: written once and kept out of the repository, because it names a
# specific Partner Center account rather than anything about the program.
# Environment variables override the file, for a build machine that would rather
# not keep one.
$identityFile = Join-Path $repoRoot "diskspace-windows\Packaging\store-identity.txt"
$fromFile = @{}
if (Test-Path $identityFile) {
    foreach ($line in Get-Content $identityFile) {
        if ($line -match '^\s*#') { continue }
        $pair = $line -split "=", 2
        if ($pair.Count -eq 2) { $fromFile[$pair[0].Trim()] = $pair[1].Trim() }
    }
}
function IdentityValue([string]$key, [string]$environmentName, [string]$fallback) {
    $fromEnvironment = [Environment]::GetEnvironmentVariable($environmentName)
    if ($fromEnvironment) { return $fromEnvironment }
    if ($fromFile.ContainsKey($key)) { return $fromFile[$key] }
    return $fallback
}

$IdentityName = IdentityValue "IdentityName" "DISKSPACE_STORE_IDENTITY" "ChofterApps.DiskSpace"
$Publisher = IdentityValue "Publisher" "DISKSPACE_STORE_PUBLISHER" "CN=Chofter Apps"
$PublisherDisplayName = IdentityValue "PublisherDisplayName" "DISKSPACE_STORE_PUBLISHER_NAME" "Chofter Apps"

# The version lives in src\app\product.h and is bumped there, so the executable
# and the package that carries it can never disagree about what they are.


"=== Disk Space - Windows Export ==="
""

# =============================================================================
# VALIDATION
# =============================================================================

$versionHeaderPath = Join-Path $windowsDir "src\app\product.h"
if (-not (Test-Path $versionHeaderPath)) { throw "missing $versionHeaderPath" }
$versionHeader = Get-Content $versionHeaderPath -Raw
function VersionPart([string]$name) {
    if ($versionHeader -match "#define\s+DISKSPACE_VERSION_$name\s+(\d+)") { return $Matches[1] }
    throw "could not read DISKSPACE_VERSION_$name from product.h"
}
$version = "{0}.{1}.{2}.{3}" -f (VersionPart "MAJOR"), (VersionPart "MINOR"),
                                (VersionPart "PATCH"), (VersionPart "REVISION")

if ((VersionPart "REVISION") -ne "0") {
    throw "the Store reserves the fourth version component and requires it to be 0"
}

$kits = "C:\Program Files (x86)\Windows Kits\10\bin"
if (-not (Test-Path $kits)) { throw "the Windows SDK was not found at $kits" }
"OK  version $version"
"OK  Windows SDK present"

# Each value is checked rather than only the first: a package carrying a
# placeholder in any one of them is rejected on upload, and finding that out from
# Partner Center is a slower way to learn it than finding out here.
$unset = @()
if ($IdentityName -eq "ChofterApps.DiskSpace" -or $IdentityName -match "REPLACE|PLACEHOLDER") {
    $unset += "IdentityName         (Package/Identity/Name)"
}
if ($Publisher -eq "CN=Chofter Apps" -or $Publisher -match "REPLACE|PLACEHOLDER") {
    $unset += "Publisher            (Package/Identity/Publisher)"
}
if ($PublisherDisplayName -match "REPLACE|PLACEHOLDER") {
    $unset += "PublisherDisplayName (Package/Properties/PublisherDisplayName)"
}
if ($unset.Count -gt 0) {
    ""
    "NOTE: these are not set to real Store values yet, so the package this"
    "      produces will be rejected on upload:"
    ""
    foreach ($item in $unset) { "        $item" }
    ""
    "      They already exist - reserving the app name is what creates them."
    "      Partner Center > your app > Product management > Product identity,"
    "      then write them into:"
    ""
    "        $identityFile"
    ""
    "      Each must match character for character. Note that Publisher is"
    "      usually a GUID, e.g. CN=A1B2C3D4-5E6F-7890-ABCD-EF1234567890, not a"
    "      readable company name. The file is not committed."
}

# =============================================================================
# BUILD
# =============================================================================

if (-not $NoBuild) {
    ""
    "=== Building (release) ==="
    Get-Process DiskSpace -ErrorAction SilentlyContinue | Stop-Process -Force
    # Full rebuild: the version resource is compiled in, and a stale object file
    # would ship a package whose executable reports the previous version.
    Remove-Item -Recurse -Force (Join-Path $buildDir "app"),
                                (Join-Path $buildDir "cli") -ErrorAction SilentlyContinue
    # ErrorActionPreference is relaxed around this one call. vcvars64.bat writes
    # chatter to stderr on some installs, and PowerShell turns any stderr from a
    # native command into a terminating error under "Stop" - which would fail a
    # build that in fact succeeded. The exit code is the thing that means
    # anything, so that is what is checked.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & (Join-Path $windowsDir "build.cmd") 2>&1 | ForEach-Object { "  $_" }
    $buildExit = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($buildExit -ne 0) { throw "build failed with exit code $buildExit" }
    "OK  built"
}

$exe = Join-Path $buildDir "DiskSpace.exe"
if (-not (Test-Path $exe)) { throw "$exe not found" }

# The executable's own resource is the thing the update check reports, so it is
# checked against the manifest rather than assumed to agree with it.
$exeVersion = (Get-Item $exe).VersionInfo.FileVersion -replace ",\s*", "."
if ($exeVersion.Trim() -ne $version) {
    throw "the executable reports version '$exeVersion' but the package would say '$version'"
}
"OK  executable reports $exeVersion"

# =============================================================================
# TESTS
# =============================================================================
#
# Cheap, and the figures are the entire product. Shipping a package that reports
# the wrong size would be worse than shipping nothing.

""
"=== Running the accuracy tests ==="
& (Join-Path $windowsDir "tests\accuracy.ps1") | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw "accuracy tests failed - not packaging" }

# =============================================================================
# PACKAGE
# =============================================================================

""
"=== Packaging ==="
$packArgs = @{
    Pack                 = $true
    IdentityName         = $IdentityName
    Publisher            = $Publisher
    PublisherDisplayName = $PublisherDisplayName
}
if ($Sign) { $packArgs.Sign = $true }
& (Join-Path $windowsDir "Packaging\make-msix.ps1") @packArgs

$msix = Join-Path $buildDir "DiskSpace.msix"
if (-not (Test-Path $msix)) { throw "makeappx produced no package" }

New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
$output = Join-Path $releaseDir "DiskSpace-$version.msix"
Copy-Item $msix $output -Force
$sizeKb = [int]((Get-Item $output).Length / 1KB)

# The portable executable is the other half of the release: the direct download
# from the site, for anyone who would rather not use the Store. It is the same
# binary that went into the package, copied out under a versioned name.
$portable = Join-Path $releaseDir "DiskSpace-$version.exe"
Copy-Item $exe $portable -Force
$portableKb = [int]((Get-Item $portable).Length / 1KB)

# =============================================================================
# CHECKSUMS
# =============================================================================
#
# Published beside the downloads. They are not a substitute for a signature -
# anyone able to replace the download could replace the checksums with it - but
# they let a careful person confirm that what arrived is what left, and they cost
# nothing. Written in the `sha256sum -c` format, which is also what most people
# already know how to read.

$checksums = Join-Path $releaseDir "SHA256SUMS.txt"
$lines = foreach ($file in @($portable, $output)) {
    $hash = (Get-FileHash $file -Algorithm SHA256).Hash.ToLower()
    "{0}  {1}" -f $hash, (Split-Path -Leaf $file)
}
# ASCII with no byte order mark: this file is meant to be piped into other tools.
[IO.File]::WriteAllLines($checksums, $lines, (New-Object Text.UTF8Encoding $false))

# =============================================================================
# DONE
# =============================================================================

""
"============================================"
"  Export complete!"
"  Version:   $version"
"  Identity:  $IdentityName"
"  Store:     $output ($sizeKb kB)"
"  Portable:  $portable ($portableKb kB)"
"  Checksums: $checksums"
"============================================"
""
Get-Content $checksums | ForEach-Object { "  $_" }
""
"Publish the checksums beside the download, so anyone who wants to can verify"
"what they got with:"
"  Get-FileHash .\DiskSpace-$version.exe -Algorithm SHA256"
""
if ($Sign) {
    "Signed with a local test certificate - for installing and checking only."
    "Do NOT submit this file: the Store rejects an already-signed package."
    "Re-run without -Sign to produce the one to upload."
} else {
    "Next: Partner Center > your app > Packages > upload this .msix."
    "Microsoft signs it on acceptance, so it needs no certificate from us and"
    "installs with no SmartScreen warning and no Smart App Control block."
}
""
"Also remember to add the release to site/lib/releases.ts, or the in-app update"
"check will go on reporting that this version is the latest one."
