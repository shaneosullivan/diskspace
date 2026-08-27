# =============================================================================
# run-linux-app.ps1
#
# Runs the built Linux application on this Windows machine, through WSL.
#
# Usage:
#   .\scripts\run-linux-app.ps1              launch it
#   .\scripts\run-linux-app.ps1 -Raise       bring the window to the front
#   .\scripts\run-linux-app.ps1 -Stop        close it
#   .\scripts\run-linux-app.ps1 -Cli volumes run diskscan instead
#
# A script rather than a documented command line, because the command line is
# genuinely awkward: it needs four environment variables, a detached launch, and
# quoting that survives both PowerShell and bash. Getting any of it wrong fails
# in ways that look like the application is broken when it is not - most notably
# a window that opens grey and empty, which is what happens on Wayland.
#
# See docs/running-linux-build-on-windows.md for what each part is for.
# =============================================================================

param(
    [switch]$Raise,
    [switch]$Stop,
    [string[]]$Cli,
    [string]$Distro = "Ubuntu-24.04",
    [string]$Path
)

$ErrorActionPreference = "Continue"
$env:WSL_UTF8 = 1

# -----------------------------------------------------------------------------
# Locating the build
# -----------------------------------------------------------------------------

$root = Split-Path -Parent $PSScriptRoot
$releaseDir = Join-Path $root "build\linux-release"

if (-not (Test-Path $releaseDir)) {
    Write-Host "No release build found at $releaseDir" -ForegroundColor Yellow
    Write-Host "Build one first, from inside WSL:"
    Write-Host "  wsl -d $Distro -- bash -c 'cd /mnt/c/... && ./scripts/export-linux-app.sh'"
    exit 1
}

# The same tree seen from inside WSL. Only the drive letter and the separators
# differ, so this is a rewrite rather than a lookup.
function ToWslPath([string]$windowsPath) {
    $full = (Resolve-Path $windowsPath).Path
    $drive = $full.Substring(0, 1).ToLower()
    return "/mnt/$drive" + $full.Substring(2).Replace("\", "/")
}

$appImage = Get-ChildItem -Path $releaseDir -Filter "*.AppImage" | Select-Object -First 1
$appDirBin = Join-Path $releaseDir "com.chofter.DiskSpace.AppDir\usr\bin"

# -----------------------------------------------------------------------------
# The simple verbs
# -----------------------------------------------------------------------------

# How to find the application, whichever way it was started. Worth stating once:
# "DiskSpace" alone matches none of them once an AppImage has mounted, because
# the process is then .../usr/bin/diskspace - lowercase, under a mount directory
# whose name is truncated to .mount_DiskSp plus random characters. A pattern that
# matches nothing fails silently, which is how this went wrong the first time.
#
# None of these match this script or the temporary ones it writes, which matters:
# their paths contain "diskspace" too, and a loose pattern kills the killer.
$processPatterns = @(
    "bin/diskspace",                  # mounted from an AppImage, or installed
    "build/diskspace/diskspace",      # run straight out of a build tree
    "DiskSpace-.*\.AppImage"         # the AppImage wrapper itself
)

function Get-RunningApp {
    foreach ($pattern in $processPatterns) {
        $found = wsl -d $Distro -- pgrep -f $pattern 2>$null
        if ($LASTEXITCODE -eq 0 -and $found) { return $found }
    }
    return $null
}

function Stop-App {
    foreach ($pattern in $processPatterns) {
        wsl -d $Distro -- pkill -f $pattern 2>$null | Out-Null
    }
}

if ($Stop) {
    Stop-App
    Start-Sleep -Seconds 1
    if (Get-RunningApp) { "Still running - something is holding it open." } else { "Stopped." }
    exit 0
}

if ($Raise) {
    # Written to a file and run from there: a pipe inside a quoted argument has
    # to survive PowerShell's parser, wsl.exe's argument handling and bash's,
    # and it does not reliably survive all three.
    $script = @'
export DISPLAY=:0
id=$(xdotool search --name "^Disk Space$" 2>/dev/null | tail -1)
if [ -z "$id" ]; then echo "not running"; exit 1; fi
xdotool windowactivate "$id" 2>/dev/null
xdotool windowraise "$id" 2>/dev/null
echo "raised window $id"
'@
    $temp = Join-Path $env:TEMP "diskspace-raise.sh"
    [IO.File]::WriteAllText($temp, $script.Replace("`r`n", "`n"))
    wsl -d $Distro -- bash (ToWslPath $temp)
    exit 0
}

if ($Cli) {
    if (-not (Test-Path $appDirBin)) {
        "No AppDir at $appDirBin - re-run the export script."
        exit 1
    }
    $binary = (ToWslPath $appDirBin) + "/diskscan"
    wsl -d $Distro -- $binary @Cli
    exit $LASTEXITCODE
}

# -----------------------------------------------------------------------------
# Launching
# -----------------------------------------------------------------------------

if (-not $appImage) {
    "No .AppImage in $releaseDir - re-run the export script."
    exit 1
}

$target = ToWslPath $appImage.FullName

# Every variable here earns its place; the table in the documentation says how.
# The short version: Wayland under WSLg opens a window that never paints, and
# there is no usable GPU, so GTK has to be told to draw on the CPU.
$launch = @"
export GDK_BACKEND=x11
export DISPLAY=:0
export GSK_RENDERER=cairo
export LIBGL_ALWAYS_SOFTWARE=1
unset WAYLAND_DISPLAY
cd "`$HOME"
setsid nohup "$target" $Path >/tmp/diskspace.log 2>&1 </dev/null &
disown
sleep 6
if pgrep -f "bin/diskspace" >/dev/null; then
  echo "running"
else
  echo "failed to start:"
  cat /tmp/diskspace.log
  exit 1
fi
"@

$temp = Join-Path $env:TEMP "diskspace-launch.sh"
[IO.File]::WriteAllText($temp, $launch.Replace("`r`n", "`n"))

# Already up? GApplication is single-instance, so a second launch would hand its
# arguments to the first and exit - which looks exactly like a failure to start.
if (Get-RunningApp) {
    "Already running. Bringing it to the front instead."
    & $PSCommandPath -Raise -Distro $Distro
    exit 0
}

"Launching $($appImage.Name) under WSL ($Distro)..."
wsl -d $Distro -- bash (ToWslPath $temp)

if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

""
"The window is titled 'Disk Space (Ubuntu-24.04)' on the Windows desktop."
"WSLg adds '[WARN: COPY MODE]' to the title; that is its own note about the"
"transport it uses, and says nothing about the application."
""
"  .\scripts\run-linux-app.ps1 -Raise    if it will not come to the front"
"  .\scripts\run-linux-app.ps1 -Stop     to close it"
""
"Scan / rather than /mnt/c for a fair impression of the speed: /mnt/c reaches"
"the Windows drive over 9p, which is slower and misreports sparse files."
