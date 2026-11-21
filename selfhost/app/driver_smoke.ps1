# selfhost/app/driver_smoke.ps1 - H8.1: the self-host driver vs the oracle.
#
# `bootstrap_smoke.ps1` proves F2 (stage 1 -> stage 2 byte-identical).  This
# harness proves that the unified driver (`selfhost/app/main.ivy`) is a drop-in
# replacement for the oracle on the three modes it ports:
#
#   * `--llvm`  - IR text on stdout: stdout bytes, stderr bytes, exit status;
#   * `-c`      - native object: object bytes, stderr bytes, exit status;
#   * link mode - executable: link stderr, exit status, and the behaviour of the
#                 produced executable (exit status, stdout, stderr).
#
# The first section rebuilds every selfhost module with the driver and compares
# the objects against the ones the oracle produced, so the driver is checked on
# its own sources as well as on the corpus.
#
# The driver must be an already built `ivy_selfhost.exe` (see
# `bootstrap_smoke.ps1`, which produces one).  `-Driver` is mandatory.
#
# Fixtures whose oracle executable is not reproducible (it reads uninitialised
# storage or prints unspecified values) are compared on the link step only.
#
# Nothing is written into the repository: everything goes to a per-run directory
# under -TempRoot (or the platform temp dir), which is also TEMP/TMP.
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/app/driver_smoke.ps1 -Driver <path>\ivy_selfhost.exe
#   powershell -NoProfile -File selfhost/app/driver_smoke.ps1 -Driver ... -TempRoot C:\scratch\h81
#   powershell -NoProfile -File selfhost/app/driver_smoke.ps1 -Driver ... -RefStage <dir>
#   powershell -NoProfile -File selfhost/app/driver_smoke.ps1 -Driver ... -FullCorpus -SkipOwn
#
# -RefStage is a directory holding the oracle-built objects (`io.obj`, ...); when
# given, the driver's own-module objects are compared against them.
# -Corpus/-LinkCorpus add fixture directories to the `--llvm` + `-c` sections and
# to the link+run section; -FullCorpus is shorthand for every fixture directory
# under selfhost\corpus (the dogfood gate).  -SkipOwn drops section 1.
#
# Exit 0 iff every check matches.

param(
    [Parameter(Mandatory=$true)][string]$Driver,
    [string]$RefStage = "",
    [string]$TempRoot = "",
    [int]$TimeoutSec = 60,
    [string]$Filter = "",
    # H8.2: extra corpus directories checked with `--llvm` and `-c` (the
    # codegen corpus is always included).  Relative paths are resolved against
    # the repository root.
    [string[]]$Corpus = @(),
    # H8.2: extra corpus directories that are also linked and executed (the
    # codegen corpus is always included).
    [string[]]$LinkCorpus = @(),
    # H8.2: check every fixture directory under selfhost\corpus with `--llvm`
    # and `-c` (the dogfood gate).  The codegen corpus is already included and
    # link+run stays limited to the codegen corpus.
    [switch]$FullCorpus,
    # H8.2: skip section 1 (the driver rebuilding its own modules); the dogfood
    # gate already covers it through the F2 bootstrap.
    [switch]$SkipOwn
)

$ErrorActionPreference = "Stop"

function Fail([string]$msg) {
    Write-Output ("FAILED: " + $msg)
    exit 1
}

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$oracle = Join-Path $repo "build\Release\ivy.exe"
if (-not (Test-Path -LiteralPath $oracle)) { Fail ("oracle not found: " + $oracle) }
if (-not (Test-Path -LiteralPath $Driver)) { Fail ("driver not found: " + $Driver) }
$Driver = (Resolve-Path -LiteralPath $Driver).Path

$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$runDir = Join-Path $tempBase ("h81_driver_" + $stamp + "_" + $PID)
$modsDir = Join-Path $runDir "mods"
$oracleExeDir = Join-Path $runDir "oracle"
$selfExeDir = Join-Path $runDir "self"
$runCwd = Join-Path $runDir "cwd"
New-Item -ItemType Directory -Force -Path $modsDir, $oracleExeDir, $selfExeDir, $runCwd | Out-Null

$env:TEMP = $runDir
$env:TMP = $runDir
# Forward slashes: the form the oracle bakes in at build time.  H9.1: the
# runtime is the object built from std/rt.ivy (CMake's `ivyrt` target writes
# build/rt.obj).
$ivyrt = Join-Path $repo "build\rt.obj"
if (-not (Test-Path -LiteralPath $ivyrt)) {
    Write-Output ("FAIL: runtime object not found: " + $ivyrt)
    exit 1
}
$env:IVYRT_LIB_PATH = ($ivyrt -replace '\\', '/')

# The oracle binary has the pre-H9.1 archive path baked in (CMake baked
# `$<TARGET_FILE:ivyrt>`, i.e. `build/Release/ivyrt.lib`); the selfhost driver
# quotes the H9.1 runtime object it reads from `IVYRT_LIB_PATH`.  The linker
# command line is printed verbatim in link-failure diagnostics, so normalize
# both spellings to `<RT>` before comparing (the exe paths get `<OUT>`).
$rtSpellings = @(($repo.Replace('\', '/') + "/build/Release/ivyrt.lib"),
                 ($ivyrt -replace '\\', '/'), $ivyrt)
function Normalize-Rt([string]$text) {
    foreach ($p in $rtSpellings) { $text = $text.Replace($p, "<RT>") }
    # H9.7: the port's links carry `-Wl,/Brepro`; the oracle's do not.  Drop it
    # from both sides in failure diagnostics (successful links print nothing).
    return $text.Replace(" -Wl,/Brepro", "")
}

function Quote-Arg([string]$s) {
    if ($s -match '[\s"]') { return '"' + ($s -replace '"', '\"') + '"' }
    return $s
}

# Run one process with separate stdout/stderr files; returns its exit status.
# stdin is closed immediately so a fixture that reads input cannot block.
# `$WorkingDir` defaults to the repository root (the compilers need it for the
# auto `lib/` include path); the linked fixtures run in a scratch directory so a
# fixture that writes files (`188_io_write_file.ivy` writes `a.txt`/`b.txt`)
# cannot pollute the checkout.
function Invoke-Separate([string]$exe, [string[]]$argList, [string]$outPath,
                         [string]$errPath, [string]$WorkingDir = "") {
    if ($WorkingDir -eq "") { $WorkingDir = $repo }
    Remove-Item -LiteralPath $outPath, $errPath -ErrorAction SilentlyContinue
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = $WorkingDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $true
    $psi.CreateNoWindow = $true
    $psi.Arguments = (@($argList | ForEach-Object { Quote-Arg $_ }) -join " ")

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    if (-not $p.Start()) { throw ("cannot start " + $exe) }
    $p.StandardInput.Close()

    $fsOut = [System.IO.File]::Create($outPath)
    $fsErr = [System.IO.File]::Create($errPath)
    $tOut = $p.StandardOutput.BaseStream.CopyToAsync($fsOut)
    $tErr = $p.StandardError.BaseStream.CopyToAsync($fsErr)
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        $p.WaitForExit()
    }
    $tOut.Wait()
    $tErr.Wait()
    $fsOut.Dispose()
    $fsErr.Dispose()
    $code = $p.ExitCode
    $p.Dispose()
    return $code
}

function Test-BytesEqual([string]$a, [string]$b) {
    if (-not (Test-Path -LiteralPath $a) -or -not (Test-Path -LiteralPath $b)) { return $false }
    $ba = [System.IO.File]::ReadAllBytes($a)
    $bb = [System.IO.File]::ReadAllBytes($b)
    if ($ba.Length -ne $bb.Length) { return $false }
    for ($i = 0; $i -lt $ba.Length; $i++) {
        if ($ba[$i] -ne $bb[$i]) { return $false }
    }
    return $true
}

function Read-Text([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    return [System.IO.File]::ReadAllText($path)
}

# --- the compiler's own sources ------------------------------------------------
$moduleDirs = @("selfhost\common", "selfhost\parsing", "selfhost\hir", "selfhost\mir",
                "selfhost\codegen", "selfhost\app", "std")
$modules = @(
    @("selfhost\app", "buildinfo"),
    @("std", "io"),
    @("selfhost\common", "diagnostic"),
    @("selfhost\parsing", "token"),
    @("selfhost\parsing", "lexer"),
    @("selfhost\parsing", "preprocessor"),
    @("selfhost\parsing", "ast"),
    @("selfhost\parsing", "parser"),
    @("selfhost\parsing", "module_io"),
    @("selfhost\app", "cli"),
    @("selfhost\app", "imports"),
    @("selfhost\hir", "hir"),
    @("selfhost\hir", "hir_builder"),
    @("selfhost\mir", "mir"),
    @("selfhost\mir", "mir_builder"),
    @("selfhost\mir", "interpreter"),
    @("selfhost\codegen", "emitter"),
    @("selfhost\codegen", "codegen"),
    @("selfhost\app", "pipeline")
)

# --- 1. the driver compiles the compiler's own sources -------------------------
if ($SkipOwn) {
    Write-Output "[1/4] the driver's own modules: skipped (-SkipOwn)"
} else {
    Write-Output "[1/4] the driver compiles the selfhost sources ..."
    $inc = @("-I", $modsDir)
    foreach ($md in $moduleDirs) { $inc += @("-I", (Join-Path $repo $md)) }
    $ownFail = 0
    foreach ($m in $modules) {
        $name = $m[1]
        $src = Join-Path $repo (Join-Path $m[0] ($name + ".ivy"))
        $obj = Join-Path $modsDir ($name + ".obj")
        $code = Invoke-Separate $Driver (@($src) + $inc + @("-c", "-o", $obj)) `
                                 (Join-Path $modsDir ("b_" + $name + ".out")) `
                                 (Join-Path $modsDir ("b_" + $name + ".err"))
        if ($code -ne 0) {
            Write-Output ("      BUILD FAILED (" + $name + ", exit " + $code + ")")
            Get-Content (Join-Path $modsDir ("b_" + $name + ".err")) | Select-Object -First 8
            $ownFail++
            continue
        }
        if ($RefStage -ne "") {
            $ref = Join-Path $RefStage ($name + ".obj")
            if ((Test-Path -LiteralPath $ref) -and -not (Test-BytesEqual $ref $obj)) {
                Write-Output ("      DIFF obj " + $name)
                $ownFail++
            }
        }
    }
    if ($ownFail -ne 0) { Fail "the driver did not reproduce the compiler objects" }
    Write-Output ("      " + $modules.Count + " / " + $modules.Count + " modules built" +
                  $(if ($RefStage -ne "") { " (objects match the oracle)" } else { "" }))
}

# --- the corpus ----------------------------------------------------------------
# The codegen corpus is always checked; -Corpus adds the rest of the tree (the
# modes that are ported only need `--llvm` and `-c`).  Every directory is its
# own `-I` root, exactly like the per-directory harnesses.
$corpusRoot = Join-Path $repo "selfhost\corpus"
function Resolve-Corpus([string]$p) {
    if ([System.IO.Path]::IsPathRooted($p)) { return $p }
    return (Join-Path $repo $p)
}
$llvmDirs = @((Join-Path $corpusRoot "codegen"))
if ($FullCorpus) {
    $codegenDir = Join-Path $corpusRoot "codegen"
    $allDirs = @(Get-ChildItem -LiteralPath $corpusRoot -Recurse -Directory |
                 Where-Object { (Get-ChildItem -LiteralPath $_.FullName -Filter *.ivy -File).Count -gt 0 } |
                 Sort-Object FullName | ForEach-Object { $_.FullName })
    foreach ($d in $allDirs) {
        if ($d -ne $codegenDir) { $llvmDirs += $d }
    }
} else {
    foreach ($c in $Corpus) { $llvmDirs += (Resolve-Corpus $c) }
}
$linkDirs = @((Join-Path $corpusRoot "codegen"))
foreach ($c in $LinkCorpus) { $linkDirs += (Resolve-Corpus $c) }

function Get-Fixtures([string]$dir) {
    if (-not (Test-Path -LiteralPath $dir)) { Fail ("corpus not found: " + $dir) }
    $fs = @(Get-ChildItem -LiteralPath $dir -Filter *.ivy -File | Sort-Object Name)
    if ($Filter -ne "") { $fs = @($fs | Where-Object { $_.Name -like $Filter }) }
    return $fs
}

# A fixture name relative to selfhost\corpus, so two directories that both have
# a `01_x.ivy` are distinguishable in the output.
function Fixture-Name([string]$path) {
    if ($path.StartsWith($corpusRoot)) {
        return $path.Substring($corpusRoot.Length).TrimStart('\', '/')
    }
    return $path
}

$llvmSuites = @()
$llvmTotal = 0
foreach ($d in $llvmDirs) {
    $fs = Get-Fixtures $d
    $llvmSuites += ,@($d, $fs)
    $llvmTotal += $fs.Count
}
if ($llvmTotal -eq 0) { Fail "no fixtures matched" }

$oOut = Join-Path $runDir "o.out"
$oErr = Join-Path $runDir "o.err"
$sOut = Join-Path $runDir "s.out"
$sErr = Join-Path $runDir "s.err"

# --- 2. `--llvm`: stdout bytes, stderr bytes, exit status ----------------------
Write-Output ("[2/4] --llvm over " + $llvmTotal + " fixtures ...")
$llvmFail = 0
foreach ($suite in $llvmSuites) {
    $dir = $suite[0]
    $files = $suite[1]
    foreach ($f in $files) {
        $oc = Invoke-Separate $oracle @("--llvm", $f.FullName, "-I", $dir) $oOut $oErr
        $sc = Invoke-Separate $Driver @("--llvm", $f.FullName, "-I", $dir) $sOut $sErr
        $ok = ($oc -eq $sc) -and (Test-BytesEqual $oOut $sOut) -and (Test-BytesEqual $oErr $sErr)
        if (-not $ok) {
            Write-Output ("      DIFF " + (Fixture-Name $f.FullName) + " (exit " + $oc + "/" + $sc + ")")
            $llvmFail++
        }
    }
}
Write-Output ("      " + ($llvmTotal - $llvmFail) + " / " + $llvmTotal + " match")

# --- 3. `-c`: object bytes, stderr bytes (path normalized), exit status --------
Write-Output ("[3/4] -c over " + $llvmTotal + " fixtures ...")
$objFail = 0
$oObj = Join-Path $runDir "o.obj"
$sObj = Join-Path $runDir "s.obj"
foreach ($suite in $llvmSuites) {
    $dir = $suite[0]
    $files = $suite[1]
    foreach ($f in $files) {
        $oc = Invoke-Separate $oracle @($f.FullName, "-I", $dir, "-c", "-o", $oObj) $oOut $oErr
        $sc = Invoke-Separate $Driver @($f.FullName, "-I", $dir, "-c", "-o", $sObj) $sOut $sErr
        $ok = ($oc -eq $sc)
        if ($ok -and $oc -eq 0) { $ok = Test-BytesEqual $oObj $sObj }
        if ($ok) {
            # stderr quotes the output path and the `.ivm` module interface
            # derived from it (`-c` always writes one next to the object);
            # normalize both away.
            $a = (Read-Text $oErr).Replace($oObj, "<OUT>").Replace(
                     [System.IO.Path]::ChangeExtension($oObj, ".ivm"), "<IVM>")
            $b = (Read-Text $sErr).Replace($sObj, "<OUT>").Replace(
                     [System.IO.Path]::ChangeExtension($sObj, ".ivm"), "<IVM>")
            if ($a -ne $b) { $ok = $false }
        }
        if (-not $ok) {
            Write-Output ("      DIFF " + (Fixture-Name $f.FullName) + " (exit " + $oc + "/" + $sc + ")")
            $objFail++
        }
    }
}
Write-Output ("      " + ($llvmTotal - $objFail) + " / " + $llvmTotal + " match")

# --- 4. link mode: link stderr + the behaviour of the executable ---------------
# Both sides use the same output file name in their own directory: the linker
# diagnostic quotes the temp object path derived from the stem
# (`<stem>__tmp.obj`), so equal stems make the two command lines comparable.
$linkSuites = @()
$linkTotal = 0
foreach ($d in $linkDirs) {
    $fs = Get-Fixtures $d
    $linkSuites += ,@($d, $fs)
    $linkTotal += $fs.Count
}
if ($linkTotal -eq 0 -and $Filter -eq "") { Fail "no link fixtures matched" }
Write-Output ("[4/4] link + run over " + $linkTotal + " fixtures ...")
$linkFail = 0
$nondet = 0
$oExe = Join-Path $oracleExeDir "app.exe"
$sExe = Join-Path $selfExeDir "app.exe"
foreach ($suite in $linkSuites) {
    $dir = $suite[0]
    $files = $suite[1]
    foreach ($f in $files) {
        $oc = Invoke-Separate $oracle @($f.FullName, "-I", $dir, "-o", $oExe) $oOut $oErr
        $sc = Invoke-Separate $Driver @($f.FullName, "-I", $dir, "-o", $sExe) $sOut $sErr
        $ok = ($oc -eq $sc)
        if ($ok) {
            $a = Normalize-Rt ((Read-Text $oErr).Replace($oracleExeDir, "<OUT>"))
            $b = Normalize-Rt ((Read-Text $sErr).Replace($selfExeDir, "<OUT>"))
            if ($a -ne $b) { $ok = $false }
        }
        if ($ok -and $oc -eq 0) {
            if (-not (Test-Path -LiteralPath $oExe) -or -not (Test-Path -LiteralPath $sExe)) {
                $ok = $false
            } else {
                $r1 = Join-Path $runDir "r1.out"; $r1b = Join-Path $runDir "r1b.out"
                $e1 = Join-Path $runDir "r1.err"; $e1b = Join-Path $runDir "r1b.err"
                $r2 = Join-Path $runDir "r2.out"; $e2 = Join-Path $runDir "r2.err"
                $rc1 = Invoke-Separate $oExe @() $r1 $e1 $runCwd
                $rc1b = Invoke-Separate $oExe @() $r1b $e1b $runCwd
                if (($rc1 -ne $rc1b) -or -not (Test-BytesEqual $r1 $r1b) -or
                    -not (Test-BytesEqual $e1 $e1b)) {
                    # The fixture reads uninitialised storage or prints unspecified
                    # values: its own output is not reproducible, so only the link
                    # step above is meaningful.
                    $nondet++
                } else {
                    $rc2 = Invoke-Separate $sExe @() $r2 $e2 $runCwd
                    $ok = ($rc1 -eq $rc2) -and (Test-BytesEqual $r1 $r2) -and
                          (Test-BytesEqual $e1 $e2)
                }
            }
        }
        if (-not $ok) {
            Write-Output ("      DIFF " + (Fixture-Name $f.FullName) + " (exit " + $oc + "/" + $sc + ")")
            $linkFail++
        }
    }
}
Write-Output ("      " + ($linkTotal - $linkFail) + " / " + $linkTotal + " match (" +
              $nondet + " nondeterministic fixture(s) compared on the link step only)")

if ($llvmFail -ne 0 -or $objFail -ne 0 -or $linkFail -ne 0) { Fail "driver parity broken" }
Write-Output "--- DRIVER PARITY PASS ---"
exit 0
