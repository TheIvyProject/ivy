# selfhost/dogfood.ps1 - H8.2: CI dogfood gate.
#
# The compiler that Ivy built itself (`ivy_selfhost.exe`, stage 1) must behave
# exactly like the C++-built oracle on every corpus fixture.  The gate runs two
# harnesses:
#
#   1. `selfhost/app/bootstrap_smoke.ps1` compiles the selfhost sources with the
#      oracle (stage 0) into stage 1 and proves F2 (stage 1 and stage 2 produce
#      byte-identical objects, `.ivm` files and executable);
#   2. `selfhost/app/driver_smoke.ps1` drives that stage 1 binary over the whole
#      corpus: `--llvm` and `-c` on every fixture (1052 today) and link+run on
#      the codegen corpus.
#
# Only the modes the selfhost driver has ported are compared; `-Dumps` adds the
# `--lex`, `--tokens`, `--ast`, `--hir`, `--mir` and `--run` modes (H9.2), which
# run the real driver against the oracle through `difftest.ps1` and
# `interpdiff.ps1` (`-DriverExe`).
#
# Nothing is written into the repository: every object, `.ivm`, executable and
# scratch file lives in a per-run directory under -TempRoot (or the platform
# temp dir).
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/dogfood.ps1
#   powershell -NoProfile -File selfhost/dogfood.ps1 -TempRoot C:\scratch\h82
#   powershell -NoProfile -File selfhost/dogfood.ps1 -SkipBootstrap -Stage1 <path>\ivy_selfhost.exe
#
# -SkipBootstrap reuses an already built stage 1 binary (the bootstrap is by far
# the slowest step while iterating on a mismatch).
#
# Exit 0 iff F2 holds and the Ivy-built compiler matches the oracle.

param(
    [string]$TempRoot = "",
    [string]$LlvmRoot = "",
    [string]$Stage1 = "",
    [switch]$SkipBootstrap,
    # H9.2: also check the dump and interpreter modes against the oracle.
    [switch]$Dumps,
    [int]$TimeoutSec = 60
)

$ErrorActionPreference = "Stop"

function Fail([string]$msg) {
    Write-Output ("FAILED: " + $msg)
    exit 1
}

$repo = Split-Path $PSScriptRoot -Parent
$appDir = Join-Path $PSScriptRoot "app"
$oracle = Join-Path $repo "build\Release\ivy.exe"
if (-not (Test-Path -LiteralPath $oracle)) { Fail ("oracle not found: " + $oracle) }

$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$runDir = Join-Path $tempBase ("h82_dogfood_" + $stamp + "_" + $PID)
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$env:TEMP = $runDir
$env:TMP = $runDir
Write-Output ("run directory: " + $runDir)

# Both child harnesses end with `exit`, so they run as separate PowerShell
# processes; `& powershell` surfaces their status in $LASTEXITCODE.
$bootstrap = Join-Path $appDir "bootstrap_smoke.ps1"
$driverSmoke = Join-Path $appDir "driver_smoke.ps1"
if (-not (Test-Path -LiteralPath $bootstrap)) { Fail ("missing " + $bootstrap) }
if (-not (Test-Path -LiteralPath $driverSmoke)) { Fail ("missing " + $driverSmoke) }
$total = 3

# --- 1. stage 0 builds stage 1 and F2 holds -----------------------------------
if ($SkipBootstrap) {
    if ($Stage1 -eq "") { Fail "-SkipBootstrap needs -Stage1 <ivy_selfhost.exe>" }
    if (-not (Test-Path -LiteralPath $Stage1)) { Fail ("stage 1 not found: " + $Stage1) }
    $stage1 = (Resolve-Path -LiteralPath $Stage1).Path
    Write-Output "[1/$total] bootstrap: skipped (-SkipBootstrap)"
} else {
    Write-Output "[1/$total] bootstrap: the oracle builds stage 1, stage 1 rebuilds itself (F2) ..."
    $stage1File = Join-Path $runDir "stage1.txt"
    $bsArgs = @("-NoProfile", "-File", $bootstrap, "-TempRoot", $runDir,
                "-Stage1File", $stage1File)
    if ($LlvmRoot -ne "") { $bsArgs += @("-LlvmRoot", $LlvmRoot) }
    & powershell @bsArgs
    if ($LASTEXITCODE -ne 0) { Fail ("bootstrap/F2 failed (exit " + $LASTEXITCODE + ")") }
    if (-not (Test-Path -LiteralPath $stage1File)) { Fail "bootstrap did not report its stage 1 binary" }
    $stage1 = (Get-Content -LiteralPath $stage1File -Raw).Trim()
    if (-not (Test-Path -LiteralPath $stage1)) { Fail ("stage 1 not found: " + $stage1) }
}

# --- 2. the Ivy-built compiler over the whole corpus --------------------------
Write-Output "[2/$total] the Ivy-built compiler vs the oracle over the corpus ..."
$dsArgs = @("-NoProfile", "-File", $driverSmoke, "-Driver", $stage1,
            "-TempRoot", $runDir, "-SkipOwn", "-FullCorpus",
            "-TimeoutSec", "$TimeoutSec")
& powershell @dsArgs
if ($LASTEXITCODE -ne 0) { Fail ("dogfood parity broken (exit " + $LASTEXITCODE + ")") }

# --- 3. the dump and interpreter modes (H9.2) --------------------------------
if (-not $Dumps) {
    Write-Output "[3/$total] dump/interpreter modes: skipped (pass -Dumps)"
} else {
    Write-Output "[3/$total] the dump and interpreter modes vs the oracle ..."
    $difftest = Join-Path $PSScriptRoot "difftest.ps1"
    $interpdiff = Join-Path $PSScriptRoot "interpdiff.ps1"
    # Corpus root, oracle flag, extra -ModuleDirs (the module corpus imports
    # its siblings).
    $dumpCases = @(
        @("selfhost/corpus/lexer",  "--lex",    ""),
        @("selfhost/corpus/pp",     "--tokens", ""),
        @("selfhost/corpus/parser", "--ast",    "selfhost/parsing,selfhost/corpus/parser/module"),
        @("selfhost/corpus/hir",    "--hir",    ""),
        @("selfhost/corpus/mir",    "--mir",    "")
    )
    foreach ($c in $dumpCases) {
        $dtArgs = @("-NoProfile", "-File", $difftest, "-Dir", $c[0], "-Kinds", "all",
                    "-Oracle", $c[1], "-Recurse", "-DriverExe", $stage1,
                    "-TempRoot", $runDir)
        if ($c[2] -ne "") { $dtArgs += @("-ModuleDirs", $c[2]) }
        & powershell @dtArgs
        if ($LASTEXITCODE -ne 0) { Fail ("dump parity broken (" + $c[1] + ", exit " + $LASTEXITCODE + ")") }
    }
    $ipArgs = @("-NoProfile", "-File", $interpdiff, "-TempRoot", $runDir,
                "-DriverExe", $stage1)
    & powershell @ipArgs
    if ($LASTEXITCODE -ne 0) { Fail ("interpreter parity broken (exit " + $LASTEXITCODE + ")") }
}

Write-Output "--- DOGFOOD PASS ---"
exit 0
