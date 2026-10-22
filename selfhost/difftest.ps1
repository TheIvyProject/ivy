# selfhost/difftest.ps1 — feature-scoped differential test for the Ivy self-host port.
#
# Compares a SUBSET of the C++ oracle `ivy --lex` against the Ivy lexer port
# driver (selfhost/parsing/lexdrive.ivy) over a feature corpus. The subset is
# selected by token kind so each H1.x feature is verified in isolation.
#
# H1.1 (identifiers / keywords / lifetime): default -Kinds "identifier,keyword,lifetime".
# H1.2 (numeric literals): -Dir selfhost/corpus/lexer/num -Kinds integer,float
# H1.3 (string / char / raw): -Dir selfhost/corpus/lexer/str -Kinds string,char
# H1.4 (operators / punctuation): -Dir selfhost/corpus/lexer/op -Kinds all
# H1.5 (comment / whitespace / line): -Dir selfhost/corpus/lexer/trivia -Kinds all
# H1.6 (error cases): -Dir selfhost/corpus/lexer/err -Kinds all
#   ("all" compares the entire token stream; needed because the comma operator's
#    kind name is a comma, which the comma-separated -Kinds list cannot express,
#    and because comments/whitespace emit no tokens of their own. For H1.6 the
#    oracle prints only diagnostics, so "all" also keeps those lines.)
#
# Usage (from repo root):
#   powershell -File selfhost/difftest.ps1
#   powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/lexer/ident -Kinds identifier,keyword,lifetime
#   powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/lexer/num -Kinds integer,float
#   powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/lexer/str -Kinds string,char
#   powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/lexer/op -Kinds all
#   powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/lexer/trivia -Kinds all
#   powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/lexer/err -Kinds all
#
# Preprocessor features use the post-processing oracle (`--tokens`) and the
# pp driver:
#   H2.1 (object-like #define): -Dir selfhost/corpus/pp/define_obj -Kinds all
#     -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive
#   H2.2 (function-like + variadic #define): -Dir selfhost/corpus/pp/define_fn
#     -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive
#   H2.3 (conditionals #if/#ifdef/#elif/#else/#undef): -Dir selfhost/corpus/pp/cond
#     -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive
#   H2.4 (# stringify / ## token paste): -Dir selfhost/corpus/pp/expand
#     -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive
#   H2.5 (predefined macros + #pragma + pp diagnostics): -Dir selfhost/corpus/pp/predef
#     -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive
#   H2.6 (#line / #error / #warning): -Dir selfhost/corpus/pp/line
#     -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive
#   H2.7 (reject #include in .ivy): -Dir selfhost/corpus/pp/no_include
#     -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive
#
# Parser features use the AST oracle (`--ast`) and the ast driver:
#   H3.1 (declarations: func def/proto, extern "C", constexpr, fn trailing,
#     variadic, unnamed params, lifetime<>, cnumber c-style types, ptr returns):
#     -Dir selfhost/corpus/parser/decl -Kinds all -Oracle --ast
#     -Modules token,lexer,preprocessor,ast,parser -Driver astdrive
#   H3.2 (expressions + precedence + minimal statements; no declarations):
#     -Dir selfhost/corpus/parser/expr -Kinds all -Oracle --ast
#     -Modules token,lexer,preprocessor,ast,parser -Driver astdrive
#   H3.3 (statements / control flow: decl, if/else, while, do-while, for,
#     switch/case/default, nextcase, break/continue, unsafe, nested blocks):
#     -Dir selfhost/corpus/parser/stmt -Kinds all -Oracle --ast
#     -Modules token,lexer,preprocessor,ast,parser -Driver astdrive
#   H3.4 (struct/enum/class: fields, methods, ctor/dtor, operator, virtual/
#     override, access specifiers, base list, enum/enum class/underlying type,
#     using alias, user-defined types in signatures/bodies):
#     -Dir selfhost/corpus/parser/aggregate -Kinds all -Oracle --ast
#     -Modules token,lexer,preprocessor,ast,parser -Driver astdrive
#   H3.5 (template / concept: template function/struct/class, non-type and
#     variadic params, constrained params, `concept Name = requires(...){...}`):
#     -Dir selfhost/corpus/parser/template -Kinds all -Oracle --ast
#     -Modules token,lexer,preprocessor,ast,parser -Driver astdrive
#   H3.6 (module / import / import cpp: `module NAME`/`export module NAME`,
#     `export <decl>`, `import cpp <...>`/`"..."`, `import c`, `import name`):
#     -Dir selfhost/corpus/parser/module -Kinds all -Oracle --ast
#     -Modules token,lexer,preprocessor,ast,parser -Driver astdrive
#   H3.7 (parser error cases E2/E3/E4: `[[ivy::unsafe]]` removal, `_t` suffix
#     removal, `#include` rejection, attribute namespace/position validation):
#     -Dir selfhost/corpus/parser/err -Kinds all -Oracle --ast
#     -Modules token,lexer,preprocessor,ast,parser -Driver astdrive
#
# HIR features use the HIR oracle (`--hir`) and the hir driver:
#   H4.1 (name resolution: locals/shadowing, free/method calls, `this`, struct
#     fields `.`/`->`, enums scoped/unscoped, using aliases, and the related
#     diagnostics):
#     -Dir selfhost/corpus/hir/name -Kinds all -Oracle --hir
#     -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder
#     -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir
#     -Driver hirdrive
#   H4.2 (type checking: literal types, the primitive type universe, integer
#     promotions / usual arithmetic conversions, unary and binary operator
#     rules, assignment and initialization compatibility, implicit conversions,
#     call argument checking and overload resolution, control-flow conditions,
#     return statements, pointers/arrays, structs/enums/aliases and the
#     matching diagnostics):
#     -Dir selfhost/corpus/hir/type -Kinds all -Oracle --hir
#     -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder
#     -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir
#     -Driver hirdrive
#   H4.3 (constexpr folding: `sizeof`/`alignof` folded to integer literals
#     with the struct layout, constexpr calls folded to literals, `if
#     constexpr` keeping only the taken branch, and the `switch` statement
#     with its diagnostics):
#     -Dir selfhost/corpus/hir/constexpr -Kinds all -Oracle --hir
#     -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder
#     -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir
#     -Driver hirdrive
#   H4.4 (template instantiation: function templates, explicit and deduced
#     template arguments, variadic packs, overload/template precedence,
#     class/struct templates, concepts and the related diagnostics):
#     -Dir selfhost/corpus/hir/template -Kinds all -Oracle --hir
#     -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder
#     -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir
#     -Driver hirdrive
#   H4.5 (borrow / lifetime check - the HIR-stage lifetime *annotations*:
#     the native `lifetime<$a>` / `$a` syntax, the legacy
#     `[[ivy::lt_def(...)]]` / `[[ivy::lt_ret(...)]]` / `[[ivy::lt(...)]]`
#     attributes, their precedence, the ten HIR-stage lifetime diagnostics
#     and the `lifetimes:` / `return lifetime:` / ` [lt:X]` dump lines):
#     -Dir selfhost/corpus/hir/borrow -Kinds all -Oracle --hir
#     -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder
#     -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir
#     -Driver hirdrive
#     (the corpus must stay MIR-clean: `ivy --hir` runs the whole pipeline,
#      so a MIR-stage diagnostic would be printed after the HIR dump and
#      could never match the HIR-only port)
#   H4.6 (move / use-after-move - the HIR-stage move tracking: the `move()`
#     builtin recognition rules (the callee is the bare name `move`, exactly
#     one argument, the argument is an identifier), the move state of every
#     binding, the use-after-move and the already-moved-out diagnostics, the
#     assignment-LHS bypass, the re-initialisation by a plain `=`, and the
#     builtin-vs-user-defined precedence):
#     -Dir selfhost/corpus/hir/move -Kinds all -Oracle --hir
#     -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder
#     -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir
#     -Driver hirdrive
#     (the corpus must stay MIR-clean: `ivy --hir` runs the whole pipeline,
#      so a MIR-stage diagnostic would be printed after the HIR dump and
#      could never match the HIR-only port)
#
# MIR features use the MIR oracle (`--mir`) and the mir driver:
#   H5.1 (CFG lowering - HIR -> MIR: the MIR model, the basic blocks, the
#     statement and the expression lowering, the compound-assignment
#     expansion, the lifetime *data* and the `--mir` dump format):
#     -Dir selfhost/corpus/mir/cfg -Kinds all -Oracle --mir
#     -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder,mir,mir_builder
#     -ModuleDirs selfhost/parsing,selfhost/hir,selfhost/mir -DriverDir selfhost/mir
#     -Driver mirdrive
#     (the corpus must stay MIR-clean: `ivy --mir` prints NO dump when the MIR
#      stage reports a diagnostic, so a fixture with a MIR-stage diagnostic
#      belongs to the H5.2 corpus, not to H5.1)
#
# The comparison is byte-exact: both sides are captured as raw bytes, the
# lines are filtered at byte level and the two byte arrays are compared.
# Only the line terminator is normalized (CR LF -> LF).  An earlier version
# wrote both sides with ASCII encoding, which folded every non-ASCII byte
# to '?' on BOTH sides and therefore hid byte-level differences in the
# non-ASCII diagnostics (e.g. the em dash U+2014 in the H4.5 lifetime
# diagnostics).
#
# Exit 0 = identical, 1 = mismatch or build failure.

param(
    [string]$Dir       = "selfhost/corpus/lexer/ident",
    [string]$Kinds     = "identifier,keyword,lifetime",
    [string]$Oracle    = "--lex",
    [string]$Modules   = "token,lexer",
    [string]$ModuleDirs = "",
    [string]$DriverDir  = "",
    [string]$Driver     = "lexdrive"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo

$ivy = Join-Path $repo "build\Release\ivy.exe"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "ivydiff_selfhost"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$port     = Join-Path $tmp "$Driver.exe"
$list     = Join-Path $tmp "files.txt"
$one      = Join-Path $tmp "one.txt"
$actRaw   = Join-Path $tmp "actual_raw.txt"
$expected = Join-Path $tmp "expected.txt"
$actual   = Join-Path $tmp "actual.txt"

$sh = "selfhost\parsing"

if ($ModuleDirs -eq "") { $ModuleDirs = "selfhost/parsing" }
if ($DriverDir -eq "") { $DriverDir = "selfhost/parsing" }
$mds = @($ModuleDirs.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
$inc = (($mds | ForEach-Object { "-I `"$_`"" }) -join " ")

# 1. Build the port modules + driver with the C++ compiler (freshness).
# Modules are compiled in the listed order so each dependency's .ivm exists
# before its dependents.
Write-Output "[1/4] building selfhost modules ..."
foreach ($m in $Modules.Split(",")) {
    $m = $m.Trim()
    if ($m -eq "") { continue }
    $src = $null
    foreach ($md in $mds) {
        $cand = Join-Path $md "$m.ivy"
        if (Test-Path $cand) { $src = $cand; break }
    }
    if ($src -eq $null) { Write-Output "BUILD FAILED (${m}: source not found)"; exit 1 }
    $obj = [System.IO.Path]::ChangeExtension($src, ".obj")
    cmd /c "`"$ivy`" $inc `"$src`" -c -o `"$obj`" > NUL 2>&1"
    if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED ($m)"; exit 1 }
}
$dsrc = Join-Path $DriverDir "$Driver.ivy"
cmd /c "`"$ivy`" $inc `"$dsrc`" -o `"$port`" > NUL 2>&1"
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED ($Driver)"; exit 1 }

# 2. Corpus file list (relative, forward slashes, sorted).
Write-Output "[2/4] collecting corpus ..."
$files = @(Get-ChildItem "$Dir/*.ivy" |
    ForEach-Object { $_.FullName.Substring($repo.Length + 1) -replace "\\", "/" } |
    Sort-Object -Unique)
if ($files.Count -eq 0) { Write-Output "no corpus files in $Dir"; exit 1 }
$files | Set-Content $list -Encoding ascii
Write-Output ("      files: " + $files.Count)

$kindSet = @{}
$allKinds = ($Kinds.Trim().ToLowerInvariant() -eq "all")
if (-not $allKinds) {
    foreach ($k in $Kinds.Split(",")) { $kindSet[$k.Trim()] = $true }
}

# Keep marker lines and token lines whose kind is in the selected set.
# Lexeme columns never contain a tab for these kinds, so field 1 is the kind.
# "all" keeps every token line (including operators, whose kind name can be a
# comma and therefore cannot be listed in the comma-separated -Kinds set).
#
# The comparison is BYTE-EXACT: the lines are kept as raw byte arrays and
# compared as such, so a difference in a non-ASCII byte (e.g. the em dash
# U+2014 in the lifetime diagnostics) cannot hide behind an encoding
# round-trip.  Only the line terminator is normalized (CR LF -> LF), which
# is a platform artifact and not a content difference.
function Get-ByteLines([byte[]]$bytes) {
    $lines = New-Object System.Collections.Generic.List[byte[]]
    $start = 0
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -eq 10) {
            $end = $i
            if ($end -gt $start -and $bytes[$end - 1] -eq 13) { $end = $end - 1 }
            $seg = New-Object byte[] ($end - $start)
            if ($end -gt $start) { [Array]::Copy($bytes, $start, $seg, 0, $end - $start) }
            $lines.Add($seg)
            $start = $i + 1
        }
    }
    if ($start -lt $bytes.Length) {
        $seg = New-Object byte[] ($bytes.Length - $start)
        [Array]::Copy($bytes, $start, $seg, 0, $bytes.Length - $start)
        $lines.Add($seg)
    }
    # The comma operator keeps the list as a single pipeline object (otherwise
    # PowerShell unrolls the nested byte arrays into individual bytes).
    return ,$lines.ToArray()
}

function Test-KeepBytes([byte[]]$ln, $kindSet, [bool]$allKinds) {
    if ($ln.Length -eq 0) { return $false }
    # "=== " marker
    if ($ln.Length -ge 4 -and $ln[0] -eq 61 -and $ln[1] -eq 61 -and $ln[2] -eq 61 -and $ln[3] -eq 32) {
        return $true
    }
    if ($allKinds) { return $true }
    # field 1 (between the first and the second tab) is the kind
    $t1 = -1
    for ($i = 0; $i -lt $ln.Length; $i++) { if ($ln[$i] -eq 9) { $t1 = $i; break } }
    if ($t1 -lt 0) { return $false }
    $t2 = -1
    for ($i = $t1 + 1; $i -lt $ln.Length; $i++) { if ($ln[$i] -eq 9) { $t2 = $i; break } }
    if ($t2 -lt 0) { return $false }
    $kind = [System.Text.Encoding]::ASCII.GetString($ln, $t1 + 1, $t2 - $t1 - 1)
    return $kindSet.ContainsKey($kind)
}

function Join-ByteLines($lines) {
    $out = New-Object System.Collections.Generic.List[byte]
    foreach ($ln in $lines) {
        foreach ($b in $ln) { $out.Add($b) }
        $out.Add(10)
    }
    return ,$out.ToArray()
}

# 3a. Expected: C++ `ivy --lex` per file, filtered, marker prepended in PS.
# (`--lex` has no trailing newline after its `eof` line, so per-file capture
# avoids the next marker gluing onto it.)
Write-Output "[3/4] running C++ oracle + Ivy port ..."
$exp = New-Object System.Collections.Generic.List[byte[]]
foreach ($f in $files) {
    cmd /c "`"$ivy`" $inc $Oracle `"$f`" > `"$one`" 2>&1"
    $exp.Add([System.Text.Encoding]::ASCII.GetBytes("=== $f ==="))
    foreach ($ln in (Get-ByteLines ([System.IO.File]::ReadAllBytes($one)))) {
        if (Test-KeepBytes $ln $kindSet $allKinds) { $exp.Add($ln) }
    }
}
$expBytes = Join-ByteLines $exp
[System.IO.File]::WriteAllBytes($expected, $expBytes)

# 3b. Actual: port over stdin (one file per line).
cmd /c "(type `"$list`" | `"$port`") > `"$actRaw`" 2>&1"
$act = New-Object System.Collections.Generic.List[byte[]]
foreach ($ln in (Get-ByteLines ([System.IO.File]::ReadAllBytes($actRaw)))) {
    if (Test-KeepBytes $ln $kindSet $allKinds) { $act.Add($ln) }
}
$actBytes = Join-ByteLines $act
[System.IO.File]::WriteAllBytes($actual, $actBytes)

# 4. Compare (byte-exact).
Write-Output "[4/4] comparing (kinds: $Kinds) ..."
$same = ($expBytes.Length -eq $actBytes.Length)
if ($same) {
    for ($i = 0; $i -lt $expBytes.Length; $i++) {
        if ($expBytes[$i] -ne $actBytes[$i]) { $same = $false; break }
    }
}
if ($same) {
    Write-Output ("PASS: identical (filtered) output on " + $files.Count + " files")
    exit 0
}
Write-Output "FAIL: outputs differ"
$e = [System.Text.Encoding]::UTF8.GetString($expBytes)
$a = [System.Text.Encoding]::UTF8.GetString($actBytes)
$expLines = $e -split "`n"
$actLines = $a -split "`n"
$shown = 0
for ($i = 0; $i -lt [Math]::Max($expLines.Count, $actLines.Count) -and $shown -lt 30; $i++) {
    $le = if ($i -lt $expLines.Count) { $expLines[$i] } else { "<EOF>" }
    $la = if ($i -lt $actLines.Count) { $actLines[$i] } else { "<EOF>" }
    if ($le -ne $la) {
        Write-Output ("line " + ($i + 1) + " exp: " + $le)
        Write-Output ("line " + ($i + 1) + " act: " + $la)
        $shown++
    }
}
exit 1
