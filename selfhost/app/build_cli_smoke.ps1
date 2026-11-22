# selfhost/app/build_cli_smoke.ps1 - H7.1 CLI argument-parsing harness.
#
# Checks `selfhost/app/cli.ivy` (the port of the argument loop in
# src/app/main.cpp:1464-1611) against the oracle driver:
#
#   [1] oracle cases - every command line that makes the oracle exit *during
#       argument parsing* is run through both drivers, and stdout, stderr and
#       the exit status are compared as RAW BYTES. This covers the usage text
#       (stdout), every CLI diagnostic (stderr), the help short-circuit, and
#       the `-o ""` / empty-argument edge cases.
#
#   [2] dump cases - command lines that parse successfully. `IVY_CLI_DUMP=1`
#       makes the selfhost driver print the parsed options (see CliArgs::dump)
#       instead of dispatching; the exact dump text is compared against the
#       expected flags/derived modes. As a cross-check the oracle is run with
#       the same command line: it must get past parsing and fail in `run()`
#       with `cannot open file '<missing>'`, which proves both sides parsed the
#       arguments identically.
#
#   [3] stub case - without IVY_CLI_DUMP a successful parse must report that
#       dispatch is not ported yet (exit 3, one stderr line, no stdout), i.e.
#       H7.1 must not pretend to compile anything.
#
# Everything is built under a fresh per-run directory in TEMP; the repository
# working tree is never written to. Only clang is needed (the driver links no
# LLVM code).
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/app/build_cli_smoke.ps1
#   powershell -NoProfile -File selfhost/app/build_cli_smoke.ps1 -TempRoot C:\scratch\h71
#
# Exit 0 iff every oracle case, dump case and the stub case match.

param(
    [string]$TempRoot = "",
    [string]$LlvmRoot = "D:\Cpp\Clang"
)

$ErrorActionPreference = "Stop"

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
# H9.9: the C++ oracle no longer ships in the tree; set IVY_ORACLE to a
# compiler built from tag `0.5-baseline` to run this parity harness.
$ivy = $env:IVY_ORACLE
if (-not $ivy) {
    Write-Output "BUILD FAILED (this parity harness needs the C++ oracle: set IVY_ORACLE)"
    exit 1
}
$clang = Join-Path $LlvmRoot "bin\clang.exe"
$appDir = Join-Path $repo "selfhost\app"
$stdDir = Join-Path $repo "std"
$libDir = Join-Path $repo "lib"

function Fail([string]$message) {
    Write-Output ("FAIL: " + $message)
    exit 1
}

if (-not (Test-Path -LiteralPath $ivy)) { Fail ("oracle not found: " + $ivy) }
if (-not (Test-Path -LiteralPath $clang)) { Fail ("clang not found: " + $clang) }

# Guard: the build must not drop .ivm/.obj/.exe next to the sources.
$before = @(Get-ChildItem -LiteralPath $appDir -File | ForEach-Object { $_.Name } |
            Sort-Object)

$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$runDir = Join-Path $tempBase ("h71cli_" + $stamp + "_" + $PID)
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$env:TEMP = $runDir
$env:TMP = $runDir
Remove-Item Env:\IVY_CLI_DUMP -ErrorAction SilentlyContinue

function Quote-Arg([string]$s) {
    if ($s -eq "" -or $s -match '[\s"]') {
        return '"' + ($s -replace '"', '\"') + '"'
    }
    return $s
}

# Run one process with byte-exact, separate stdout/stderr files and return its
# exit status. `$stdinPath` is unused by the CLI tests but kept for symmetry
# with the other harnesses; pass "".
function Invoke-Separate([string]$exe, [string[]]$argList, [string]$stdinPath,
                         [string]$outPath, [string]$errPath,
                         [int]$TimeoutMs = 0, [string]$WorkingDirectory = $repo) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = $WorkingDirectory
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
    $timedOut = $false
    if ($TimeoutMs -gt 0) {
        if (-not $p.WaitForExit($TimeoutMs)) {
            $timedOut = $true
            try { $p.Kill() } catch { }
            $p.WaitForExit()
        }
    } else {
        $p.WaitForExit()
    }
    $tOut.Wait()
    $tErr.Wait()
    $fsOut.Dispose()
    $fsErr.Dispose()
    $code = $p.ExitCode
    $p.Dispose()
    if ($timedOut) { return -1 }
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

function Read-Bytes([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return [byte[]]@() }
    return [System.IO.File]::ReadAllBytes($path)
}

function Show-Text([byte[]]$b) {
    if ($b.Length -eq 0) { return "(empty)" }
    return [System.Text.Encoding]::UTF8.GetString($b).Replace("`r", "\r").Replace("`n", "\n")
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# ---------------------------------------------------------------------------
# Build one selfhost module with the oracle. Each `-c -o <run>/<m>.obj` also
# writes `<run>/<m>.ivm`, which the next module imports through `-I <run>`.
# ---------------------------------------------------------------------------
function Build-Mod([string]$source, [string]$obj, [string[]]$incs) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($obj)
    $argList = @($source) + $incs + @("-c", "-o", $obj)
    $code = Invoke-Separate $ivy $argList "" `
                 (Join-Path $runDir ("build_" + $name + ".out")) `
                 (Join-Path $runDir ("build_" + $name + ".err"))
    if ($code -ne 0 -or -not (Test-Path -LiteralPath $obj)) {
        Write-Output ("BUILD FAILED (" + $name + ", exit " + $code + ")")
        Write-Output (Show-Text (Read-Bytes (Join-Path $runDir ("build_" + $name + ".err"))))
        exit 1
    }
}

# ---------------------------------------------------------------------------
# [1/6] Build the frontend + driver modules (H7.2 needs token/lexer/preprocessor
# in addition to cli/imports).
# ---------------------------------------------------------------------------
Write-Output "[1/6] building modules ..."
$diagObj = Join-Path $runDir "diagnostic.obj"
$tokenObj = Join-Path $runDir "token.obj"
$lexerObj = Join-Path $runDir "lexer.obj"
$ppObj = Join-Path $runDir "preprocessor.obj"
$cliObj = Join-Path $runDir "cli.obj"
$importsObj = Join-Path $runDir "imports.obj"
$mainObj = Join-Path $runDir "main.obj"

Build-Mod (Join-Path $repo "selfhost\common\diagnostic.ivy") $diagObj @()
Build-Mod (Join-Path $repo "selfhost\parsing\token.ivy") $tokenObj @()
Build-Mod (Join-Path $repo "selfhost\parsing\lexer.ivy") $lexerObj @("-I", $runDir)
Build-Mod (Join-Path $repo "selfhost\parsing\preprocessor.ivy") $ppObj @("-I", $runDir)
Build-Mod (Join-Path $appDir "cli.ivy") $cliObj @("-I", $runDir, "-I", $stdDir)
Build-Mod (Join-Path $appDir "imports.ivy") $importsObj @("-I", $runDir, "-I", $stdDir)
Build-Mod (Join-Path $appDir "main.ivy") $mainObj @("-I", $runDir, "-I", $stdDir)

$cliIvm = Join-Path $runDir "cli.ivm"
if (-not (Test-Path -LiteralPath $cliIvm)) { Fail ("module interface missing: " + $cliIvm) }

# ---------------------------------------------------------------------------
# [2/6] Build the Ivy runtime (std/rt.ivy -> rt.obj).
# ---------------------------------------------------------------------------
Write-Output "[2/6] building runtime ..."
$rtObj = Join-Path $runDir "rt.obj"
$code = Invoke-Separate $ivy @("-c", (Join-Path $repo "std\rt.ivy"),
                               "-I", (Join-Path $repo "std"), "-o", $rtObj) "" `
             (Join-Path $runDir "build_rt.out") (Join-Path $runDir "build_rt.err")
if ($code -ne 0 -or -not (Test-Path -LiteralPath $rtObj)) {
    Write-Output ("BUILD FAILED (rt, exit " + $code + ")")
    Write-Output (Show-Text (Read-Bytes (Join-Path $runDir "build_rt.err")))
    exit 1
}

# ---------------------------------------------------------------------------
# [3/6] Link the driver.
# ---------------------------------------------------------------------------
Write-Output "[3/6] linking driver ..."
$mainExe = Join-Path $runDir "main.exe"
$code = Invoke-Separate $clang @($mainObj, $importsObj, $cliObj, $lexerObj, $ppObj,
                                 $tokenObj, $diagObj, $rtObj, "-o", $mainExe) "" `
             (Join-Path $runDir "link.out") (Join-Path $runDir "link.err")
if ($code -ne 0 -or -not (Test-Path -LiteralPath $mainExe)) {
    Write-Output ("BUILD FAILED (link, exit " + $code + ")")
    Write-Output (Show-Text (Read-Bytes (Join-Path $runDir "link.err")))
    exit 1
}
Write-Output ("      scratch: " + $runDir)

# H7.2: `main.ivy` appends the auto `lib/` include path when it exists. To keep
# the H7.1 dump text unchanged, run the dump cases from a directory that has no
# `lib/` in itself, its parent or its grandparent.
$dumpCwd = Join-Path $runDir "cwd\deep\nest"
New-Item -ItemType Directory -Force -Path $dumpCwd | Out-Null

# ---------------------------------------------------------------------------
# Expected dump text. Field order is fixed by CliArgs::dump.
# ---------------------------------------------------------------------------
function New-Expect([string]$fileVal, [string]$outVal, [string]$targetVal,
                    [string[]]$includes, [int]$tokens = 0, [int]$lex = 0,
                    [int]$ast = 0, [int]$hir = 0, [int]$mir = 0, [int]$llvm = 0,
                    [int]$run = 0, [int]$compileOnly = 0, [int]$crtStatic = 0,
                    [int]$linkMode = 0) {
    $lines = @(
        ("file=" + $fileVal),
        ("tokens=" + $tokens),
        ("lex=" + $lex),
        ("ast=" + $ast),
        ("hir=" + $hir),
        ("mir=" + $mir),
        ("llvm=" + $llvm),
        ("run=" + $run),
        ("compile_only=" + $compileOnly),
        ("crt_static=" + $crtStatic),
        ("link_mode=" + $linkMode),
        ("target=" + $targetVal),
        ("out=" + $outVal)
    )
    foreach ($i in $includes) { $lines += ("include=" + $i) }
    return (($lines -join "`r`n") + "`r`n")
}

# ---------------------------------------------------------------------------
# [4/6] CLI smoke.
# ---------------------------------------------------------------------------
Write-Output "[4/6] CLI smoke ..."

$pass = 0
$fail = 0
$failures = New-Object System.Collections.Generic.List[string]

# (a) Oracle cases: parsing must fail (or print help) exactly like the oracle.
$oracleCases = @(
    @{ Name = "no-args";            Args = @() },
    @{ Name = "help-short";         Args = @("-h") },
    @{ Name = "help-long";          Args = @("--help") },
    @{ Name = "help-wins";          Args = @("--help", "--bogus") },
    @{ Name = "unknown-then-help";  Args = @("--bogus", "--help") },
    @{ Name = "unknown-long";       Args = @("--bogus") },
    @{ Name = "unknown-dash";       Args = @("-") },
    @{ Name = "unknown-dashdash";   Args = @("--") },
    @{ Name = "unknown-short";      Args = @("-x") },
    @{ Name = "compile-only";       Args = @("-c") },
    @{ Name = "crt-static";         Args = @("--crt-static") },
    @{ Name = "tokens-only";        Args = @("--tokens") },
    @{ Name = "run-and-unknown";    Args = @("--run", "--bogus") },
    @{ Name = "target-missing";     Args = @("--target") },
    @{ Name = "target-bad";         Args = @("--target", "bogus") },
    @{ Name = "target-eats-help";   Args = @("--target", "--help") },
    @{ Name = "target-no-file";     Args = @("--target", "itanium") },
    @{ Name = "out-missing";        Args = @("-o") },
    @{ Name = "inc-missing";        Args = @("-I") },
    @{ Name = "inc-joined-no-file"; Args = @("-Ifoo") },
    @{ Name = "out-no-file";        Args = @("-o", "out.obj") },
    @{ Name = "empty-arg";          Args = @("") },
    @{ Name = "ext-missing";        Args = @("foo") },
    @{ Name = "ext-trailing-dot";   Args = @("foo.") },
    @{ Name = "ext-unknown";        Args = @("foo.txt") },
    @{ Name = "ext-unknown-case";   Args = @("foo.TxT") },
    @{ Name = "ext-legacy-cpp";     Args = @("foo.cpp") },
    @{ Name = "ext-legacy-cpp-up";  Args = @("foo.CPP") },
    @{ Name = "ext-legacy-cc";      Args = @("foo.cc") },
    @{ Name = "ext-legacy-cxx";     Args = @("foo.cxx") },
    @{ Name = "ext-legacy-c";       Args = @("foo.c") },
    @{ Name = "ext-legacy-c-up";    Args = @("foo.C") },
    @{ Name = "file-then-unknown";  Args = @("foo.ivy", "--bogus") },
    @{ Name = "inc-then-unknown";   Args = @("-I", "x", "--bogus") },
    @{ Name = "out-then-unknown";   Args = @("-o", "x", "--bogus") },
    @{ Name = "last-positional";    Args = @("a.ivy", "b.txt") },
    @{ Name = "inc-then-ext";       Args = @("-Ifoo", "bar") },
    @{ Name = "target-then-ext";    Args = @("--target", "msvc", "foo") },
    @{ Name = "out-ll-then-ext";    Args = @("-o", "a.ll", "foo") },
    @{ Name = "flags-then-legacy";  Args = @("--crt-static", "-c", "foo.cpp") }
)

$caseIndex = 0
foreach ($case in $oracleCases) {
    $caseIndex = $caseIndex + 1
    $oOut = Join-Path $runDir ("oc" + $caseIndex + ".out")
    $oErr = Join-Path $runDir ("oc" + $caseIndex + ".err")
    $sOut = Join-Path $runDir ("sc" + $caseIndex + ".out")
    $sErr = Join-Path $runDir ("sc" + $caseIndex + ".err")
    $oCode = Invoke-Separate $ivy $case.Args "" $oOut $oErr
    $sCode = Invoke-Separate $mainExe $case.Args "" $sOut $sErr
    $oOutB = Read-Bytes $oOut; $sOutB = Read-Bytes $sOut
    $oErrB = Read-Bytes $oErr; $sErrB = Read-Bytes $sErr
    $outOk = Test-BytesEqual $oOutB $sOutB
    $errOk = Test-BytesEqual $oErrB $sErrB
    if ($oCode -eq $sCode -and $outOk -and $errOk) {
        Write-Output ("PASS " + $case.Name + " exit=" + $sCode +
                      " out=" + $sOutB.Length + "B err=" + $sErrB.Length + "B")
        $pass = $pass + 1
    } else {
        Write-Output ("FAIL " + $case.Name + " (oracle=" + $oCode + " self=" + $sCode + ")")
        if (-not $outOk) {
            Write-Output ("     stdout diff at byte " + (First-Diff $oOutB $sOutB))
            Write-Output ("     oracle stdout: " + (Show-Text $oOutB))
            Write-Output ("     self stdout:   " + (Show-Text $sOutB))
        }
        if (-not $errOk) {
            Write-Output ("     stderr diff at byte " + (First-Diff $oErrB $sErrB))
            Write-Output ("     oracle stderr: " + (Show-Text $oErrB))
            Write-Output ("     self stderr:   " + (Show-Text $sErrB))
        }
        $fail = $fail + 1
        $failures.Add($case.Name)
    }
}

# (b) Dump cases: the parse must succeed and yield the expected flags/modes.
# The oracle cross-check must fail in run() with "cannot open file".
$missing = "missing.ivy"

$dumpCases = @(
    @{ Name = "default-link";       Args = @($missing);
       Expect = (New-Expect $missing "-" "none" @() -linkMode 1) },
    @{ Name = "compile-only";       Args = @("-c", $missing);
       Expect = (New-Expect $missing "-" "none" @() -compileOnly 1) },
    @{ Name = "run";                Args = @("--run", $missing);
       Expect = (New-Expect $missing "-" "none" @() -run 1) },
    @{ Name = "llvm";               Args = @("--llvm", $missing);
       Expect = (New-Expect $missing "-" "none" @() -llvm 1) },
    @{ Name = "crt-static";         Args = @("--crt-static", $missing);
       Expect = (New-Expect $missing "-" "none" @() -crtStatic 1 -linkMode 1) },
    @{ Name = "debug-flags";        Args = @("--tokens", "--lex", "--ast", "--hir", "--mir", $missing);
       Expect = (New-Expect $missing "-" "none" @() -tokens 1 -lex 1 -ast 1 -hir 1 -mir 1) },
    @{ Name = "target-itanium";     Args = @("--target", "itanium", $missing);
       Expect = (New-Expect $missing "-" "itanium" @() -linkMode 1) },
    @{ Name = "target-msvc";        Args = @("--target", "msvc", $missing);
       Expect = (New-Expect $missing "-" "msvc" @() -linkMode 1) },
    @{ Name = "includes";           Args = @("-I", "a", "-I", "b", "-Ic", $missing);
       Expect = (New-Expect $missing "-" "none" @("a", "b", "c") -linkMode 1) },
    @{ Name = "include-dash-value"; Args = @("-I", "-c", $missing);
       Expect = (New-Expect $missing "-" "none" @("-c") -linkMode 1) },
    @{ Name = "include-joined";     Args = @("-Ifoo", $missing);
       Expect = (New-Expect $missing "-" "none" @("foo") -linkMode 1) },
    @{ Name = "out-obj";            Args = @("-o", "out.obj", $missing);
       Expect = (New-Expect $missing "out.obj" "none" @() -compileOnly 1) },
    @{ Name = "out-obj-upper";      Args = @("-o", "out.OBJ", $missing);
       Expect = (New-Expect $missing "out.OBJ" "none" @() -compileOnly 1) },
    @{ Name = "out-o";              Args = @("-o", "out.o", $missing);
       Expect = (New-Expect $missing "out.o" "none" @() -compileOnly 1) },
    @{ Name = "out-ll";             Args = @("-o", "out.ll", $missing);
       Expect = (New-Expect $missing "out.ll" "none" @() -llvm 1) },
    @{ Name = "out-ll-upper";       Args = @("-o", "out.LL", $missing);
       Expect = (New-Expect $missing "out.LL" "none" @() -linkMode 1) },
    @{ Name = "out-i";              Args = @("-o", "out.i", $missing);
       Expect = (New-Expect $missing "out.i" "none" @()) },
    @{ Name = "out-ii";             Args = @("-o", "out.ii", $missing);
       Expect = (New-Expect $missing "out.ii" "none" @()) },
    @{ Name = "out-exe";            Args = @("-o", "out.exe", $missing);
       Expect = (New-Expect $missing "out.exe" "none" @() -linkMode 1) },
    @{ Name = "out-empty";          Args = @("-o", "", $missing);
       Expect = (New-Expect $missing "-" "none" @() -linkMode 1) },
    @{ Name = "out-dash-value";     Args = @("-o", "-c", $missing);
       Expect = (New-Expect $missing "-c" "none" @() -linkMode 1) },
    @{ Name = "last-positional";    Args = @("a.ivy", $missing);
       Expect = (New-Expect $missing "-" "none" @() -linkMode 1) },
    @{ Name = "ivy-upper";          Args = @("missing.IVY");
       Expect = (New-Expect "missing.IVY" "-" "none" @() -linkMode 1) },
    @{ Name = "explicit-c-and-out"; Args = @("-c", "-o", "out.obj", $missing);
       Expect = (New-Expect $missing "out.obj" "none" @() -compileOnly 1) },
    @{ Name = "llvm-and-out-ll";    Args = @("--llvm", "-o", "out.ll", $missing);
       Expect = (New-Expect $missing "out.ll" "none" @() -llvm 1) },
    @{ Name = "combined";           Args = @("--target", "msvc", "-c", "-o", "x.obj", "-I", "inc", $missing);
       Expect = (New-Expect $missing "x.obj" "msvc" @("inc") -compileOnly 1) }
)

$dumpIndex = 0
foreach ($case in $dumpCases) {
    $dumpIndex = $dumpIndex + 1
    $sOut = Join-Path $runDir ("dc" + $dumpIndex + ".out")
    $sErr = Join-Path $runDir ("dc" + $dumpIndex + ".err")
    $oOut = Join-Path $runDir ("do" + $dumpIndex + ".out")
    $oErr = Join-Path $runDir ("do" + $dumpIndex + ".err")

    $env:IVY_CLI_DUMP = "1"
    $sCode = Invoke-Separate $mainExe $case.Args "" $sOut $sErr 0 $dumpCwd
    Remove-Item Env:\IVY_CLI_DUMP -ErrorAction SilentlyContinue

    $oCode = Invoke-Separate $ivy $case.Args "" $oOut $oErr

    $sOutB = Read-Bytes $sOut; $sErrB = Read-Bytes $sErr
    $expB = $utf8NoBom.GetBytes($case.Expect)
    $dumpOk = Test-BytesEqual $expB $sOutB
    $quietOk = ($sErrB.Length -eq 0)
    # The oracle must get past parsing and fail opening the (missing) source.
    # `std::filesystem::path` is streamed quoted (`operator<<` uses
    # `std::quoted`), hence the embedded double quotes.
    $oracleFile = $case.Args[$case.Args.Count - 1]
    $oracleMissingErr = $utf8NoBom.GetBytes(
        "ivy: error: cannot open file '`"" + $oracleFile + "`"'`r`n")
    $oracleOk = ($oCode -eq 2) -and
                (Test-BytesEqual $oracleMissingErr (Read-Bytes $oErr))

    if ($sCode -eq 0 -and $dumpOk -and $quietOk -and $oracleOk) {
        Write-Output ("PASS " + $case.Name + " (dump " + $sOutB.Length + "B, oracle parsed)")
        $pass = $pass + 1
    } else {
        Write-Output ("FAIL " + $case.Name + " (self=" + $sCode + " dump=" + $dumpOk +
                      " stderr_empty=" + $quietOk + " oracle_parsed=" + $oracleOk + ")")
        if (-not $dumpOk) {
            Write-Output ("     dump diff at byte " + (First-Diff $expB $sOutB))
            Write-Output ("     expected: " + (Show-Text $expB))
            Write-Output ("     actual:   " + (Show-Text $sOutB))
        }
        if (-not $quietOk) {
            Write-Output ("     self stderr: " + (Show-Text $sErrB))
        }
        if (-not $oracleOk) {
            Write-Output ("     oracle exit=" + $oCode + " stderr: " +
                          (Show-Text (Read-Bytes $oErr)))
        }
        $fail = $fail + 1
        $failures.Add($case.Name)
    }
}

# (c) Stub case: a successful scan without IVY_CLI_DUMP must not compile.
# H7.2 opens/lexes/preprocesses the source first, so use a real (empty of
# imports) file rather than a missing one.
$stubFile = Join-Path $dumpCwd "ok.ivy"
[System.IO.File]::WriteAllText($stubFile, "int32 main() { return 0; }`n")
$stubOut = Join-Path $runDir "stub.out"
$stubErr = Join-Path $runDir "stub.err"
$stubCode = Invoke-Separate $mainExe @("ok.ivy") "" $stubOut $stubErr 0 $dumpCwd
$stubOutB = Read-Bytes $stubOut
$stubErrB = Read-Bytes $stubErr
$stubExpected = $utf8NoBom.GetBytes(
    "ivy: error: selfhost driver dispatch is not ported yet (H7.2: import resolution only)`r`n")
if ($stubCode -eq 3 -and $stubOutB.Length -eq 0 -and
    (Test-BytesEqual $stubExpected $stubErrB)) {
    Write-Output "PASS dispatch-stub (exit 3, one stderr line, no stdout)"
    $pass = $pass + 1
} else {
    Write-Output ("FAIL dispatch-stub (exit " + $stubCode + ")")
    Write-Output ("     self stdout: " + (Show-Text $stubOutB))
    Write-Output ("     self stderr: " + (Show-Text $stubErrB))
    $fail = $fail + 1
    $failures.Add("dispatch-stub")
}

# Guard: nothing new next to the sources.
$after = @(Get-ChildItem -LiteralPath $appDir -File | ForEach-Object { $_.Name } |
           Sort-Object)
$newFiles = @($after | Where-Object { $before -notcontains $_ })
$missingFiles = @($before | Where-Object { $after -notcontains $_ })

$total = $pass + $fail
Write-Output ("[6/6] summary: PASS " + $pass + " / " + $total +
              " (" + $oracleCases.Count + " oracle, " + $dumpCases.Count +
              " dump, 1 stub)")
if ($newFiles.Count -ne 0 -or $missingFiles.Count -ne 0) {
    Write-Output ("      source tree changed: new=[" + ($newFiles -join ", ") +
                  "] missing=[" + ($missingFiles -join ", ") + "]")
    exit 1
}
if ($fail -ne 0) {
    Write-Output ("      FAIL: " + ($failures -join ", "))
    exit 1
}
Write-Output "PASS: CLI parsing matches the oracle and derived modes are correct"
exit 0
