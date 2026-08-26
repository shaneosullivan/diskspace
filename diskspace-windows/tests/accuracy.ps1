# Accuracy test.
#
# Builds a tree whose exact on-disk cost is known in advance — including the
# cases that are easy to get wrong on Windows — scans it, and asserts the totals
# to the byte. This is the Windows counterpart of checking a scanner against du:
# rather than trusting another tool, the tree is constructed so the right answer
# is arithmetic.
#
#   .\tests\accuracy.ps1                 uses .\build\diskscan.exe
#   .\tests\accuracy.ps1 -Exe path\to\diskscan.exe

param(
    [string]$Exe = (Join-Path $PSScriptRoot "..\build\diskscan.exe"),
    [string]$Root = (Join-Path $env:TEMP "diskspace-accuracy"),
    [switch]$Keep
)

$ErrorActionPreference = "Stop"
$script:failures = 0

function Assert-Equal($label, $expected, $actual) {
    if ($expected -eq $actual) {
        "  PASS  {0,-34} {1}" -f $label, $actual
    } else {
        $script:failures++
        "  FAIL  {0,-34} expected {1}, got {2}" -f $label, $expected, $actual
    }
}

# Windows PowerShell's own cmdlets stop at 260 characters, which is exactly the
# limit the deep-path case exists to exercise, so the file APIs are called
# directly with the \\?\ prefix that turns path parsing off.
function Extended($path) {
    if ($path.StartsWith("\\?\")) { return $path }
    return "\\?\" + [System.IO.Path]::GetFullPath($path)
}

function New-File($path, $bytes) {
    $dir = Split-Path -Parent $path
    [System.IO.Directory]::CreateDirectory((Extended $dir)) | Out-Null
    $stream = [System.IO.File]::Create((Extended $path))
    try {
        $buffer = New-Object byte[] $bytes
        # Non-zero, so nothing can be silently stored as a hole.
        for ($i = 0; $i -lt $bytes; $i += 997) { $buffer[$i] = 0xA5 }
        $stream.Write($buffer, 0, $bytes)
    } finally { $stream.Dispose() }
}

# ---- Build the tree -------------------------------------------------------

function Remove-Tree($path) {
    if (-not (Test-Path $path)) { return }
    # robocopy mirrors an empty folder over the target, which is the one way to
    # delete a tree containing paths past 260 characters from Windows PowerShell.
    $empty = Join-Path $env:TEMP ("diskspace-empty-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $empty | Out-Null
    robocopy $empty $path /MIR /NJH /NJS /NP /NFL /NDL | Out-Null
    Remove-Item -Recurse -Force $empty
    Remove-Item -Recurse -Force $path
}

Remove-Tree $Root
New-Item -ItemType Directory -Force -Path $Root | Out-Null

$cluster = (Get-CimInstance Win32_Volume | Where-Object {
    $_.DriveLetter -eq (Split-Path -Qualifier $Root)
}).BlockSize
if (-not $cluster) { $cluster = 4096 }
function Clusters($bytes) { [int64][math]::Ceiling($bytes / $cluster) * $cluster }

# Every size is a whole number of clusters or clearly above the resident-file
# threshold, so allocation is predictable rather than dependent on how much slack
# a particular MFT record happens to have.
$plain = @{
    "a.bin"                     = 8192
    "sub\b.bin"                 = 5000
    "sub\deeper\c.bin"          = 4096
    "sub\deeper\uni_ünïcødé_文件.bin" = 12288
}
$expectedAllocated = 0L
$expectedLogical = 0L
$expectedFiles = 0
foreach ($name in $plain.Keys) {
    New-File (Join-Path $Root $name) $plain[$name]
    $expectedAllocated += Clusters $plain[$name]
    $expectedLogical += $plain[$name]
    $expectedFiles++
}

# A hard link is a second name for bytes already counted. It must add nothing to
# the total, and must be reported as a duplicate.
$linkTarget = Join-Path $Root "a.bin"
$linkPath = Join-Path $Root "sub\a-hardlink.bin"
cmd /c mklink /H "`"$linkPath`"" "`"$linkTarget`"" | Out-Null
# Still a row in the tree, so it is a node — but not a file, because its bytes
# belong to the name that reached them first.
$expectedDuplicates = 1

# A directory junction. Following it would count its target a second time and can
# send a walk round in a circle, so it must be recorded and left closed.
$junctionTarget = Join-Path $Root "sub"
$junctionPath = Join-Path $Root "junction-to-sub"
cmd /c mklink /J "`"$junctionPath`"" "`"$junctionTarget`"" | Out-Null

# A path past the old 260-character limit, which needs the \\?\ prefix to open.
$deep = $Root
foreach ($i in 1..12) { $deep = Join-Path $deep ("long-directory-name-segment-{0:D2}" -f $i) }
New-File (Join-Path $deep "deep.bin") 4096
$expectedAllocated += Clusters 4096
$expectedLogical += 4096
$expectedFiles++

# A sparse file: a megabyte of nothing, occupying no clusters at all. Trusting the
# reported length here would add a megabyte that is not on the disk.
$sparsePath = Join-Path $Root "sparse.bin"
New-File $sparsePath 4096
fsutil sparse setflag "$sparsePath" | Out-Null
$stream = [System.IO.File]::Open($sparsePath, "Open", "Write")
$stream.SetLength(1MB)
$stream.Dispose()
fsutil sparse setrange "$sparsePath" 0 1048576 | Out-Null
$expectedLogical += 1MB
$expectedFiles++
# Its allocation is whatever the file system decided to keep, which is checked as
# "much smaller than its length" rather than as an exact figure.

# Every entry the scan should produce: the root, four plain files, the hard link,
# the junction, sub, sub\deeper, twelve nested directories, the deep file and the
# sparse file. Asserting this catches both a missing entry and a junction that was
# wrongly descended into.
$expectedNodes = 1 + 4 + 1 + 1 + 2 + 12 + 1 + 1

"tree built at $Root (cluster size $cluster bytes)"
""

# ---- Scan and compare -----------------------------------------------------

function Scan([string[]]$extra) {
    $output = & $Exe scan $Root --raw @extra
    if ($LASTEXITCODE -ne 0) { throw "diskscan failed: $output" }
    $map = @{}
    foreach ($line in $output) {
        $parts = $line -split "=", 2
        if ($parts.Count -eq 2) { $map[$parts[0]] = $parts[1] }
    }
    return $map
}

$r = Scan @()
$sparseAllocated = [int64]$r["allocated"] - $expectedAllocated

"counting hard links once"
Assert-Equal "files with bytes of their own" $expectedFiles ([int]$r["files"])
Assert-Equal "entries in the tree"  $expectedNodes      ([int]$r["nodes"])
Assert-Equal "hard-link duplicates" $expectedDuplicates ([int]$r["hardlink_duplicates"])
Assert-Equal "logical bytes"        $expectedLogical    ([int64]$r["logical"])
Assert-Equal "unreadable entries"   0                   ([int]$r["errors"])
if ($sparseAllocated -ge 0 -and $sparseAllocated -lt 64KB) {
    "  PASS  {0,-34} {1} bytes for a 1 MB sparse file" -f "sparse file occupies almost nothing", $sparseAllocated
} else {
    $script:failures++
    "  FAIL  {0,-34} {1} bytes attributed to a 1 MB sparse file" -f "sparse file occupies almost nothing", $sparseAllocated
}

# The junction must not have been followed: doing so would count sub\ twice.
$junctionBytes = (Clusters $plain["sub\b.bin"]) + (Clusters $plain["sub\deeper\c.bin"]) +
                 (Clusters $plain["sub\deeper\uni_ünïcødé_文件.bin"])
if ([int64]$r["allocated"] -lt $expectedAllocated + $junctionBytes) {
    "  PASS  {0,-34} not followed" -f "directory junction"
} else {
    $script:failures++
    "  FAIL  {0,-34} appears to have been followed" -f "directory junction"
}

""
"counting every link (--no-hardlinks)"
$n = Scan @("--no-hardlinks")
$withLink = [int64]$r["allocated"] + (Clusters $plain["a.bin"])
Assert-Equal "allocated with the link counted" $withLink ([int64]$n["allocated"])

""
# ---- Which backend the automatic choice lands on ---------------------------
#
# Regression guard. The whole-volume test was once a comparison between "C:\" and
# "C:" that could never be equal, so the fast backend was never chosen
# automatically and elevating changed nothing. Nothing failed and nothing was
# reported - the scan was simply slower and saw less. Asserting the inputs to the
# decision catches that without needing administrator rights to observe it.
"backend selection"
# "C:" on its own means "the current directory on C:", which is exactly the
# trap this test exists for - so the separator is explicit.
$systemRoot = "$env:SystemDrive\"
function Check([string]$path) {
    $map = @{}
    foreach ($line in (& $Exe check $path)) {
        $parts = $line -split "=", 2
        if ($parts.Count -eq 2) { $map[$parts[0]] = $parts[1] }
    }
    return $map
}
$driveCheck = Check $systemRoot
Assert-Equal "$systemRoot is a whole volume" "1" $driveCheck["volume_root"]
$folderCheck = Check $Root
Assert-Equal "a folder on it is not"        "0" $folderCheck["volume_root"]
# On NTFS the only thing left between here and the fast backend is elevation, so
# that is what the reason has to say.
if ($driveCheck["ntfs"] -eq "1") {
    $expected = if ($driveCheck["elevated"] -eq "1") { "mft" } else { "enum" }
    Assert-Equal "automatic backend for the drive" $expected $driveCheck["auto_backend"]
    if ($driveCheck["elevated"] -ne "1") {
        Assert-Equal "and the reason given" "needs administrator rights" $driveCheck["auto_reason"]
    }
}

""
if ($script:failures -eq 0) {
    "all checks passed"
} else {
    "$($script:failures) check(s) FAILED"
}

if (-not $Keep) { Remove-Tree $Root }
exit $script:failures
