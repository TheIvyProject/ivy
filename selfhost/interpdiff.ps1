# selfhost/interpdiff.ps1 — H5.4 differential harness for `ivy --run`.
#
# Unlike difftest.ps1 this harness is built specifically for the interpreter:
#   * oracle and selfhost run in SEPARATE processes, one process per fixture;
#   * stdout and stderr are redirected to SEPARATE files (never `2>&1`);
#   * both streams are compared as RAW BYTES (no decoding, no line filtering,
#     no empty-line drop, no CR/LF normalization);
#   * the process exit status is compared too (nonzero main return, exit(code),
#     runtime diagnostics).
#
# Build: compiles every selfhost module and `selfhost/mir/interpdrive.ivy`
# into a fresh TEMP directory. No .obj/.ivm is written into the repository.
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/interpdiff.ps1
#   powershell -NoProfile -File selfhost/interpdiff.ps1 -Dir selfhost/corpus/interp
#   powershell -NoProfile -File selfhost/interpdiff.ps1 -TempRoot C:\scratch\h54
#
# -TempRoot is the PARENT directory for a per-run scratch directory; when it is
# empty the platform temp directory is used. The run directory becomes the
# child processes' TEMP/TMP, so no user/global temp is shared.
#
# Exit 0 iff every fixture matches stdout bytes, stderr bytes, and exit status.

param(
    [string]$Dir = "selfhost/corpus/interp",
    [string]$TempRoot = ""
)

$ErrorActionPreference = "Stop"

$repo = Split-Path $PSScriptRoot -Parent
$ivy = Join-Path $repo "build\Release\ivy.exe"
if (-not (Test-Path -LiteralPath $ivy)) {
    Write-Output "BUILD FAILED (oracle not found: $ivy)"
    exit 1
}

$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$runDir = Join-Path $tempBase ("h54interp_" + $stamp + "_" + $PID)
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

# Every compiler and fixture process inherits this isolated TEMP/TMP.
$env:TEMP = $runDir
$env:TMP = $runDir

function Quote-Arg([string]$s) {
    if ($s -match '[\s"]') {
        return '"' + ($s -replace '"', '\"') + '"'
    }
    return $s
}

# Run one process with byte-exact, separate stdout/stderr files and return its
# exit status. Optional stdin is a UTF-8/ASCII file whose bytes are forwarded.
function Invoke-Separate([string]$exe, [string[]]$argList, [string]$stdinPath,
                         [string]$outPath, [string]$errPath) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = $repo
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = ($stdinPath -ne "")
    $psi.CreateNoWindow = $true
    $quoted = @($argList | ForEach-Object { Quote-Arg $_ })
    $psi.Arguments = ($quoted -join " ")

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    if (-not $p.Start()) { throw "cannot start $exe" }

    if ($stdinPath -ne "") {
        $text = [System.IO.File]::ReadAllText($stdinPath)
        $p.StandardInput.Write($text)
        $p.StandardInput.Close()
    }

    $fsOut = [System.IO.File]::Create($outPath)
    $fsErr = [System.IO.File]::Create($errPath)
    $tOut = $p.StandardOutput.BaseStream.CopyToAsync($fsOut)
    $tErr = $p.StandardError.BaseStream.CopyToAsync($fsErr)
    $p.WaitForExit()
    $tOut.Wait()
    $tErr.Wait()
    $fsOut.Dispose()
    $fsErr.Dispose()
    $code = $p.ExitCode
    $p.Dispose()
    return $code
}

function Test-BytesEqual([byte[]]$a, [byte[]]$b) {
    if ($a.Length -ne $b.Length) { return $false }
    for ($i = 0; $i -lt $a.Length; $i++) {
        if ($a[$i] -ne $b[$i]) { return $false }
    }
    return $true
}

function First-Diff([byte[]]$a, [byte[]]$b) {
    $n = [Math]::Min($a.Length, $b.Length)
    for ($i = 0; $i -lt $n; $i++) {
        if ($a[$i] -ne $b[$i]) { return $i }
    }
    if ($a.Length -ne $b.Length) { return $n }
    return -1
}

function Show-Bytes([byte[]]$b) {
    if ($b.Length -eq 0) { return "(empty)" }
    return [BitConverter]::ToString($b)
}

# 1. Build the selfhost modules + driver in the fresh run directory.
Write-Output "[1/4] building selfhost interpreter driver ..."
$moduleDirs = @("selfhost\parsing", "selfhost\hir", "selfhost\mir", "std")
$inc = @("-I", $runDir)
foreach ($md in $moduleDirs) { $inc += @("-I", (Join-Path $repo $md)) }

$modules = @(
    @("std", "io"),
    @("selfhost\parsing", "token"),
    @("selfhost\parsing", "lexer"),
    @("selfhost\parsing", "preprocessor"),
    @("selfhost\parsing", "ast"),
    @("selfhost\parsing", "parser"),
    @("selfhost\hir", "hir"),
    @("selfhost\hir", "hir_builder"),
    @("selfhost\mir", "mir"),
    @("selfhost\mir", "mir_builder"),
    @("selfhost\mir", "interpreter")
)

foreach ($m in $modules) {
    $name = $m[1]
    $src = Join-Path $repo (Join-Path $m[0] ($name + ".ivy"))
    $obj = Join-Path $runDir ($name + ".obj")
    $bOut = Join-Path $runDir ("build_" + $name + ".out")
    $bErr = Join-Path $runDir ("build_" + $name + ".err")
    $bArgs = @($src) + $inc + @("-c", "-o", $obj)
    $code = Invoke-Separate $ivy $bArgs "" $bOut $bErr
    if ($code -ne 0) {
        Write-Output ("BUILD FAILED (" + $name + ", exit " + $code + ")")
        if (Test-Path -LiteralPath $bErr) { Write-Output ([System.IO.File]::ReadAllText($bErr)) }
        exit 1
    }
}

$driverExe = Join-Path $runDir "interpdrive.exe"
$dOut = Join-Path $runDir "build_interpdrive.out"
$dErr = Join-Path $runDir "build_interpdrive.err"
$dArgs = @((Join-Path $repo "selfhost\mir\interpdrive.ivy")) + $inc + @("-o", $driverExe)
$dCode = Invoke-Separate $ivy $dArgs "" $dOut $dErr
if ($dCode -ne 0) {
    Write-Output ("BUILD FAILED (interpdrive, exit " + $dCode + ")")
    if (Test-Path -LiteralPath $dErr) { Write-Output ([System.IO.File]::ReadAllText($dErr)) }
    exit 1
}
Write-Output ("      scratch: " + $runDir)

# 2. Collect the corpus, sorted by file name.
Write-Output "[2/4] collecting corpus ..."
$dirAbs = $Dir
if (-not [System.IO.Path]::IsPathRooted($dirAbs)) { $dirAbs = Join-Path $repo $Dir }
if (-not (Test-Path -LiteralPath $dirAbs)) {
    Write-Output ("no corpus directory: " + $dirAbs)
    exit 1
}
$files = @(Get-ChildItem -LiteralPath $dirAbs -Filter *.ivy -File | Sort-Object Name)
if ($files.Count -eq 0) {
    Write-Output ("no corpus files in " + $Dir)
    exit 1
}

$repoPrefix = $repo.TrimEnd('\') + '\'
function Get-FixtureArg([string]$full) {
    if ($full.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($repoPrefix.Length).Replace('\', '/')
    }
    return $full
}

# 3. Run oracle and driver separately, one process per fixture.
Write-Output ("[3/4] running " + $files.Count + " fixtures ...")
$pass = 0
$fail = 0
$failures = New-Object System.Collections.Generic.List[string]

foreach ($f in $files) {
    $id = $f.Name
    $arg = Get-FixtureArg $f.FullName
    $safe = $f.BaseName -replace '[^A-Za-z0-9_.-]', '_'

    $oracleOut = Join-Path $runDir ($safe + ".oracle.out")
    $oracleErr = Join-Path $runDir ($safe + ".oracle.err")
    $driverOut = Join-Path $runDir ($safe + ".driver.out")
    $driverErr = Join-Path $runDir ($safe + ".driver.err")
    $driverIn = Join-Path $runDir ($safe + ".driver.in")

    $oArgs = @("--run", $arg)
    $oracleCode = Invoke-Separate $ivy $oArgs "" $oracleOut $oracleErr

    [System.IO.File]::WriteAllText($driverIn, ($arg + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    $drvArgs = @()
    $driverCode = Invoke-Separate $driverExe $drvArgs $driverIn $driverOut $driverErr

    $obO = [System.IO.File]::ReadAllBytes($oracleOut)
    $obE = [System.IO.File]::ReadAllBytes($oracleErr)
    $dbO = [System.IO.File]::ReadAllBytes($driverOut)
    $dbE = [System.IO.File]::ReadAllBytes($driverErr)

    $outOk = Test-BytesEqual $obO $dbO
    $errOk = Test-BytesEqual $obE $dbE
    $exitOk = ($oracleCode -eq $driverCode)

    if ($outOk -and $errOk -and $exitOk) {
        Write-Output ("PASS " + $id)
        $pass = $pass + 1
    } else {
        Write-Output ("FAIL " + $id)
        if (-not $exitOk) {
            Write-Output ("     exit: oracle=" + $oracleCode + " driver=" + $driverCode)
        }
        if (-not $outOk) {
            Write-Output ("     stdout diff at byte " + (First-Diff $obO $dbO))
            Write-Output ("     oracle stdout: " + (Show-Bytes $obO))
            Write-Output ("     driver stdout: " + (Show-Bytes $dbO))
        }
        if (-not $errOk) {
            Write-Output ("     stderr diff at byte " + (First-Diff $obE $dbE))
            Write-Output ("     oracle stderr: " + (Show-Bytes $obE))
            Write-Output ("     driver stderr: " + (Show-Bytes $dbE))
        }
        $fail = $fail + 1
        $failures.Add($id)
    }
}

# 4. Summary.
Write-Output "[4/4] summary"
Write-Output ("      PASS " + $pass + " / " + $files.Count)
if ($fail -ne 0) {
    Write-Output ("      FAIL " + $fail + ": " + ($failures -join ", "))
    exit 1
}
Write-Output "PASS: stdout bytes, stderr bytes, and exit status match for every fixture"
exit 0
