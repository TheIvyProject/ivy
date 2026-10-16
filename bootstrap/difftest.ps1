# bootstrap/difftest.ps1 — F1.1 differential test: Ivy lexer port vs C++ lexer.
#
# Compares `ivy --lex` (raw C++ lexer dump) against bootstrap/lexer.ivy
# (compiled to lexport.exe) over the corpus:
#   examples/*.ivy + std/*.ivy + bootstrap/*.ivy + bootstrap/corpus_err/*.ivy
# (--tokens is post-preprocessor and would hide directives; --lex is raw.)
#
# Byte-exact comparison (fc /B): corpus contains UTF-8, so all captures use
# cmd.exe raw redirects — never PowerShell string pipelines (lossy).
#
# Usage (from repo root):  powershell -File bootstrap/difftest.ps1
# Exit 0 = identical, 1 = mismatch or build failure.

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo

$ivy = Join-Path $repo "build\Release\ivy.exe"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "ivydiff"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$port = Join-Path $tmp "lexport.exe"
$list = Join-Path $tmp "files.txt"
$expected = Join-Path $tmp "expected.txt"
$actual = Join-Path $tmp "actual.txt"

# 1. Build the port with the C++ compiler (freshness guaranteed).
Write-Output "[1/4] building bootstrap/lexer.ivy ..."
cmd /c "`"$ivy`" bootstrap/lexer.ivy -o `"$port`" > NUL 2>&1"
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED"; exit 1 }

# 2. Corpus file list (relative, forward slashes, sorted; ASCII only).
Write-Output "[2/4] collecting corpus ..."
$files = @(Get-ChildItem examples/*.ivy, std/*.ivy, bootstrap/*.ivy, bootstrap/corpus_err/*.ivy |
    ForEach-Object { $_.FullName.Substring($repo.Length + 1) -replace "\\", "/" } |
    Sort-Object -Unique)
$files | Set-Content $list -Encoding ascii
Write-Output ("      files: " + $files.Count)

# 3. Expected: C++ `ivy --lex` per file, raw byte capture via cmd.
Write-Output "[3/4] running C++ lexer ..."
if (Test-Path $expected) { Remove-Item $expected }
foreach ($f in $files) {
    cmd /c "(echo === $f ===& `"$ivy`" --lex `"$f`") >> `"$expected`" 2>&1"
}

# 4. Actual: port over stdin list, raw byte capture via cmd.
Write-Output "[4/4] running Ivy lexer port ..."
cmd /c "(type `"$list`" | `"$port`") > `"$actual`" 2>&1"

# 5. Byte compare.
& "$env:SystemRoot\System32\fc.exe" /B "$expected" "$actual" > $null 2> $null
if ($LASTEXITCODE -eq 0) {
    Write-Output ("PASS: identical output on " + $files.Count + " files")
    exit 0
}
Write-Output "FAIL: outputs differ"
$expLines = [System.IO.File]::ReadAllLines($expected, [System.Text.Encoding]::UTF8)
$actLines = [System.IO.File]::ReadAllLines($actual, [System.Text.Encoding]::UTF8)
$shown = 0
for ($i = 0; $i -lt [Math]::Max($expLines.Count, $actLines.Count) -and $shown -lt 20; $i++) {
    $e = if ($i -lt $expLines.Count) { $expLines[$i] } else { "<EOF>" }
    $a = if ($i -lt $actLines.Count) { $actLines[$i] } else { "<EOF>" }
    if ($e -ne $a) { Write-Output ("line " + ($i + 1) + " exp: " + $e); Write-Output ("line " + ($i + 1) + " act: " + $a); $shown++ }
}
exit 1
