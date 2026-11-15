# selfhost/app/build_import_smoke.ps1 - H7.2 import-resolution harness.
#
# Checks the H7.2 part of the selfhost driver (`selfhost/app/imports.ivy` plus
# the open/lex/preprocess/scan flow in `selfhost/app/main.ivy`) against the
# oracle:
#
#   [1] missing-module cases - `import <name>` / `import "<name>"` that do not
#       resolve. The oracle exits 1 right after the pre-scan, so stdout, stderr
#       and the exit status are compared as RAW BYTES. Covers `import cpp ...`
#       and `import c <...>` (skipped), `import c;` (not skipped), `#if 0`
#       (import removed) vs `#if 1`, and several missing modules in one file.
#
#   [2] resolve/dump cases - `IVY_IMPORT_DUMP=1` makes the selfhost driver print
#       the include search path and the resolved imports. The exact text is
#       compared against the expected paths, and the oracle is run on the same
#       command line with `-c`: it must exit 0, which proves the import really
#       resolved on both sides. Covers `-I` order, `<cwd>/<name>.ivm`,
#       `<cwd>/std/<name>.ivm`, the auto `<cwd>/lib` / `../lib` / `../../lib`
#       probe, the `-I ""` edge case and duplicate imports.
#
#   [3] cannot-open cases - a missing relative and a missing absolute path,
#       compared as raw bytes (`std::filesystem::path` is printed quoted with
#       `\` escaped by `std::quoted`).
#
#   [4] stub case - a source without imports, no dump env: exit 3 with the
#       H7.2 "dispatch not ported" line.
#
#   [5] CLI dump integration - `IVY_CLI_DUMP=1` from a directory that has
#       `lib/` must show the auto include path (H7.1 dump + H7.2 auto path).
#
# Everything is built under a fresh per-run directory in TEMP; the repository
# working tree is never written to. Only clang is needed (no LLVM libs).
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/app/build_import_smoke.ps1
#   powershell -NoProfile -File selfhost/app/build_import_smoke.ps1 -TempRoot C:\scratch\h72
#
# Exit 0 iff every case matches.

param(
    [string]$TempRoot = "",
    [string]$LlvmRoot = "D:\Cpp\Clang"
)

$ErrorActionPreference = "Stop"

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$ivy = Join-Path $repo "build\Release\ivy.exe"
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
$runDir = Join-Path $tempBase ("h72imp_" + $stamp + "_" + $PID)
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$env:TEMP = $runDir
$env:TMP = $runDir
Remove-Item Env:\IVY_CLI_DUMP -ErrorAction SilentlyContinue
Remove-Item Env:\IVY_IMPORT_DUMP -ErrorAction SilentlyContinue

function Quote-Arg([string]$s) {
    if ($s -eq "" -or $s -match '[\s"]') {
        return '"' + ($s -replace '"', '\"') + '"'
    }
    return $s
}

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

# Run a process, returning @{ Code; Out; Err } with raw bytes.
$script:capSeq = 0
function Run-Capture([string]$exe, [string[]]$argList, [string]$wd) {
    $script:capSeq = $script:capSeq + 1
    $o = Join-Path $runDir ("cap" + $script:capSeq + ".out")
    $e = Join-Path $runDir ("cap" + $script:capSeq + ".err")
    $code = Invoke-Separate $exe $argList "" $o $e 0 $wd
    return @{ Code = $code; Out = (Read-Bytes $o); Err = (Read-Bytes $e) }
}

function Write-Lines([string]$path, [string[]]$lines) {
    [System.IO.File]::WriteAllText($path, (($lines -join "`n") + "`n"))
}

# ---------------------------------------------------------------------------
# Build the selfhost modules with the oracle.
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

Write-Output "[1/6] building modules ..."
$tokenObj = Join-Path $runDir "token.obj"
$lexerObj = Join-Path $runDir "lexer.obj"
$ppObj = Join-Path $runDir "preprocessor.obj"
$cliObj = Join-Path $runDir "cli.obj"
$importsObj = Join-Path $runDir "imports.obj"
$mainObj = Join-Path $runDir "main.obj"

Build-Mod (Join-Path $repo "selfhost\parsing\token.ivy") $tokenObj @()
Build-Mod (Join-Path $repo "selfhost\parsing\lexer.ivy") $lexerObj @("-I", $runDir)
Build-Mod (Join-Path $repo "selfhost\parsing\preprocessor.ivy") $ppObj @("-I", $runDir)
Build-Mod (Join-Path $appDir "cli.ivy") $cliObj @("-I", $runDir, "-I", $stdDir)
Build-Mod (Join-Path $appDir "imports.ivy") $importsObj @("-I", $runDir, "-I", $stdDir)
Build-Mod (Join-Path $appDir "main.ivy") $mainObj @("-I", $runDir, "-I", $stdDir)

Write-Output "[2/6] building runtime ..."
$rtObj = Join-Path $runDir "ivyrt.obj"
$code = Invoke-Separate $clang @("-c", (Join-Path $libDir "ivyrt.c"),
                                 "-I", $libDir, "-o", $rtObj) "" `
             (Join-Path $runDir "build_rt.out") (Join-Path $runDir "build_rt.err")
if ($code -ne 0 -or -not (Test-Path -LiteralPath $rtObj)) {
    Write-Output ("BUILD FAILED (ivyrt, exit " + $code + ")")
    Write-Output (Show-Text (Read-Bytes (Join-Path $runDir "build_rt.err")))
    exit 1
}

Write-Output "[3/6] linking driver ..."
$mainExe = Join-Path $runDir "main.exe"
$code = Invoke-Separate $clang @($mainObj, $importsObj, $cliObj, $lexerObj, $ppObj,
                                 $tokenObj, $rtObj, "-o", $mainExe) "" `
             (Join-Path $runDir "link.out") (Join-Path $runDir "link.err")
if ($code -ne 0 -or -not (Test-Path -LiteralPath $mainExe)) {
    Write-Output ("BUILD FAILED (link, exit " + $code + ")")
    Write-Output (Show-Text (Read-Bytes (Join-Path $runDir "link.err")))
    exit 1
}
Write-Output ("      scratch: " + $runDir)

# ---------------------------------------------------------------------------
# Fixture tree:  <w>/a  <w>/b  <w>/std  <w>/lib  plus module and consumer
# sources. Module interfaces are produced by the oracle (`-c -o <dir>/<m>.obj`
# writes `<dir>/<m>.ivm`).
# ---------------------------------------------------------------------------
Write-Output "[4/6] building fixtures ..."
$w = Join-Path $runDir "w"
foreach ($d in @("a", "b", "std", "lib", "a\x", "a\x\y\z")) {
    New-Item -ItemType Directory -Force -Path (Join-Path $w $d) | Out-Null
}

$mods = @(
    @{ Path = "a\aa.ivy";  Lines = @("export module aa;",  "export struct Foo { int32 x; };") },
    @{ Path = "b\bb.ivy";  Lines = @("export module bb;",  "export struct Bar { int32 y; };") },
    @{ Path = "cc.ivy";    Lines = @("export module cc;",  "export struct Baz { int32 z; };") },
    @{ Path = "std\ss.ivy"; Lines = @("export module ss;", "export struct S { int32 s; };") },
    @{ Path = "lib\kk.ivy"; Lines = @("export module kk;", "export struct K { int32 k; };") },
    @{ Path = "a\dup.ivy"; Lines = @("export module dup;", "export struct A { int32 a; };") },
    @{ Path = "b\dup.ivy"; Lines = @("export module dup;", "export struct B { int32 b; };") }
)
foreach ($m in $mods) {
    $p = Join-Path $w $m.Path
    Write-Lines $p $m.Lines
    $obj = ($p -replace '\.ivy$', '.obj')
    $r = Run-Capture $ivy @($p, "-c", "-o", $obj) $repo
    if ($r.Code -ne 0) {
        Write-Output ("FIXTURE FAILED (" + $m.Path + ", exit " + $r.Code + ")")
        Write-Output (Show-Text $r.Err)
        exit 1
    }
}

$cons = @(
    @{ Path = "c_aa.ivy";       Lines = @("import aa;", "int32 main() { return 0; }") },
    @{ Path = "c_str.ivy";      Lines = @("import ""aa"";", "int32 main() { return 0; }") },
    @{ Path = "c_nope.ivy";     Lines = @("import nope;", "int32 main() { return 0; }") },
    @{ Path = "c_nope2.ivy";    Lines = @("import ""nope2"";", "int32 main() { return 0; }") },
    @{ Path = "c_two.ivy";      Lines = @("import nope;", "import nope2;", "int32 main() { return 0; }") },
    @{ Path = "c_cpp.ivy";      Lines = @("import cpp <vector>;", "import nope;", "int32 main() { return 0; }") },
    @{ Path = "c_c.ivy";        Lines = @("import c <stdio.h>;", "import nope;", "int32 main() { return 0; }") },
    @{ Path = "c_conly.ivy";    Lines = @("import c;", "int32 main() { return 0; }") },
    @{ Path = "c_if0.ivy";      Lines = @("#if 0", "import nope;", "#endif", "int32 main() { return 0; }") },
    @{ Path = "c_if1.ivy";      Lines = @("#if 1", "import nope;", "#endif", "int32 main() { return 0; }") },
    @{ Path = "c_comment.ivy";  Lines = @("// import nope;", "int32 main() { return 0; }") },
    @{ Path = "c_nosemi.ivy";   Lines = @("import nope", "int32 main() { return 0; }") },
    @{ Path = "c_dup.ivy";      Lines = @("import dup;", "int32 main() { return 0; }") },
    @{ Path = "c_cc.ivy";       Lines = @("import cc;", "int32 main() { return 0; }") },
    @{ Path = "c_ss.ivy";       Lines = @("import ss;", "int32 main() { return 0; }") },
    @{ Path = "c_kk.ivy";       Lines = @("import kk;", "int32 main() { return 0; }") },
    @{ Path = "c_order.ivy";    Lines = @("import cc;", "import aa;", "int32 main() { return 0; }") },
    @{ Path = "c_dupimp.ivy";   Lines = @("import aa;", "import aa;", "int32 main() { return 0; }") },
    @{ Path = "c_mid.ivy";      Lines = @("import aa;", "import nope;", "int32 main() { return 0; }") },
    @{ Path = "c_after.ivy";    Lines = @("struct Q { int32 q; };", "import aa;", "int32 main() { return 0; }") }
)
foreach ($c in $cons) { Write-Lines (Join-Path $w $c.Path) $c.Lines }

# ---------------------------------------------------------------------------
# [5/6] Cases.
# ---------------------------------------------------------------------------
Write-Output "[5/6] import smoke ..."

$pass = 0
$fail = 0
$failures = New-Object System.Collections.Generic.List[string]

# (a) Missing modules: byte-exact against the oracle (both exit 1 after the
# pre-scan, so stdout is empty and stderr is only the module diagnostics).
$missingCases = @(
    @{ Name = "missing-ident";   Wd = $w; Args = @("c_nope.ivy") },
    @{ Name = "missing-string";  Wd = $w; Args = @("c_nope2.ivy") },
    @{ Name = "missing-two";     Wd = $w; Args = @("c_two.ivy") },
    @{ Name = "skip-cpp";        Wd = $w; Args = @("c_cpp.ivy") },
    @{ Name = "skip-c-header";   Wd = $w; Args = @("c_c.ivy") },
    @{ Name = "c-not-skipped";   Wd = $w; Args = @("c_conly.ivy") },
    @{ Name = "if1-keeps";       Wd = $w; Args = @("c_if1.ivy") },
    @{ Name = "no-semicolon";    Wd = $w; Args = @("c_nosemi.ivy") },
    @{ Name = "resolved-then-missing"; Wd = $w; Args = @("-I", "a", "c_mid.ivy") },
    @{ Name = "no-auto-lib";     Wd = (Join-Path $w "a\x\y\z"); Args = @("..\..\..\..\c_kk.ivy") }
)

$mi = 0
foreach ($case in $missingCases) {
    $mi = $mi + 1
    $o = Run-Capture $ivy $case.Args $case.Wd
    $s = Run-Capture $mainExe $case.Args $case.Wd
    $outOk = Test-BytesEqual $o.Out $s.Out
    $errOk = Test-BytesEqual $o.Err $s.Err
    if ($o.Code -eq $s.Code -and $outOk -and $errOk) {
        Write-Output ("PASS " + $case.Name + " exit=" + $s.Code + " err=" + $s.Err.Length + "B")
        $pass = $pass + 1
    } else {
        Write-Output ("FAIL " + $case.Name + " (oracle=" + $o.Code + " self=" + $s.Code + ")")
        if (-not $outOk) {
            Write-Output ("     stdout diff at byte " + (First-Diff $o.Out $s.Out))
            Write-Output ("     oracle stdout: " + (Show-Text $o.Out))
            Write-Output ("     self stdout:   " + (Show-Text $s.Out))
        }
        if (-not $errOk) {
            Write-Output ("     stderr diff at byte " + (First-Diff $o.Err $s.Err))
            Write-Output ("     oracle stderr: " + (Show-Text $o.Err))
            Write-Output ("     self stderr:   " + (Show-Text $s.Err))
        }
        $fail = $fail + 1
        $failures.Add($case.Name)
    }
}

# (b) Resolve/dump: exact dump text + oracle `-c` must succeed.
function New-ImportExpect([string[]]$includes, [string[]]$imports) {
    $lines = @()
    if ($includes -ne $null) { foreach ($i in $includes) { $lines += ("include=" + $i) } }
    $lines += ("imports=" + @($imports).Count)
    if ($imports -ne $null) { foreach ($im in $imports) { $lines += ("import=" + $im) } }
    return (($lines -join "`r`n") + "`r`n")
}

$libW = $w + "\lib"
$dumpCases = @(
    @{ Name = "include-dir"; Wd = $w; Args = @("-I", "a", "c_aa.ivy");
       Includes = @("a", $libW); Imports = @("aa -> a\aa.ivm") },
    @{ Name = "string-form"; Wd = $w; Args = @("-I", "a", "c_str.ivy");
       Includes = @("a", $libW); Imports = @("aa -> a\aa.ivm") },
    @{ Name = "precedence-a-b"; Wd = $w; Args = @("-I", "a", "-I", "b", "c_dup.ivy");
       Includes = @("a", "b", $libW); Imports = @("dup -> a\dup.ivm") },
    @{ Name = "precedence-b-a"; Wd = $w; Args = @("-I", "b", "-I", "a", "c_dup.ivy");
       Includes = @("b", "a", $libW); Imports = @("dup -> b\dup.ivm") },
    @{ Name = "cwd-fallback"; Wd = $w; Args = @("c_cc.ivy");
       Includes = @($libW); Imports = @("cc -> " + $w + "\cc.ivm") },
    @{ Name = "std-fallback"; Wd = $w; Args = @("c_ss.ivy");
       Includes = @($libW); Imports = @("ss -> " + $w + "\std\ss.ivm") },
    @{ Name = "lib-fallback"; Wd = $w; Args = @("c_kk.ivy");
       Includes = @($libW); Imports = @("kk -> " + $libW + "\kk.ivm") },
    @{ Name = "lib-parent"; Wd = (Join-Path $w "a"); Args = @("..\c_kk.ivy");
       Includes = @($w + "\a\..\lib"); Imports = @("kk -> " + $w + "\a\..\lib\kk.ivm") },
    @{ Name = "lib-grandparent"; Wd = (Join-Path $w "a\x"); Args = @("..\..\c_kk.ivy");
       Includes = @($w + "\a\x\..\..\lib"); Imports = @("kk -> " + $w + "\a\x\..\..\lib\kk.ivm") },
    @{ Name = "no-lib-deep"; Wd = (Join-Path $w "a\x\y\z"); Args = @("..\..\..\..\c_if0.ivy");
       Includes = @(); Imports = @() },
    @{ Name = "if0-removed"; Wd = $w; Args = @("c_if0.ivy");
       Includes = @($libW); Imports = @() },
    @{ Name = "comment"; Wd = $w; Args = @("c_comment.ivy");
       Includes = @($libW); Imports = @() },
    @{ Name = "order"; Wd = $w; Args = @("-I", "a", "c_order.ivy");
       Includes = @("a", $libW); Imports = @(("cc -> " + $w + "\cc.ivm"), "aa -> a\aa.ivm") },
    @{ Name = "duplicate"; Wd = $w; Args = @("-I", "a", "c_dupimp.ivy");
       Includes = @("a", $libW); Imports = @("aa -> a\aa.ivm", "aa -> a\aa.ivm") },
    @{ Name = "missing-include-dir"; Wd = $w; Args = @("-I", "no_such", "-I", "a", "c_aa.ivy");
       Includes = @("no_such", "a", $libW); Imports = @("aa -> a\aa.ivm") },
    @{ Name = "empty-include"; Wd = $w; Args = @("-I", "", "c_cc.ivy");
       Includes = @("", $libW); Imports = @("cc -> cc.ivm") },
    @{ Name = "after-declaration"; Wd = $w; Args = @("-I", "a", "c_after.ivy");
       Includes = @("a", $libW); Imports = @("aa -> a\aa.ivm") }
)

$di = 0
foreach ($case in $dumpCases) {
    $di = $di + 1
    $env:IVY_IMPORT_DUMP = "1"
    $s = Run-Capture $mainExe $case.Args $case.Wd
    Remove-Item Env:\IVY_IMPORT_DUMP -ErrorAction SilentlyContinue

    $expB = $utf8NoBom.GetBytes((New-ImportExpect $case.Includes $case.Imports))
    $dumpOk = Test-BytesEqual $expB $s.Out
    $quietOk = ($s.Err.Length -eq 0)

    $obj = Join-Path $runDir ("xo" + $di + ".obj")
    $o = Run-Capture $ivy (@($case.Args) + @("-c", "-o", $obj)) $case.Wd
    $oracleOk = ($o.Code -eq 0)

    if ($s.Code -eq 0 -and $dumpOk -and $quietOk -and $oracleOk) {
        Write-Output ("PASS " + $case.Name + " (dump " + $s.Out.Length + "B, oracle resolved)")
        $pass = $pass + 1
    } else {
        Write-Output ("FAIL " + $case.Name + " (self=" + $s.Code + " dump=" + $dumpOk +
                      " stderr_empty=" + $quietOk + " oracle_resolved=" + $oracleOk + ")")
        if (-not $dumpOk) {
            Write-Output ("     dump diff at byte " + (First-Diff $expB $s.Out))
            Write-Output ("     expected: " + (Show-Text $expB))
            Write-Output ("     actual:   " + (Show-Text $s.Out))
        }
        if (-not $quietOk) {
            Write-Output ("     self stderr: " + (Show-Text $s.Err))
        }
        if (-not $oracleOk) {
            Write-Output ("     oracle exit=" + $o.Code + " stderr: " + (Show-Text $o.Err))
        }
        $fail = $fail + 1
        $failures.Add($case.Name)
    }
}

# (c) cannot-open: the path is streamed through `std::filesystem::path`
# `operator<<` (quoted, backslashes escaped).
$missingAbs = Join-Path $runDir "no\such\file.ivy"
$openCases = @(
    @{ Name = "cannot-open-relative"; Args = @("missing.ivy") },
    @{ Name = "cannot-open-absolute"; Args = @($missingAbs) }
)
foreach ($case in $openCases) {
    $o = Run-Capture $ivy $case.Args $w
    $s = Run-Capture $mainExe $case.Args $w
    if ($o.Code -eq $s.Code -and (Test-BytesEqual $o.Out $s.Out) -and
        (Test-BytesEqual $o.Err $s.Err)) {
        Write-Output ("PASS " + $case.Name + " exit=" + $s.Code + " err=" + $s.Err.Length + "B")
        $pass = $pass + 1
    } else {
        Write-Output ("FAIL " + $case.Name + " (oracle=" + $o.Code + " self=" + $s.Code + ")")
        Write-Output ("     oracle stderr: " + (Show-Text $o.Err))
        Write-Output ("     self stderr:   " + (Show-Text $s.Err))
        $fail = $fail + 1
        $failures.Add($case.Name)
    }
}

# (d) Stub: a source without imports and without a dump env must not compile.
$stub = Run-Capture $mainExe @("c_if0.ivy") $w
$stubExpected = $utf8NoBom.GetBytes(
    "ivy: error: selfhost driver dispatch is not ported yet (H7.2: import resolution only)`r`n")
if ($stub.Code -eq 3 -and $stub.Out.Length -eq 0 -and
    (Test-BytesEqual $stubExpected $stub.Err)) {
    Write-Output "PASS dispatch-stub (exit 3, one stderr line, no stdout)"
    $pass = $pass + 1
} else {
    Write-Output ("FAIL dispatch-stub (exit " + $stub.Code + ")")
    Write-Output ("     self stdout: " + (Show-Text $stub.Out))
    Write-Output ("     self stderr: " + (Show-Text $stub.Err))
    $fail = $fail + 1
    $failures.Add("dispatch-stub")
}

# (e) CLI dump integration: from <w> (which has lib/) the auto include path is
# appended to the H7.1 dump; from the deep no-lib directory it is absent.
function Test-CliDump([string]$name, [string]$wd, [string]$file, [bool]$expectLib) {
    $env:IVY_CLI_DUMP = "1"
    $r = Run-Capture $mainExe @($file) $wd
    Remove-Item Env:\IVY_CLI_DUMP -ErrorAction SilentlyContinue
    $text = [System.Text.Encoding]::UTF8.GetString($r.Out)
    $lines = @($text -split "`r`n" | Where-Object { $_ -ne "" })
    $incLines = @($lines | Where-Object { $_ -like "include=*" })
    $ok = ($r.Code -eq 0) -and ($r.Err.Length -eq 0)
    if ($expectLib) {
        $ok = $ok -and ($incLines.Count -eq 1) -and ($incLines[0] -eq ("include=" + $libW))
    } else {
        $ok = $ok -and ($incLines.Count -eq 0)
    }
    if ($ok) {
        Write-Output ("PASS " + $name + " (cli dump auto-lib=" + $expectLib + ")")
        $script:pass = $script:pass + 1
    } else {
        Write-Output ("FAIL " + $name + " (exit " + $r.Code + " includes=" + ($incLines -join ",") + ")")
        Write-Output ("     dump: " + (Show-Text $r.Out))
        $script:fail = $script:fail + 1
        $script:failures.Add($name)
    }
}

Test-CliDump "cli-dump-auto-lib" $w "c_aa.ivy" $true
Test-CliDump "cli-dump-no-lib" (Join-Path $w "a\x\y\z") "..\..\..\..\c_aa.ivy" $false

# ---------------------------------------------------------------------------
# Guard: nothing new next to the sources.
# ---------------------------------------------------------------------------
$after = @(Get-ChildItem -LiteralPath $appDir -File | ForEach-Object { $_.Name } |
           Sort-Object)
$newFiles = @($after | Where-Object { $before -notcontains $_ })
$missingFiles = @($before | Where-Object { $after -notcontains $_ })

$total = $pass + $fail
Write-Output ("[6/6] summary: PASS " + $pass + " / " + $total +
              " (" + $missingCases.Count + " missing, " + $dumpCases.Count +
              " dump, " + $openCases.Count + " cannot-open, 1 stub, 2 cli-dump)")
if ($newFiles.Count -ne 0 -or $missingFiles.Count -ne 0) {
    Write-Output ("      source tree changed: new=[" + ($newFiles -join ", ") +
                  "] missing=[" + ($missingFiles -join ", ") + "]")
    exit 1
}
if ($fail -ne 0) {
    Write-Output ("      FAIL: " + ($failures -join ", "))
    exit 1
}
Write-Output "PASS: import resolution and search paths match the oracle"
exit 0
