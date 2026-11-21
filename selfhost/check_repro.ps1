# selfhost/check_repro.ps1 - H9.7 reproducible-build gate.
#
# Copies the tree twice into directories with different depths, bootstraps the
# compiler in each copy with `build.ps1 -SelfLink` (no oracle, no CMake, no
# `build/`), and requires every produced `.obj`, `.ivm` and `.exe` to be
# byte-identical.  That proves two things at once:
#
#   * the object files carry no absolute source path (the two copies live at
#     different absolute paths), and
#   * every link is reproducible (`/Brepro`, no PE timestamp, no PDB path).
#
# Both copies get the same `-GitHash` because the copy has no `.git` of its
# own; that keeps the `--version` stamp out of the comparison.
#
# Everything happens in TEMP; the repository is only read.  Exit 0 iff the two
# trees agree byte for byte.
#
# Usage (from the repository root, with LLVM installed):
#   powershell -NoProfile -File selfhost/check_repro.ps1
#   powershell -NoProfile -File selfhost/check_repro.ps1 -TempRoot C:\scratch\h97 -Keep
#
# This needs two bootstraps (~1-3 min, no golden gate); the push CI still runs
# only the dogfood gate, and `selfhost-repro.yml` runs this one weekly or on
# demand.

param(
    [string]$TempRoot = "",
    [string]$LlvmRoot = "",
    [string]$GitHash = "",
    [switch]$Keep = $false
)

$ErrorActionPreference = "Stop"

function Fail([string]$msg) {
    Write-Output ("FAILED: " + $msg)
    exit 1
}

$repo = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path -LiteralPath (Join-Path $repo "selfhost\build.ps1"))) {
    Fail ("not a checkout: " + $repo)
}

# The stamp both trees get.  A working-tree copy has no `.git`, so resolve the
# hash here once and pass it down (an archive checkout without git still works:
# the stamp then stays `unknown` in both trees).
if ($GitHash -eq "") {
    $GitHash = "unknown"
    try {
        $h = & git -C $repo rev-parse --short=12 HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and $h) { $GitHash = ([string]$h).Trim() }
    } catch { }
}
if ($GitHash -notmatch '^[0-9a-fA-F]{7,40}$') { $GitHash = "unknown" }

$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$base = Join-Path $tempBase ("h97repro_" + $stamp + "_" + $PID)
New-Item -ItemType Directory -Force -Path $base | Out-Null
Write-Output ("run directory: " + $base)
Write-Output ("git hash:      " + $GitHash)

# Different depths (and different name lengths) so a leaked absolute path in
# a string table, a `.ivm`, or a PE debug entry cannot go unnoticed.
$copyA = Join-Path $base "a\repo"
$copyB = Join-Path $base "b\much\deeper\source\tree\repo"
New-Item -ItemType Directory -Force -Path $copyA, $copyB | Out-Null

# Copy the working tree (not HEAD): the gate is about the bytes the current
# sources build, and it can be run before committing.  Excluded:
#   * .git   - each copy must be a plain source tree;
#   * build  - the oracle tree / CMake output; its presence would defeat the
#              "no oracle" property and it is not an input of build.ps1.
foreach ($dst in @($copyA, $copyB)) {
    $rcArgs = @($repo, $dst, "/E", "/NFL", "/NDL", "/NJH", "/NJS", "/NP",
                "/XD", (Join-Path $repo ".git"), (Join-Path $repo "build"))
    & robocopy @rcArgs | Out-Null
    # robocopy: 0-7 are success codes (8+ are failures).
    if ($LASTEXITCODE -ge 8) { Fail ("copy failed (" + $LASTEXITCODE + "): " + $dst) }
}

function Invoke-Build([string]$tree) {
    $run = Join-Path (Split-Path $tree -Parent) "run"
    New-Item -ItemType Directory -Force -Path $run | Out-Null
    $log = Join-Path (Split-Path $tree -Parent) "build.log"
    $bargs = @("-NoProfile", "-File", (Join-Path $tree "selfhost\build.ps1"),
               "-TempRoot", $run, "-GitHash", $GitHash, "-SelfLink")
    if ($LlvmRoot -ne "") { $bargs += @("-LlvmRoot", $LlvmRoot) }
    & powershell @bargs *>&1 | Tee-Object -FilePath $log | Out-Host
    $code = $LASTEXITCODE
    if ($code -ne 0) { Fail ("bootstrap failed in " + $tree + " (exit " + $code + ", log " + $log + ")") }
    $runs = @(Get-ChildItem -LiteralPath $run -Directory | Sort-Object Name)
    if ($runs.Count -ne 1) { Fail ("expected one run directory under " + $run) }
    return $runs[0].FullName
}

function Get-Artifacts([string]$runDir) {
    $map = @{}
    $files = @(Get-ChildItem -LiteralPath $runDir -Recurse -File |
               Where-Object { $_.Extension -in @(".obj", ".ivm", ".exe") })
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($runDir.Length + 1)
        $map[$rel] = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
    }
    return $map
}

Write-Output "[1/3] bootstrapping tree A ..."
$runA = Invoke-Build $copyA
Write-Output "[2/3] bootstrapping tree B ..."
$runB = Invoke-Build $copyB

Write-Output "[3/3] comparing artifacts ..."
$mapA = Get-Artifacts $runA
$mapB = Get-Artifacts $runB
$diffs = New-Object System.Collections.Generic.List[string]

foreach ($rel in ($mapA.Keys | Sort-Object)) {
    if (-not $mapB.ContainsKey($rel)) {
        $diffs.Add("only in A: " + $rel)
    } elseif ($mapA[$rel] -ne $mapB[$rel]) {
        $diffs.Add("bytes differ: " + $rel)
    }
}
foreach ($rel in ($mapB.Keys | Sort-Object)) {
    if (-not $mapA.ContainsKey($rel)) { $diffs.Add("only in B: " + $rel) }
}

$obj = @($mapA.Keys | Where-Object { $_ -like "*.obj" }).Count
$ivm = @($mapA.Keys | Where-Object { $_ -like "*.ivm" }).Count
$exe = @($mapA.Keys | Where-Object { $_ -like "*.exe" }).Count
Write-Output ("      " + $mapA.Count + " artifacts: " + $obj + " object, " + $ivm +
              " .ivm, " + $exe + " executable")

foreach ($d in $diffs) { Write-Output ("      DIFF " + $d) }

$exeA = Join-Path $runA "stage1\ivy_selfhost.exe"
if (Test-Path -LiteralPath $exeA) {
    Write-Output ("      stage 1 sha256: " + (Get-FileHash -LiteralPath $exeA -Algorithm SHA256).Hash)
}

if ($diffs.Count -ne 0) {
    Fail ("" + $diffs.Count + " artifact(s) differ between the two trees")
}

Write-Output "--- REPRO PASS ---"
if (-not $Keep) {
    Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue
}
exit 0
