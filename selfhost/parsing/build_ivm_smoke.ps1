# selfhost/parsing/build_ivm_smoke.ps1 - H7.3 `.ivm` read/write harness.
#
# Checks `selfhost/parsing/module_io.ivy` (the port of `src/parsing/module_io.cpp`)
# through `selfhost/parsing/ivmdrive.ivy`, a small driver with two modes:
#
#   ivmdrive --write <src.ivy> <out.ivm>   lex+preprocess+parse the source and
#                                          write the module interface
#   ivmdrive --dump <in.ivm>               read an interface and print the
#                                          serialized form of the parsed unit
#
#   [1] write cases - for each fixture module the oracle is run with
#       `-c -o <dir>/<name>.obj` (which writes `<dir>/<name>.ivm`); the
#       selfhost-written interface must be BYTE-IDENTICAL. A consumer that
#       imports the selfhost-written interface must compile with the oracle,
#       which proves the file is a usable interface and not just equal bytes.
#
#   [2] dump cases - `--dump` of the oracle interface must equal the interface
#       text with the reader's two documented normalizations applied: functions
#       are qualified with the module name (unless `extern "C"` or already
#       qualified) and enumerator values are dropped (`=N` -> `=-1`). Every
#       line of the dump is produced by re-serializing the parsed unit, so this
#       checks every field of the reader.
#
#   [3] real interfaces - the same dump check for every `std/*.ivm` and
#       `examples/**/*.ivm` in the repository (read-only). These files contain
#       template arguments, references, `const`, pointers, arrays, operators
#       and template copy-constructors.
#
#   [4] error cases - an empty file, a magic-only file, a wrong version byte
#       and a garbage file are compared byte-exactly against the oracle (which
#       fails while importing the same file). A missing file, an unwritable
#       output path and a source without `export module` are checked against
#       the messages in `src/parsing/module_io.cpp` and the driver.
#
# Everything is built under a fresh per-run directory in TEMP; the repository
# working tree is never written to. Only clang is needed (no LLVM libs).
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/parsing/build_ivm_smoke.ps1
#   powershell -NoProfile -File selfhost/parsing/build_ivm_smoke.ps1 -TempRoot C:\scratch\h73
#
# Exit 0 iff every case matches.

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
$parseDir = Join-Path $repo "selfhost\parsing"
$stdDir = Join-Path $repo "std"
$libDir = Join-Path $repo "lib"

function Fail([string]$message) {
    Write-Output ("FAIL: " + $message)
    exit 1
}

if (-not (Test-Path -LiteralPath $ivy)) { Fail ("oracle not found: " + $ivy) }
if (-not (Test-Path -LiteralPath $clang)) { Fail ("clang not found: " + $clang) }

# Guard: the build must not drop .ivm/.obj/.exe next to the sources.
$beforeParse = @(Get-ChildItem -LiteralPath $parseDir -File | ForEach-Object { $_.Name } |
                 Sort-Object)
$beforeRepo = @(Get-ChildItem -LiteralPath $repo -File | ForEach-Object { $_.Name } |
                Sort-Object)

$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$runDir = Join-Path $tempBase ("h73ivm_" + $stamp + "_" + $PID)
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

Write-Output "[1/5] building modules ..."
$incs = @("-I", $runDir, "-I", $stdDir)
$diagObj = Join-Path $runDir "diagnostic.obj"
$tokenObj = Join-Path $runDir "token.obj"
$lexerObj = Join-Path $runDir "lexer.obj"
$ppObj = Join-Path $runDir "preprocessor.obj"
$astObj = Join-Path $runDir "ast.obj"
$parserObj = Join-Path $runDir "parser.obj"
$moduleObj = Join-Path $runDir "module_io.obj"
$driveObj = Join-Path $runDir "ivmdrive.obj"

Build-Mod (Join-Path $repo "selfhost\common\diagnostic.ivy") $diagObj $incs
Build-Mod (Join-Path $parseDir "token.ivy") $tokenObj $incs
Build-Mod (Join-Path $parseDir "lexer.ivy") $lexerObj $incs
Build-Mod (Join-Path $parseDir "preprocessor.ivy") $ppObj $incs
Build-Mod (Join-Path $parseDir "ast.ivy") $astObj $incs
Build-Mod (Join-Path $parseDir "parser.ivy") $parserObj $incs
Build-Mod (Join-Path $parseDir "module_io.ivy") $moduleObj $incs
Build-Mod (Join-Path $parseDir "ivmdrive.ivy") $driveObj $incs

Write-Output "[2/5] building runtime and linking the driver ..."
$rtObj = Join-Path $runDir "rt.obj"
$code = Invoke-Separate $ivy @("-c", (Join-Path $repo "std\rt.ivy"),
                               "-I", (Join-Path $repo "std"), "-o", $rtObj) "" `
             (Join-Path $runDir "build_rt.out") (Join-Path $runDir "build_rt.err")
if ($code -ne 0 -or -not (Test-Path -LiteralPath $rtObj)) {
    Write-Output ("BUILD FAILED (rt, exit " + $code + ")")
    Write-Output (Show-Text (Read-Bytes (Join-Path $runDir "build_rt.err")))
    exit 1
}

$exe = Join-Path $runDir "ivmdrive.exe"
$code = Invoke-Separate $clang @($driveObj, $moduleObj, $parserObj, $astObj, $ppObj,
                                 $lexerObj, $tokenObj, $diagObj, $rtObj, "-o", $exe) "" `
             (Join-Path $runDir "link.out") (Join-Path $runDir "link.err")
if ($code -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
    Write-Output ("BUILD FAILED (link, exit " + $code + ")")
    Write-Output (Show-Text (Read-Bytes (Join-Path $runDir "link.err")))
    exit 1
}
Write-Output ("      scratch: " + $runDir)

# ---------------------------------------------------------------------------
# [3/5] Fixtures: write byte-identical, dump, consumer compile.
# ---------------------------------------------------------------------------
Write-Output "[3/5] write/dump fixtures ..."

# Expected dump text: the interface file with the reader's normalizations.
#   - `func <name> ... <is_extern_c> ...`: the reader qualifies a non-extern-C
#     function with the module name when the name is still bare.
#   - `enum ... <name>=<value>`: the reader drops enumerator values.
function New-IvmExpect([string]$path) {
    $text = [System.IO.File]::ReadAllText($path)
    $lines = $text -split "`n"
    $out = New-Object System.Collections.Generic.List[string]
    $moduleName = ""
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($i -eq 0) { continue }
        if ($line -eq "" -and $i -eq ($lines.Count - 1)) { continue }
        if ($line.StartsWith("module ")) {
            $moduleName = $line.Substring(7)
            $out.Add($line)
            continue
        }
        if ($line.StartsWith("func ")) {
            $t = $line.Split(' ')
            $np = [int]$t[3]
            if ($moduleName -ne "" -and $t[4 + $np] -eq "0" -and -not $t[1].Contains("::")) {
                $t[1] = $moduleName + "::" + $t[1]
            }
            $out.Add(($t -join ' '))
            continue
        }
        if ($line.StartsWith("enum ")) {
            $t = $line.Split(' ')
            $n = [int]$t[4]
            for ($k = 0; $k -lt $n; $k++) {
                $eq = $t[5 + $k].IndexOf('=')
                if ($eq -ge 0) { $t[5 + $k] = $t[5 + $k].Substring(0, $eq) + "=-1" }
            }
            $out.Add(($t -join ' '))
            continue
        }
        $out.Add($line)
    }
    return (($out -join "`r`n") + "`r`n")
}

$fxDir = Join-Path $runDir "fx"
$selfDir = Join-Path $runDir "selfmods"
New-Item -ItemType Directory -Force -Path $fxDir | Out-Null
New-Item -ItemType Directory -Force -Path $selfDir | Out-Null

$fixtures = @(
    @{ Name = "f_empty"; Lines = @(
        "export module f_empty;"
    ) },
    @{ Name = "f_funcs"; Lines = @(
        "#pragma ivy cnumber",
        "export module f_funcs;",
        "",
        "extern ""C"" int32 ext_puts(const char* s);",
        "extern ""C"" int32 ext_printf(const char* fmt, ...);",
        "",
        "export fn add(int32 a, int32 b) -> int32 {",
        "    return a + b;",
        "}",
        "",
        "export constexpr fn sq(int32 n) -> int32 {",
        "    return n * n;",
        "}",
        "",
        "export consteval fn twice(int32 n) -> int32 {",
        "    return n * 2;",
        "}",
        "",
        "fn hidden(int32 n) -> int32 {",
        "    return n;",
        "}"
    ) },
    @{ Name = "f_structs"; Lines = @(
        "#pragma ivy cnumber",
        "export module f_structs;",
        "",
        "export struct Base {",
        "    int32 v;",
        "};",
        "",
        "export struct Widget : public Base {",
        "    int32 w;",
        "    int32 h;",
        "",
        "    Widget(int32 a, int32 b) {",
        "        this.w = a;",
        "        this.h = b;",
        "    }",
        "",
        "    ~Widget() {",
        "    }",
        "",
        "    int32 area() {",
        "        return this.w * this.h;",
        "    }",
        "",
        "    int32 operator+(int32 k) {",
        "        return this.w + k;",
        "    }",
        "};",
        "",
        "export struct Holder {",
        "    char* name;",
        "    uint64 count;",
        "};"
    ) },
    @{ Name = "f_enums"; Lines = @(
        "export module f_enums;",
        "",
        "export enum Color {",
        "    Red,",
        "    Green = 5,",
        "    Blue = 7",
        "};",
        "",
        "export enum class Mode : uint8 {",
        "    Off,",
        "    On = 3",
        "};",
        "",
        "export enum class Flags : uint64 {",
        "    None = 0,",
        "    All = 15",
        "};",
        "",
        "export enum Signed : int32 {",
        "    Neg = -3,",
        "    Zero = 0",
        "};"
    ) },
    @{ Name = "f_tpl"; Lines = @(
        "export module f_tpl;",
        "",
        "export template <typename T>",
        "struct Box {",
        "    T v;",
        "    T get() {",
        "        return this.v;",
        "    }",
        "};",
        "",
        "export template <typename T>",
        "T ident(T x) {",
        "    return x;",
        "}"
    ) },
    @{ Name = "f_alias"; Lines = @(
        "export module f_alias;",
        "",
        "export using MyInt = int32;",
        "using Hidden = uint8;",
        "",
        "concept Addable = requires(T a, T b) {",
        "    a + b;",
        "};",
        "",
        "concept Comparable = requires(T a, T b) {",
        "    a == b;",
        "    a != b;",
        "};"
    ) },
    @{ Name = "f_mixed"; Lines = @(
        "export module f_mixed;",
        "",
        "export fn add(int32 a, int32 b) -> int32 {",
        "    return a + b;",
        "}",
        "",
        "export struct Point {",
        "    int32 x;",
        "    int32 y;",
        "    int32 sum() {",
        "        return this.x + this.y;",
        "    }",
        "};",
        "",
        "export enum Color {",
        "    Red,",
        "    Green = 5",
        "};",
        "",
        "export using MyInt = int32;",
        "",
        "concept Addable = requires(T a, T b) {",
        "    a + b;",
        "};"
    ) },
    @{ Name = "f_rich"; Lines = @(
        "export module f_rich;",
        "",
        "export struct Base {",
        "    int32 v;",
        "};",
        "",
        "export struct Widget : public Base {",
        "    int32 w;",
        "    int32 h;",
        "",
        "    Widget(int32 a, int32 b) {",
        "        this.w = a;",
        "        this.h = b;",
        "    }",
        "",
        "    ~Widget() {",
        "    }",
        "",
        "    int32 area() {",
        "        return this.w * this.h;",
        "    }",
        "",
        "    int32 operator+(int32 k) {",
        "        return this.w + k;",
        "    }",
        "};",
        "",
        "export enum class Mode : uint8 {",
        "    Off,",
        "    On = 3",
        "};",
        "",
        "export template <typename T>",
        "struct Box {",
        "    T v;",
        "    T get() {",
        "        return this.v;",
        "    }",
        "};",
        "",
        "export template <typename T>",
        "T ident(T x) {",
        "    return x;",
        "}",
        "",
        "export constexpr fn square(int32 n) -> int32 {",
        "    return n * n;",
        "}",
        "",
        "using Alias = Base;",
        "",
        "concept Sized = requires(T a) {",
        "    a;",
        "};"
    ) }
)

foreach ($fx in $fixtures) {
    $name = $fx.Name
    $src = Join-Path $fxDir ($name + ".ivy")
    Write-Lines $src $fx.Lines

    $oracleObj = Join-Path $fxDir ($name + ".obj")
    $r = Run-Capture $ivy @($src, "-c", "-o", $oracleObj) $runDir
    $oracleIvm = Join-Path $fxDir ($name + ".ivm")
    if ($r.Code -ne 0 -or -not (Test-Path -LiteralPath $oracleIvm)) {
        Add-Fail ($name + "-fixture") @(
            ("oracle exit " + $r.Code + " (no interface)"),
            ("stderr: " + (Show-Text $r.Err)))
        continue
    }

    # (a) write: byte-identical interface.
    $selfIvm = Join-Path $selfDir ($name + ".ivm")
    $s = Run-Capture $exe @("--write", $src, $selfIvm) $runDir
    $a = Read-Bytes $oracleIvm
    $b = Read-Bytes $selfIvm
    if ($s.Code -eq 0 -and (Test-BytesEqual $a $b)) {
        Add-Pass ($name + "-write") ("(" + $b.Length + "B identical)")
    } else {
        Add-Fail ($name + "-write") @(
            ("self exit " + $s.Code + ", oracle=" + $a.Length + "B self=" + $b.Length + "B"),
            ("first diff at byte " + (First-Diff $a $b)),
            ("self stderr: " + (Show-Text $s.Err)))
    }

    # (b) dump: re-serialization of the parsed unit.
    $d = Run-Capture $exe @("--dump", $oracleIvm) $runDir
    $expB = $utf8NoBom.GetBytes((New-IvmExpect $oracleIvm))
    if ($d.Code -eq 0 -and (Test-BytesEqual $expB $d.Out) -and $d.Err.Length -eq 0) {
        Add-Pass ($name + "-dump") ("(" + $d.Out.Length + "B)")
    } else {
        Add-Fail ($name + "-dump") @(
            ("self exit " + $d.Code + " stderr=" + (Show-Text $d.Err)),
            ("diff at byte " + (First-Diff $expB $d.Out)),
            ("expected: " + (Show-Text $expB)),
            ("actual:   " + (Show-Text $d.Out)))
    }

    # (c) the selfhost-written interface must be usable by the oracle.
    $cons = Join-Path $runDir ("cons_" + $name + ".ivy")
    Write-Lines $cons @(("import " + $name + ";"), "int32 main() { return 0; }")
    $c = Run-Capture $ivy @($cons, "-I", $selfDir, "-c", "-o",
                            (Join-Path $runDir ("cons_" + $name + ".obj"))) $runDir
    if ($c.Code -eq 0) {
        Add-Pass ($name + "-consumer") "(oracle imported the selfhost .ivm)"
    } else {
        Add-Fail ($name + "-consumer") @(
            ("oracle exit " + $c.Code),
            ("stderr: " + (Show-Text $c.Err)))
    }
}

# ---------------------------------------------------------------------------
# [4/5] Real interfaces from the repository (read-only).
# ---------------------------------------------------------------------------
Write-Output "[4/5] real module interfaces ..."
$realFiles = @()
$realFiles += @(Get-ChildItem -LiteralPath $stdDir -Filter "*.ivm" -File | Sort-Object Name)
$examplesDir = Join-Path $repo "examples"
if (Test-Path -LiteralPath $examplesDir) {
    $realFiles += @(Get-ChildItem -LiteralPath $examplesDir -Filter "*.ivm" -File -Recurse |
                    Sort-Object FullName)
}
foreach ($f in $realFiles) {
    $label = "real-" + $f.Name
    $d = Run-Capture $exe @("--dump", $f.FullName) $runDir
    $expB = $utf8NoBom.GetBytes((New-IvmExpect $f.FullName))
    if ($d.Code -eq 0 -and (Test-BytesEqual $expB $d.Out) -and $d.Err.Length -eq 0) {
        Add-Pass $label ("(" + $d.Out.Length + "B)")
    } else {
        Add-Fail $label @(
            ("self exit " + $d.Code + " stderr=" + (Show-Text $d.Err)),
            ("diff at byte " + (First-Diff $expB $d.Out)),
            ("expected: " + (Show-Text $expB)),
            ("actual:   " + (Show-Text $d.Out)))
    }
}

# ---------------------------------------------------------------------------
# [5/5] Error cases.
# ---------------------------------------------------------------------------
Write-Output "[5/5] error cases ..."
$errDir = Join-Path $runDir "err"
New-Item -ItemType Directory -Force -Path $errDir | Out-Null

# Bad interfaces: the oracle imports the file and fails, so the exit status,
# stdout and stderr are compared as raw bytes with the same path on both sides.
[System.IO.File]::WriteAllBytes((Join-Path $errDir "zero.ivm"), [byte[]]@())
[System.IO.File]::WriteAllText((Join-Path $errDir "trunc.ivm"),
                               ("IVYMOD" + [char]1 + "`n"), $utf8NoBom)
[System.IO.File]::WriteAllBytes((Join-Path $errDir "ver.ivm"),
                                [byte[]](0x49, 0x56, 0x59, 0x4D, 0x4F, 0x44, 0x02, 0x0A))
[System.IO.File]::WriteAllText((Join-Path $errDir "junk.ivm"),
                               "GARBAGE`nmodule junk`n", $utf8NoBom)

foreach ($m in @("zero", "trunc", "ver", "junk")) {
    $label = "bad-" + $m
    $ivmPath = Join-Path $errDir ($m + ".ivm")
    $cons = Join-Path $errDir ("c_" + $m + ".ivy")
    Write-Lines $cons @(("import " + $m + ";"), "int32 main() { return 0; }")
    $o = Run-Capture $ivy @($cons, "-I", $errDir, "-c", "-o",
                            (Join-Path $errDir ("x_" + $m + ".obj"))) $errDir
    $s = Run-Capture $exe @("--dump", $ivmPath) $errDir
    if ($o.Code -eq $s.Code -and (Test-BytesEqual $o.Out $s.Out) -and
        (Test-BytesEqual $o.Err $s.Err)) {
        Add-Pass $label ("(exit " + $s.Code + ", stderr " + $s.Err.Length + "B)")
    } else {
        Add-Fail $label @(
            ("oracle exit " + $o.Code + " self exit " + $s.Code),
            ("oracle stderr: " + (Show-Text $o.Err)),
            ("self stderr:   " + (Show-Text $s.Err)),
            ("oracle stdout: " + (Show-Text $o.Out)),
            ("self stdout:   " + (Show-Text $s.Out)))
    }
}

# Missing file: the message is the one in src/parsing/module_io.cpp:227 (the
# oracle cannot be driven into this state because it resolves before opening).
$missingPath = Join-Path $errDir "no_such.ivm"
$s = Run-Capture $exe @("--dump", $missingPath) $errDir
$expB = $utf8NoBom.GetBytes("ivy: error: cannot read module interface '" +
                            $missingPath + "'`r`n")
if ($s.Code -eq 1 -and $s.Out.Length -eq 0 -and (Test-BytesEqual $expB $s.Err)) {
    Add-Pass "missing-file" "(exit 1, one stderr line)"
} else {
    Add-Fail "missing-file" @(
        ("self exit " + $s.Code),
        ("self stderr: " + (Show-Text $s.Err)))
}

# A source that is not a module interface unit: the driver rejects it before
# writing anything.
$badOut = Join-Path $errDir "no_such_dir\out.ivm"
$plain = Join-Path $errDir "plain.ivy"
Write-Lines $plain @("int32 main() { return 0; }")
$s = Run-Capture $exe @("--write", $plain, $badOut) $errDir
$expB = $utf8NoBom.GetBytes("ivmdrive: source is not a module interface unit`r`n")
if ($s.Code -eq 1 -and $s.Out.Length -eq 0 -and (Test-BytesEqual $expB $s.Err) -and
    -not (Test-Path -LiteralPath $badOut)) {
    Add-Pass "not-a-module" "(exit 1, no output file)"
} else {
    Add-Fail "not-a-module" @(
        ("self exit " + $s.Code),
        ("self stderr: " + (Show-Text $s.Err)))
}

$modSrc = Join-Path $errDir "mod.ivy"
Write-Lines $modSrc @("export module mod;", "export struct S { int32 x; };")
# Unwritable output path: the message is the one in module_io.ivy `write`.
$s = Run-Capture $exe @("--write", $modSrc, $badOut) $errDir
$expB = $utf8NoBom.GetBytes("ivy: error: cannot write module interface to '" +
                            $badOut + "'`r`n")
if ($s.Code -eq 1 -and $s.Out.Length -eq 0 -and (Test-BytesEqual $expB $s.Err) -and
    -not (Test-Path -LiteralPath $badOut)) {
    Add-Pass "unwritable-output" "(exit 1, one stderr line)"
} else {
    Add-Fail "unwritable-output" @(
        ("self exit " + $s.Code),
        ("self stderr: " + (Show-Text $s.Err)))
}

# ---------------------------------------------------------------------------
# Guard: nothing new next to the sources.
# ---------------------------------------------------------------------------
$afterParse = @(Get-ChildItem -LiteralPath $parseDir -File | ForEach-Object { $_.Name } |
                Sort-Object)
$afterRepo = @(Get-ChildItem -LiteralPath $repo -File | ForEach-Object { $_.Name } |
               Sort-Object)
$newFiles = @(@($afterParse | Where-Object { $beforeParse -notcontains $_ }) +
              @($afterRepo | Where-Object { $beforeRepo -notcontains $_ }))
$missingFiles = @(@($beforeParse | Where-Object { $afterParse -notcontains $_ }) +
                  @($beforeRepo | Where-Object { $afterRepo -notcontains $_ }))

$total = $pass + $fail
Write-Output ("summary: PASS " + $pass + " / " + $total + " (" + $fixtures.Count +
              " fixtures x write/dump/consumer, " + $realFiles.Count +
              " real interfaces, 4 bad-interface, 3 driver errors)")
if ($newFiles.Count -ne 0 -or $missingFiles.Count -ne 0) {
    Write-Output ("      source tree changed: new=[" + ($newFiles -join ", ") +
                  "] missing=[" + ($missingFiles -join ", ") + "]")
    exit 1
}
if ($fail -ne 0) {
    Write-Output ("      FAIL: " + ($failures -join ", "))
    exit 1
}
Write-Output "PASS: .ivm read/write matches the oracle"
exit 0
