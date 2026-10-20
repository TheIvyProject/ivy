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
$ascii = New-Object System.Text.ASCIIEncoding

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
function Test-KeepLine([string]$ln, $kindSet, [bool]$allKinds) {
    if ($ln.Length -eq 0) { return $false }
    if ($ln.StartsWith("=== ")) { return $true }
    if ($allKinds) { return $true }
    $f = $ln.Split([char]9)
    return ($f.Length -ge 2 -and $kindSet.ContainsKey($f[1]))
}

# 3a. Expected: C++ `ivy --lex` per file, filtered, marker prepended in PS.
# (`--lex` has no trailing newline after its `eof` line, so per-file capture
# avoids the next marker gluing onto it.)
Write-Output "[3/4] running C++ oracle + Ivy port ..."
$exp = New-Object System.Collections.Generic.List[string]
foreach ($f in $files) {
    cmd /c "`"$ivy`" $inc $Oracle `"$f`" > `"$one`" 2>&1"
    $exp.Add("=== $f ===")
    $txt = ([System.IO.File]::ReadAllText($one)) -replace "`r`n", "`n"
    foreach ($ln in $txt.Split("`n")) { if (Test-KeepLine $ln $kindSet $allKinds) { $exp.Add($ln) } }
}
[System.IO.File]::WriteAllText($expected, (($exp -join "`n") + "`n"), $ascii)

# 3b. Actual: port over stdin (one file per line).
cmd /c "(type `"$list`" | `"$port`") > `"$actRaw`" 2>&1"
$act = New-Object System.Collections.Generic.List[string]
$txt = ([System.IO.File]::ReadAllText($actRaw)) -replace "`r`n", "`n"
foreach ($ln in $txt.Split("`n")) { if (Test-KeepLine $ln $kindSet $allKinds) { $act.Add($ln) } }
[System.IO.File]::WriteAllText($actual, (($act -join "`n") + "`n"), $ascii)

# 4. Compare.
Write-Output "[4/4] comparing (kinds: $Kinds) ..."
$e = [System.IO.File]::ReadAllText($expected)
$a = [System.IO.File]::ReadAllText($actual)
if ($e -eq $a) {
    Write-Output ("PASS: identical (filtered) output on " + $files.Count + " files")
    exit 0
}
Write-Output "FAIL: outputs differ"
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
