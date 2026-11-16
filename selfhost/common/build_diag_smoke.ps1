# selfhost/common/build_diag_smoke.ps1 - H7.4 diagnostic harness.
#
# Checks the H7.4 diagnostic port (`selfhost/common/diagnostic.ivy` plus the
# printers wired into the lexer, the preprocessor, the parser and the drivers)
# against the oracle:
#
#   [5/7] app-stderr  - `main.exe <file>` vs `ivy <file>`: the real driver must
#         write the lexer / preprocessor diagnostics to STDERR, byte-exactly
#         (same text, same quoting, same order, same exit status). Covers the
#         whole lexer error corpus, every preprocessor diagnostic (directives,
#         conditionals, macro arity, `#line` remap, multiple diagnostics) and
#         the `std::filesystem::path` quoting of paths with spaces, non-ASCII
#         bytes, deep nesting and missing files.
#
#   [6/7] stage-stdout - the differential drivers (lexdrive / ppdrive /
#         astdrive) must write the same lines to STDOUT, because the merged
#         `2>&1` comparison in difftest.ps1 sees one stream. Covers the lexer
#         error corpus (`--lex`), the preprocessor fixtures (`--tokens`,
#         including warnings followed by the token dump) and the parser error
#         corpus (`--ast`), plus a lex-error file fed to ppdrive / astdrive to
#         exercise the "lexer failed, no later dump" path.
#
#   [7/7] summary + a guard that no `.obj`/`.ivm`/`.exe` appeared next to the
#         sources.
#
# Everything is built under a fresh per-run directory in TEMP; the repository
# working tree is never written to. Only clang is needed (no LLVM libs).
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/common/build_diag_smoke.ps1
#   powershell -NoProfile -File selfhost/common/build_diag_smoke.ps1 -TempRoot C:\scratch\h74
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
$commonDir = Join-Path $repo "selfhost\common"
$parseDir = Join-Path $repo "selfhost\parsing"
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
function Get-SourceNames([string]$dir) {
    return @(Get-ChildItem -LiteralPath $dir -File | ForEach-Object { $_.Name } | Sort-Object)
}
$before = @{}
foreach ($d in @($commonDir, $parseDir, $appDir)) { $before[$d] = Get-SourceNames $d }

$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$runDir = Join-Path $tempBase ("h74diag_" + $stamp + "_" + $PID)
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$env:TEMP = $runDir
$env:TMP = $runDir

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

# Run a process through cmd.exe with `> out 2>&1`, so the child's stdout and
# stderr are merged in real write order (the oracle prints diagnostics to
# stderr and dumps to stdout, and the drivers print everything to stdout).
function Run-Merged([string]$exe, [string[]]$argList, [string]$wd, [string]$outPath) {
    $quoted = @($argList | ForEach-Object { Quote-Arg $_ })
    $line = '"' + $exe + '" ' + ($quoted -join " ") + ' > "' + $outPath + '" 2>&1'
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    $psi.WorkingDirectory = $wd
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.Arguments = '/d /c "' + $line + '"'
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    if (-not $p.Start()) { throw "cannot start cmd.exe" }
    $p.WaitForExit()
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

# Only the platform line terminator is normalized (CR LF -> LF); the byte
# comparison stays byte-exact for the message text (the diagnostics contain
# UTF-8, e.g. the em dash in the `#include` message).
function Convert-Lf([byte[]]$b) {
    $o = New-Object System.Collections.Generic.List[byte]
    for ($i = 0; $i -lt $b.Length; $i++) {
        if ($b[$i] -eq 13 -and $i + 1 -lt $b.Length -and $b[$i + 1] -eq 10) { continue }
        $o.Add($b[$i])
    }
    return ,$o.ToArray()
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
    [System.IO.File]::WriteAllText($path, (($lines -join "`n") + "`n"), $utf8NoBom)
}

$pass = 0
$fail = 0
$failures = New-Object System.Collections.Generic.List[string]

function Add-Pass([string]$name, [string]$detail) {
    Write-Output ("PASS " + $name + " " + $detail)
    $script:pass = $script:pass + 1
}

function Add-Fail([string]$name, [string[]]$detail) {
    Write-Output ("FAIL " + $name)
    foreach ($d in $detail) { if ($d -ne $null -and $d -ne "") { Write-Output ("     " + $d) } }
    $script:fail = $script:fail + 1
    $script:failures.Add($name)
}

# ---------------------------------------------------------------------------
# [1/7] Build the selfhost modules with the oracle.
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

Write-Output "[1/7] building modules ..."
$incs = @("-I", $runDir, "-I", $stdDir)
$diagObj = Join-Path $runDir "diagnostic.obj"
$tokenObj = Join-Path $runDir "token.obj"
$lexerObj = Join-Path $runDir "lexer.obj"
$ppObj = Join-Path $runDir "preprocessor.obj"
$astObj = Join-Path $runDir "ast.obj"
$parserObj = Join-Path $runDir "parser.obj"
$cliObj = Join-Path $runDir "cli.obj"
$importsObj = Join-Path $runDir "imports.obj"
$mainObj = Join-Path $runDir "main.obj"
$lexDriveObj = Join-Path $runDir "lexdrive.obj"
$ppDriveObj = Join-Path $runDir "ppdrive.obj"
$astDriveObj = Join-Path $runDir "astdrive.obj"

Build-Mod (Join-Path $commonDir "diagnostic.ivy") $diagObj $incs
Build-Mod (Join-Path $parseDir "token.ivy") $tokenObj $incs
Build-Mod (Join-Path $parseDir "lexer.ivy") $lexerObj $incs
Build-Mod (Join-Path $parseDir "preprocessor.ivy") $ppObj $incs
Build-Mod (Join-Path $parseDir "ast.ivy") $astObj $incs
Build-Mod (Join-Path $parseDir "parser.ivy") $parserObj $incs
Build-Mod (Join-Path $appDir "cli.ivy") $cliObj $incs
Build-Mod (Join-Path $appDir "imports.ivy") $importsObj $incs
Build-Mod (Join-Path $appDir "main.ivy") $mainObj $incs
Build-Mod (Join-Path $parseDir "lexdrive.ivy") $lexDriveObj $incs
Build-Mod (Join-Path $parseDir "ppdrive.ivy") $ppDriveObj $incs
Build-Mod (Join-Path $parseDir "astdrive.ivy") $astDriveObj $incs

# ---------------------------------------------------------------------------
# [2/7] Runtime.
# ---------------------------------------------------------------------------
Write-Output "[2/7] building runtime ..."
$rtObj = Join-Path $runDir "ivyrt.obj"
$code = Invoke-Separate $clang @("-c", (Join-Path $libDir "ivyrt.c"),
                                 "-I", $libDir, "-o", $rtObj) "" `
             (Join-Path $runDir "build_rt.out") (Join-Path $runDir "build_rt.err")
if ($code -ne 0 -or -not (Test-Path -LiteralPath $rtObj)) {
    Write-Output ("BUILD FAILED (ivyrt, exit " + $code + ")")
    Write-Output (Show-Text (Read-Bytes (Join-Path $runDir "build_rt.err")))
    exit 1
}

# ---------------------------------------------------------------------------
# [3/7] Link the real driver and the three differential drivers.
# ---------------------------------------------------------------------------
Write-Output "[3/7] linking drivers ..."
function Link-Exe([string]$name, [string[]]$objs) {
    $exe = Join-Path $runDir $name
    $argList = @($objs) + @($rtObj, "-o", $exe)
    $code = Invoke-Separate $clang $argList "" `
                 (Join-Path $runDir ("link_" + $name + ".out")) `
                 (Join-Path $runDir ("link_" + $name + ".err"))
    if ($code -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
        Write-Output ("BUILD FAILED (link " + $name + ", exit " + $code + ")")
        Write-Output (Show-Text (Read-Bytes (Join-Path $runDir ("link_" + $name + ".err"))))
        exit 1
    }
    return $exe
}

$mainExe = Link-Exe "main.exe" @($mainObj, $importsObj, $cliObj, $lexerObj, $ppObj,
                                 $tokenObj, $diagObj)
$lexDrive = Link-Exe "lexdrive.exe" @($lexDriveObj, $lexerObj, $tokenObj, $diagObj)
$ppDrive = Link-Exe "ppdrive.exe" @($ppDriveObj, $ppObj, $lexerObj, $tokenObj, $diagObj)
$astDrive = Link-Exe "astdrive.exe" @($astDriveObj, $parserObj, $astObj, $ppObj,
                                      $lexerObj, $tokenObj, $diagObj)
Write-Output ("      scratch: " + $runDir)

# ---------------------------------------------------------------------------
# [4/7] Fixtures: preprocessor diagnostics in a scratch tree, plus the corpus.
# ---------------------------------------------------------------------------
Write-Output "[4/7] writing fixtures ..."
$fx = Join-Path $runDir "fx"
$qDir = Join-Path $fx "dir with space"
$qDeep = Join-Path $qDir "sub\deeper"
foreach ($d in @($fx, $qDir, $qDeep)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

# `Fail = $true` means the oracle exits 1 right after the preprocessor, so the
# real driver (`main.exe`) can be compared as well; warnings-only fixtures are
# only used for the stdout comparison (the H7.2 driver stops with exit 3 after
# a successful preprocess).
$ppFixtures = @(
    @{ Name = "error_directive";   Fail = $true;  Lines = @("#error stop here") },
    @{ Name = "include_ivy";       Fail = $true;  Lines = @("#include <stdio.h>") },
    @{ Name = "pragma_unknown";    Fail = $true;  Lines = @("#pragma foo") },
    @{ Name = "endif_orphan";      Fail = $true;  Lines = @("#endif") },
    @{ Name = "else_orphan";       Fail = $true;  Lines = @("#else") },
    @{ Name = "elif_orphan";       Fail = $true;  Lines = @("#elif 1") },
    @{ Name = "elif_after_else";   Fail = $true;  Lines = @("#if 0", "#else", "#elif 1", "#endif") },
    @{ Name = "define_bad";        Fail = $true;  Lines = @("#define 123") },
    @{ Name = "macro_arity";       Fail = $true;  Lines = @("#define F(a,b) a", "F(1)") },
    @{ Name = "line_bad";          Fail = $true;  Lines = @("#line abc") },
    @{ Name = "line_file_only";    Fail = $true;  Lines = @('#line "x y.ivy"', "#error e") },
    @{ Name = "line_remap";        Fail = $true;  Lines = @('#line 100 "virt.ivy"', "#error boom") },
    @{ Name = "if_unterm";         Fail = $true;  Lines = @("#if 1", "int32 x;") },
    @{ Name = "ifdef_unterm";      Fail = $true;  Lines = @("#ifdef FOO") },
    @{ Name = "nested_if";         Fail = $true;  Lines = @("#if 1", "#if 1", "#else", "#endif") },
    @{ Name = "multi_error";       Fail = $true;  Lines = @("#error first", "#error second") },
    @{ Name = "warning_and_error"; Fail = $true;  Lines = @("#warning w1", "#error e1") },
    @{ Name = "duplicate_else";    Fail = $true;  Lines = @("#if 0", "#else", "#else", "#endif") },
    @{ Name = "defined_no_name";   Fail = $true;  Lines = @("#if defined") },
    @{ Name = "defined_no_paren";  Fail = $true;  Lines = @("#if defined(FOO") },
    @{ Name = "trailing_tokens";   Fail = $true;  Lines = @("#if 1 1") },
    @{ Name = "div_zero";          Fail = $true;  Lines = @("#if 1 / 0") },
    @{ Name = "mod_zero";          Fail = $true;  Lines = @("#if 1 % 0") },
    @{ Name = "bad_expr";          Fail = $true;  Lines = @("#if )") },
    @{ Name = "ternary_no_colon";  Fail = $true;  Lines = @("#if 1 ? 2") },
    @{ Name = "paste_invalid";     Fail = $true;  Lines = @("#define P(a,b) a##b", "P(1,+)") },
    @{ Name = "paste_no_left";     Fail = $true;  Lines = @("#define N ## b", "N") },
    @{ Name = "macro_unterm_params"; Fail = $true; Lines = @("#define F(a") },
    @{ Name = "macro_bad_param";   Fail = $true;  Lines = @("#define F(a 1) a", "F(1)") },
    @{ Name = "variadic_arity";    Fail = $true;  Lines = @("#define V(a,...) a", "V()") },
    @{ Name = "undef_predefined";  Fail = $true;  Lines = @("#undef __LINE__") },
    @{ Name = "redefine_predefined"; Fail = $true; Lines = @("#define __LINE__ 5") },
    @{ Name = "line_positive";     Fail = $true;  Lines = @("#line 0") },
    @{ Name = "warning_directive"; Fail = $false; Lines = @("#warning hey", "int32 x;") },
    @{ Name = "undef_missing";     Fail = $false; Lines = @("#undef NOPE") }
)
foreach ($p in $ppFixtures) { Write-Lines (Join-Path $fx ($p.Name + ".ivy")) $p.Lines }

# Quoting fixtures: a lexer error (path printed through
# `std::filesystem::path` operator<< = `std::quoted`) at paths with a space,
# non-ASCII bytes and deep nesting, plus a missing file.
Write-Lines (Join-Path $qDir "loi thuong.ivy") @("@")
Write-Lines (Join-Path $qDir "lỗi.ivy") @("@")
Write-Lines (Join-Path $qDeep "err.ivy") @("@")

$lexErrDir = Join-Path $repo "selfhost\corpus\lexer\err"
$lexErrFiles = @(Get-ChildItem -LiteralPath $lexErrDir -Filter *.ivy -File | Sort-Object Name)
$astErrDir = Join-Path $repo "selfhost\corpus\parser\err"
$astErrFiles = @(Get-ChildItem -LiteralPath $astErrDir -Filter *.ivy -File | Sort-Object Name)
if ($lexErrFiles.Count -eq 0) { Fail ("no lexer error corpus in " + $lexErrDir) }
if ($astErrFiles.Count -eq 0) { Fail ("no parser error corpus in " + $astErrDir) }

# ---------------------------------------------------------------------------
# [5/7] app-stderr: `main.exe <file>` vs `ivy <file>`, raw bytes.
# ---------------------------------------------------------------------------
Write-Output "[5/7] app driver (stderr) ..."

function Compare-App([string]$name, [string]$file, [string]$wd) {
    $o = Run-Capture $ivy @($file) $wd
    $s = Run-Capture $mainExe @($file) $wd
    $outOk = Test-BytesEqual $o.Out $s.Out
    $errOk = Test-BytesEqual $o.Err $s.Err
    if ($o.Code -eq $s.Code -and $outOk -and $errOk) {
        Add-Pass $name ("exit=" + $s.Code + " err=" + $s.Err.Length + "B")
        return
    }
    $d = @("oracle=" + $o.Code + " self=" + $s.Code)
    if (-not $outOk) {
        $d += ("stdout diff at byte " + (First-Diff $o.Out $s.Out))
        $d += ("oracle stdout: " + (Show-Text $o.Out))
        $d += ("self stdout:   " + (Show-Text $s.Out))
    }
    if (-not $errOk) {
        $d += ("stderr diff at byte " + (First-Diff $o.Err $s.Err))
        $d += ("oracle stderr: " + (Show-Text $o.Err))
        $d += ("self stderr:   " + (Show-Text $s.Err))
    }
    Add-Fail $name $d
}

foreach ($f in $lexErrFiles) {
    Compare-App ("lex-" + $f.BaseName) ("selfhost\corpus\lexer\err\" + $f.Name) $repo
}
Compare-App "lex-absolute" (Join-Path $lexErrDir $lexErrFiles[0].Name) $repo
foreach ($p in $ppFixtures) {
    if ($p.Fail) { Compare-App ("pp-" + $p.Name) ($p.Name + ".ivy") $fx }
}
Compare-App "quote-space" "dir with space\loi thuong.ivy" $fx
Compare-App "quote-non-ascii" "dir with space\lỗi.ivy" $fx
Compare-App "quote-deep" "dir with space\sub\deeper\err.ivy" $fx
Compare-App "quote-missing" "dir with space\missing file.ivy" $fx

# ---------------------------------------------------------------------------
# [6/7] stage-stdout: differential drivers vs the merged oracle output.
# ---------------------------------------------------------------------------
Write-Output "[6/7] differential drivers (stdout) ..."

function Compare-Stage([string]$name, [string]$driver, [string]$oracleArg,
                       [string[]]$files, [string]$wd) {
    $list = Join-Path $runDir ("list_" + $name + ".txt")
    [System.IO.File]::WriteAllLines($list, $files)
    $actOut = Join-Path $runDir ("act_" + $name + ".out")
    $actErr = Join-Path $runDir ("act_" + $name + ".err")
    $code = Invoke-Separate $driver @() $list $actOut $actErr 0 $wd

    $ms = New-Object System.IO.MemoryStream
    $idx = 0
    foreach ($f in $files) {
        $idx = $idx + 1
        $marker = $utf8NoBom.GetBytes("=== " + $f + " ===`n")
        $ms.Write($marker, 0, $marker.Length)
        $one = Join-Path $runDir ("oracle_" + $name + "_" + $idx + ".txt")
        [void](Run-Merged $ivy @($oracleArg, $f) $wd $one)
        $bytes = Read-Bytes $one
        if ($bytes.Length -gt 0) { $ms.Write($bytes, 0, $bytes.Length) }
        if ($bytes.Length -eq 0 -or $bytes[$bytes.Length - 1] -ne 10) { $ms.WriteByte(10) }
    }
    $exp = Convert-Lf $ms.ToArray()
    $act = Convert-Lf (Read-Bytes $actOut)
    $errBytes = Read-Bytes $actErr

    if ($code -eq 0 -and (Test-BytesEqual $exp $act) -and $errBytes.Length -eq 0) {
        Add-Pass $name ($files.Count.ToString() + " files, " + $act.Length + "B")
        return
    }
    $d = @("driver exit=" + $code + " stderr=" + $errBytes.Length + "B" +
           " exp=" + $exp.Length + "B act=" + $act.Length + "B" +
           " firstdiff=" + (First-Diff $exp $act))
    $el = [System.Text.Encoding]::UTF8.GetString($exp) -split "`n"
    $al = [System.Text.Encoding]::UTF8.GetString($act) -split "`n"
    $shown = 0
    for ($i = 0; $i -lt [Math]::Max($el.Count, $al.Count) -and $shown -lt 12; $i++) {
        $x = if ($i -lt $el.Count) { $el[$i] } else { "<EOF>" }
        $y = if ($i -lt $al.Count) { $al[$i] } else { "<EOF>" }
        if ($x -ne $y) {
            $d += ("line " + ($i + 1) + " exp: " + $x)
            $d += ("line " + ($i + 1) + " act: " + $y)
            $shown = $shown + 1
        }
    }
    Add-Fail $name $d
}

$lexAbs = @($lexErrFiles | ForEach-Object { $_.FullName })
$astAbs = @($astErrFiles | ForEach-Object { $_.FullName })
$ppAbs = @($ppFixtures | ForEach-Object { Join-Path $fx ($_.Name + ".ivy") })

Compare-Stage "lex-err" $lexDrive "--lex" $lexAbs $repo
Compare-Stage "pp-diag" $ppDrive "--tokens" $ppAbs $fx
Compare-Stage "pp-lexerror" $ppDrive "--tokens" @($lexAbs[0]) $repo
Compare-Stage "ast-err" $astDrive "--ast" $astAbs $repo
Compare-Stage "ast-lexerror" $astDrive "--ast" @($lexAbs[0]) $repo

# ---------------------------------------------------------------------------
# [7/7] Summary + source-tree guard.
# ---------------------------------------------------------------------------
$total = $pass + $fail
Write-Output ("[7/7] summary: PASS " + $pass + " / " + $total)

$changed = $false
foreach ($d in @($commonDir, $parseDir, $appDir)) {
    $after = Get-SourceNames $d
    $new = @($after | Where-Object { $before[$d] -notcontains $_ })
    $missing = @($before[$d] | Where-Object { $after -notcontains $_ })
    if ($new.Count -ne 0 -or $missing.Count -ne 0) {
        Write-Output ("      source tree changed in " + $d + ": new=[" + ($new -join ", ") +
                      "] missing=[" + ($missing -join ", ") + "]")
        $changed = $true
    }
}
if ($changed) { exit 1 }
if ($fail -ne 0) {
    Write-Output ("      FAIL: " + ($failures -join ", "))
    exit 1
}
Write-Output "PASS: diagnostic text, stream and quoting match the oracle"
exit 0
