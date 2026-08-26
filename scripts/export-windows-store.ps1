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

$IdentityName = if ($env:DISKSPACE_STORE_IDENTITY) { $env:DISKSPACE_STORE_IDENTITY }
                else { "ChofterApps.DiskSpace" }
$Publisher = if ($env:DISKSPACE_STORE_PUBLISHER) { $env:DISKSPACE_STORE_PUBLISHER }
             else { "CN=Chofter Apps" }
$PublisherDisplayName = if ($env:DISKSPACE_STORE_PUBLISHER_NAME) { $env:DISKSPACE_STORE_PUBLISHER_NAME }
                        else { "Chofter Apps" }

# The version lives in src\app\version.h and is bumped there, so the executable
# and the package that carries it can never disagree about what they are.

# =============================================================================
# PATHS
# =============================================================================

$repoRoot = Split-Path -Parent $PSScriptRoot
$windowsDir = Join-Path $repoRoot "diskspace-windows"
$buildDir = Join-Path $windowsDir "build"
$releaseDir = Join-Path $repoRoot "build\release"

"=== Disk Space - Windows Store Export ==="
""

# =============================================================================
# VALIDATION
# =============================================================================

$versionHeaderPath = Join-Path $windowsDir "src\app\version.h"
if (-not (Test-Path $versionHeaderPath)) { throw "missing $versionHeaderPath" }
$versionHeader = Get-Content $versionHeaderPath -Raw
function VersionPart([string]$name) {
    if ($versionHeader -match "#define\s+DISKSPACE_VERSION_$name\s+(\d+)") { return $Matches[1] }
    throw "could not read DISKSPACE_VERSION_$name from version.h"
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

if ($IdentityName -eq "ChofterApps.DiskSpace" -and -not $env:DISKSPACE_STORE_IDENTITY) {
    ""
    "NOTE: using the local testing identity, not a reserved Store one."
    "      Before submitting, reserve the name in Partner Center and set:"
    "        DISKSPACE_STORE_IDENTITY        e.g. 12345ChofterApps.DiskSpace"
    "        DISKSPACE_STORE_PUBLISHER       e.g. CN=ABCD1234-....."
    "        DISKSPACE_STORE_PUBLISHER_NAME  e.g. Chofter Apps"
    "      They are on the Product identity page, and must match exactly."
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

# =============================================================================
# DONE
# =============================================================================

""
"============================================"
"  Package complete!"
"  Version:  $version"
"  Identity: $IdentityName"
"  Package:  $output ($sizeKb kB)"
"============================================"
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
