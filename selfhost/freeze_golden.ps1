# selfhost/freeze_golden.ps1 - H9.3: freeze the oracle behaviour into selfhost/golden/.
#
# The golden tree is the oracle-free reference for every gate.  Once frozen,
# `selfhost/check_golden.ps1` compares a built `ivy_selfhost.exe` against these
# files and never runs a C++ compiler; a checkout without the C++ tree can
# therefore still prove parity (H9.5/H9.6/H9.9).  Re-freezing needs the retired
# oracle from tag `0.5-baseline` (pass -Oracle or set IVY_ORACLE).
#
# What is recorded, per fixture (repository-relative, forward-slash path):
#
#   dump/lex.txt              --lex    stdout+stderr (merged), exit status
#   dump/tokens.txt           --tokens stdout+stderr (merged), exit status
#   dump/ast.txt              --ast    stdout+stderr (merged), exit status
#   dump/hir.txt              --hir    stdout+stderr (merged), exit status
#   dump/mir.txt              --mir    stdout+stderr (merged), exit status
#   dump/llvm.txt             --llvm   stdout+stderr (merged), exit status
#   compile/c.txt             -c       stdout+stderr (merged, <OUT> normalized)
#   compile/objects.tsv       -c       SHA-256/size of the .obj and the .ivm
#   compile/o.txt             -o *.i   stdout+stderr (merged, <OUT> normalized)
#   compile/preproc.tsv       -o *.i   SHA-256/size of the written file
#   interp/stdout.txt         --run    raw stdout, exit status
#   interp/stderr.txt         --run    raw stderr, exit status
#   link/diag.txt             link     stderr (<OUT>/<RT> normalized), exit status
#   link/stdout.txt           run      raw stdout of the linked executable
#   link/stderr.txt           run      raw stderr of the linked executable
#   link/sideeffects.txt      run      every file the executable wrote
#   link/nondet.txt           run      fixtures whose own output is not reproducible
#   exceptions.txt            any      records whose oracle run crashed (mode|fixture|reason);
#                                      the gate only requires the port not to crash there
#
# The text modes are line-normalized exactly like selfhost/difftest.ps1 (CRLF ->
# LF, one final newline), so the record does not depend on the Windows CRT text
# mode.  `--run`, executable stdout/stderr, objects and `.i` files are byte
# exact.
#
# Records are length-prefixed, so a block that does not end with a newline (the
# `--lex`/`--tokens` dump ends with an unterminated `eof` token) is preserved:
#
#   "@@ <path> <nbytes> <exit>\n" followed by <nbytes> raw bytes
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/freeze_golden.ps1
#   powershell -NoProfile -File selfhost/freeze_golden.ps1 -OutDir C:\scratch\golden
#   powershell -NoProfile -File selfhost/freeze_golden.ps1 -Modes lex,ast -Filter 01_
#
# -Filter keeps only fixture file names that match the wildcard.  -Modes limits
# the sections (dev only; the committed golden tree is produced with the full
# default).  Nothing is written outside -OutDir and the per-run TEMP directory.

param(
    # H9.9: the C++ oracle no longer ships in the tree.  Point this at a
    # compiler built from tag `0.5-baseline` (or set IVY_ORACLE) to re-freeze.
    [string]$Oracle = "",
    [string]$OutDir = "selfhost/golden",
    [string]$TempRoot = "",
    [int]$TimeoutSec = 120,
    # Linker whose path is folded to <CLANG> in the diagnostics.  Defaults to
    # `$env:IVY_LINKER`, then the first `clang++` on PATH (what the compiler
    # finds), so the baseline stays independent of the LLVM installation path.
    [string]$Linker = "",
    # Directory holding the prebuilt `.ivm` interfaces the corpus imports;
    # appended to the fixture's own `-I` (see check_golden.ps1).  Pass the same
    # directory to both scripts so a re-freeze and a check resolve the imports
    # to identical bytes.
    [string]$IvmDir = "",
    # Development helpers: restrict the corpus / section.
    [string]$Filter = "",
    [string]$Modes = "lex,tokens,ast,hir,mir,llvm,c,o,run,link"
)

$ErrorActionPreference = "Stop"

function Fail([string]$msg) {
    Write-Output ("FAILED: " + $msg)
    exit 1
}

$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo

$oraclePath = $Oracle
if (-not $oraclePath) { $oraclePath = $env:IVY_ORACLE }
if (-not $oraclePath) {
    Fail "re-freezing needs the C++ oracle from tag 0.5-baseline: pass -Oracle <ivy.exe> or set IVY_ORACLE"
}
if (-not [System.IO.Path]::IsPathRooted($oraclePath)) { $oraclePath = Join-Path $repo $oraclePath }
if (-not (Test-Path -LiteralPath $oraclePath)) { Fail ("oracle not found: " + $oraclePath) }
$oraclePath = (Resolve-Path -LiteralPath $oraclePath).Path
$oracleSha = (Get-FileHash -LiteralPath $oraclePath -Algorithm SHA256).Hash

$target = $OutDir
if (-not [System.IO.Path]::IsPathRooted($target)) { $target = Join-Path $repo $target }
$target = [System.IO.Path]::GetFullPath($target)
$stage = $target + ".tmp"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$work = Join-Path $tempBase ("h93golden_" + $stamp + "_" + $PID)
New-Item -ItemType Directory -Force -Path $work | Out-Null
$env:TEMP = $work
$env:TMP = $work
Write-Output ("run directory: " + $work)
Write-Output ("golden target: " + $target)

$modeSet = @{}
foreach ($m in $Modes.Split(",")) { $k = $m.Trim(); if ($k -ne "") { $modeSet[$k] = $true } }

# --- helpers -------------------------------------------------------------------
$script:Latin1 = [System.Text.Encoding]::GetEncoding(28591)

# CRLF -> LF and exactly one final newline (mirrors difftest.ps1's byte-line
# join).  An empty capture stays empty.
function Normalize-Lines([byte[]]$bytes) {
    if ($bytes.Length -eq 0) { return ,[byte[]]@() }
    $s = $script:Latin1.GetString($bytes)
    $s = $s.Replace("`r`n", "`n")
    if ($s.EndsWith("`n")) { $s = $s.Substring(0, $s.Length - 1) }
    return ,$script:Latin1.GetBytes($s + "`n")
}

# The oracle bakes the runtime archive path at build time; the self-host driver
# reads IVYRT_LIB_PATH.  Both spellings are folded to <RT> so a diagnostic that
# prints the linker command line stays location independent.
$script:RtPaths = @(
    (Join-Path $repo "build\Release\ivyrt.lib"),
    ((Join-Path $repo "build\Release\ivyrt.lib") -replace '\\', '/'),
    (Join-Path $repo "build\rt.obj"),
    ((Join-Path $repo "build\rt.obj") -replace '\\', '/')
)

# H9.6: the linker path is baked into the `linker failed (exit code ...)`
# diagnostics.  Fold both spellings of the path to `<CLANG>` so the same
# baseline is valid on any machine, including a CI runner whose LLVM lives
# somewhere else.  `-Linker` wins, then IVY_LINKER (the compiler's own
# override), then the first `clang++` on PATH.  The path is matched before the
# file name (the compiler prints `"C:/.../clang++.exe"` in its command line and
# `clang++: error: ...` for clang's own driver errors; the shorter `clang++`
# stays as is).
$linkerPath = $Linker
if (-not $linkerPath) { $linkerPath = $env:IVY_LINKER }
if (-not $linkerPath) {
    $cmd = Get-Command "clang++" -CommandType Application -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($cmd) { $linkerPath = $cmd.Source }
}
$script:ClangPaths = @()
if ($linkerPath) {
    $script:ClangPaths += $linkerPath
    $script:ClangPaths += ($linkerPath -replace '\\', '/')
}

function Normalize-Paths([byte[]]$bytes, [string]$root) {
    $s = $script:Latin1.GetString($bytes)
    $s = $s.Replace($root, "<OUT>")
    $s = $s.Replace(($root -replace '\\', '/'), "<OUT>")
    foreach ($p in $script:RtPaths) { $s = $s.Replace($p, "<RT>") }
    foreach ($p in $script:ClangPaths) { $s = $s.Replace($p, "<CLANG>") }
    # H9.7: see check_golden.ps1 - the port links with `/Brepro`, the oracle
    # did not; the flag is a build detail and is dropped from diagnostics.
    $s = $s.Replace(" -Wl,/Brepro", "")
    return Normalize-Lines ($script:Latin1.GetBytes($s))
}

function Quote-Arg([string]$s) {
    if ($s -match '[\s"]') { return '"' + ($s -replace '"', '\"') + '"' }
    return $s
}

# Run one process with stdout+stderr merged into a single file (the same
# capture difftest.ps1 uses) and return its exit status.
function Invoke-Merged([string]$exe, [string[]]$argList, [string]$outPath) {
    Remove-Item -LiteralPath $outPath -ErrorAction SilentlyContinue
    $line = '"' + $exe + '"'
    foreach ($a in $argList) { $line += " " + (Quote-Arg $a) }
    cmd /c "$line > `"$outPath`" 2>&1"
    return $LASTEXITCODE
}

# Run one process with byte-exact separate stdout/stderr files.  stdin is
# closed immediately so a fixture that reads input cannot block.
function Invoke-Separate([string]$exe, [string[]]$argList, [string]$outPath,
                         [string]$errPath, [string]$workingDir) {
    Remove-Item -LiteralPath $outPath, $errPath -ErrorAction SilentlyContinue
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = $workingDir
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

# Length-prefixed record buffers, written once at the end.
$script:Buffers = @{}
# Records whose oracle run crashed (a negative exit status) are listed in
# `exceptions.txt`: there is no reference output to compare, so the gate only
# requires that the self-host compiler does not crash on the same input.  Only
# the dump modes can currently crash in the oracle (`--hir`/`--mir` on an
# init-list with a synthesized null element).
$script:Exceptions = New-Object System.Collections.Generic.List[string]
function Add-Record([string]$name, [string]$key, [int]$exit, [byte[]]$data) {
    if (-not $script:Buffers.ContainsKey($name)) {
        $script:Buffers[$name] = New-Object System.Collections.Generic.List[byte]
    }
    $hdr = [System.Text.Encoding]::UTF8.GetBytes("@@ " + $key + " " + $data.Length + " " + $exit + "`n")
    $script:Buffers[$name].AddRange([byte[]]$hdr)
    $script:Buffers[$name].AddRange([byte[]]$data)
    if ($exit -lt 0 -and $name.StartsWith("dump/")) {
        $mode = [System.IO.Path]::GetFileNameWithoutExtension($name)
        $script:Exceptions.Add($mode + "|" + $key + "|oracle crash " + $exit + " (0x" +
                               (4294967296 + [int64]$exit).ToString("X8") + "); port must not crash")
    }
}

$script:Tsv = @{}
function Add-Tsv([string]$name, [string]$line) {
    if (-not $script:Tsv.ContainsKey($name)) {
        $script:Tsv[$name] = New-Object System.Collections.Generic.List[string]
    }
    $script:Tsv[$name].Add($line)
}

function Read-Bytes([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return ,[byte[]]@() }
    return ,[System.IO.File]::ReadAllBytes($path)
}

function Test-BytesEqual([byte[]]$a, [byte[]]$b) {
    if ($a.Length -ne $b.Length) { return $false }
    for ($i = 0; $i -lt $a.Length; $i++) {
        if ($a[$i] -ne $b[$i]) { return $false }
    }
    return $true
}

function Get-Sha([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return "-" }
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

function Get-Size([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return 0 }
    return (Get-Item -LiteralPath $path).Length
}

# --- corpus --------------------------------------------------------------------
$corpusRoot = Join-Path $repo "selfhost\corpus"
$all = @(Get-ChildItem -LiteralPath $corpusRoot -Recurse -Filter *.ivy -File |
         ForEach-Object { $_.FullName.Substring($repo.Length + 1).Replace('\', '/') } |
         Sort-Object)
if ($Filter -ne "") { $all = @($all | Where-Object { (Split-Path $_ -Leaf) -like $Filter }) }
if ($all.Count -eq 0) { Fail "no fixtures matched" }

function Get-FixtureDir([string]$rel) {
    $p = [System.IO.Path]::GetDirectoryName($rel).Replace('\', '/')
    return $p
}

function Get-ModeArgs([string]$mode, [string]$rel, [string]$objPath, [string]$outFile) {
    $dir = Get-FixtureDir $rel
    $inc = @("-I", $dir)
    if ($IvmDir -ne "") { $inc += @("-I", $IvmDir) }
    switch ($mode) {
        "lex"    { return @("--lex", $rel) + $inc }
        "tokens" { return @("--tokens", $rel) + $inc }
        "ast"    { return @("--ast", $rel) + $inc }
        "hir"    { return @("--hir", $rel) + $inc }
        "mir"    { return @("--mir", $rel) + $inc }
        "llvm"   { return @("--llvm", $rel) + $inc }
        "c"      { return @($rel) + $inc + @("-c", "-o", $objPath) }
        "o"      { return @($rel) + $inc + @("-o", $outFile) }
        "run"    { return @("--run", $rel) }
        "link"   { return @($rel) + $inc + @("-o", $objPath) }
    }
    throw ("unknown mode " + $mode)
}

$capture = Join-Path $work "capture.txt"
$objPath = Join-Path $work "out.obj"
$ivmPath = Join-Path $work "out.ivm"
$preprocPath = Join-Path $work "out.i"

# --- 1. dump modes -------------------------------------------------------------
$dumpModes = @("lex", "tokens", "ast", "hir", "mir", "llvm")
foreach ($mode in $dumpModes) {
    if (-not $modeSet.ContainsKey($mode)) { continue }
    Write-Output ("[dump] --" + $mode + " over " + $all.Count + " fixtures ...")
    $n = 0
    foreach ($rel in $all) {
        $code = Invoke-Merged $oraclePath (Get-ModeArgs $mode $rel "" "") $capture
        $text = Normalize-Paths (Read-Bytes $capture) $work
        Add-Record ("dump/" + $mode + ".txt") $rel $code $text
        $n++
        if (($n % 200) -eq 0) { Write-Output ("       " + $n + " / " + $all.Count) }
    }
}

# --- 2. -c: merged output + object/.ivm hashes ---------------------------------
if ($modeSet.ContainsKey("c")) {
    Write-Output ("[c] -c over " + $all.Count + " fixtures ...")
    $n = 0
    foreach ($rel in $all) {
        Remove-Item -LiteralPath $objPath, $ivmPath -ErrorAction SilentlyContinue
        $code = Invoke-Merged $oraclePath (Get-ModeArgs "c" $rel $objPath "") $capture
        $text = Normalize-Paths (Read-Bytes $capture) $work
        Add-Record "compile/c.txt" $rel $code $text
        $objSha = Get-Sha $objPath
        $objSize = Get-Size $objPath
        $ivmSha = Get-Sha $ivmPath
        $ivmSize = Get-Size $ivmPath
        Add-Tsv "compile/objects.tsv" ($rel + "`t" + $objSha + "`t" + $objSize + "`t" + $ivmSha + "`t" + $ivmSize)
        $n++
        if (($n % 200) -eq 0) { Write-Output ("       " + $n + " / " + $all.Count) }
    }
}

# --- 3. -o *.i: merged output + preprocessed file hash -------------------------
if ($modeSet.ContainsKey("o")) {
    Write-Output ("[o] -o *.i over " + $all.Count + " fixtures ...")
    $n = 0
    foreach ($rel in $all) {
        Remove-Item -LiteralPath $preprocPath -ErrorAction SilentlyContinue
        $code = Invoke-Merged $oraclePath (Get-ModeArgs "o" $rel "" $preprocPath) $capture
        $text = Normalize-Paths (Read-Bytes $capture) $work
        Add-Record "compile/o.txt" $rel $code $text
        Add-Tsv "compile/preproc.tsv" ($rel + "`t" + (Get-Sha $preprocPath) + "`t" + (Get-Size $preprocPath))
        $n++
        if (($n % 200) -eq 0) { Write-Output ("       " + $n + " / " + $all.Count) }
    }
}

# --- 4. --run: raw stdout/stderr/exit of the interpreter -----------------------
$interpFixtures = @($all | Where-Object { $_ -like "selfhost/corpus/interp/*" })
if ($modeSet.ContainsKey("run")) {
    Write-Output ("[run] --run over " + $interpFixtures.Count + " fixtures ...")
    $cwd = Join-Path $work "interp"
    New-Item -ItemType Directory -Force -Path $cwd | Out-Null
    $oOut = Join-Path $work "i.out"
    $oErr = Join-Path $work "i.err"
    $i = 0
    foreach ($rel in $interpFixtures) {
        $code = Invoke-Separate $oraclePath (Get-ModeArgs "run" $rel "" "") $oOut $oErr $cwd
        Add-Record "interp/stdout.txt" $rel $code (Read-Bytes $oOut)
        Add-Record "interp/stderr.txt" $rel $code (Read-Bytes $oErr)
        $i++
    }
}

# --- 5. link + run ------------------------------------------------------------
$linkFixtures = @($all | Where-Object { $_ -like "selfhost/corpus/codegen/*" })
$nondet = New-Object System.Collections.Generic.List[string]
if ($modeSet.ContainsKey("link")) {
    Write-Output ("[link] link + run over " + $linkFixtures.Count + " fixtures ...")
    $oOut = Join-Path $work "l.out"
    $oErr = Join-Path $work "l.err"
    $exe = Join-Path $work "app.exe"
    $i = 0
    foreach ($rel in $linkFixtures) {
        Remove-Item -LiteralPath $exe -ErrorAction SilentlyContinue
        $code = Invoke-Merged $oraclePath (Get-ModeArgs "link" $rel $exe "") $capture
        Add-Record "link/diag.txt" $rel $code (Normalize-Paths (Read-Bytes $capture) $work)
        if ($code -eq 0) {
            if (-not (Test-Path -LiteralPath $exe)) { Fail ("link succeeded without an executable: " + $rel) }
            # Run the oracle executable twice in independent directories; a
            # fixture that reads uninitialised storage or prints unspecified
            # values is only comparable on the link step.
            $runs = @()
            for ($k = 1; $k -le 2; $k++) {
                $cwd = Join-Path $work ("exec\" + $i + "_" + $k)
                New-Item -ItemType Directory -Force -Path $cwd | Out-Null
                $rc = Invoke-Separate $exe @() $oOut $oErr $cwd
                $files = @{}
                foreach ($f in (Get-ChildItem -LiteralPath $cwd -Recurse -File | Sort-Object FullName)) {
                    $name = $f.FullName.Substring($cwd.Length).TrimStart('\', '/').Replace('\', '/')
                    $files[$name] = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
                }
                $runs += ,@($rc, (Read-Bytes $oOut), (Read-Bytes $oErr), $files)
            }
            $same = ($runs[0][0] -eq $runs[1][0])
            if ($same) { $same = Test-BytesEqual $runs[0][1] $runs[1][1] }
            if ($same) { $same = Test-BytesEqual $runs[0][2] $runs[1][2] }
            if ($same) {
                $fa = $runs[0][3]; $fb = $runs[1][3]
                if ($fa.Count -ne $fb.Count) { $same = $false }
                else { foreach ($k in $fa.Keys) { if ($fb[$k] -ne $fa[$k]) { $same = $false } } }
            }
            if (-not $same) {
                $nondet.Add($rel)
            } else {
                Add-Record "link/stdout.txt" $rel $runs[0][0] $runs[0][1]
                Add-Record "link/stderr.txt" $rel $runs[0][0] $runs[0][2]
                foreach ($k in ($runs[0][3].Keys | Sort-Object)) {
                    # The bytes live in the first run's directory.
                    $src = Join-Path (Join-Path $work ("exec\" + $i + "_1")) $k
                    Add-Record "link/sideeffects.txt" ($rel + "|" + $k) 0 (Read-Bytes $src)
                }
            }
        } elseif (Test-Path -LiteralPath $exe) {
            Fail ("link failed but left an executable: " + $rel)
        }
        $i++
        if (($i % 50) -eq 0) { Write-Output ("       " + $i + " / " + $linkFixtures.Count) }
    }
}

# --- 6. write the staging tree -------------------------------------------------
foreach ($name in $script:Buffers.Keys) {
    $path = Join-Path $stage $name
    New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
    [System.IO.File]::WriteAllBytes($path, $script:Buffers[$name].ToArray())
}
foreach ($name in $script:Tsv.Keys) {
    $path = Join-Path $stage $name
    New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
    $text = ($script:Tsv[$name] -join "`n")
    if ($text -ne "") { $text += "`n" }
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}
[System.IO.File]::WriteAllText((Join-Path $stage "corpus.txt"),
    (($all -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))

# The records whose oracle run crashed.  The gate skips the byte comparison for
# them but still fails if the self-host compiler reproduces the crash.
$excLines = @($script:Exceptions | Sort-Object -Unique)
$excText = "# mode|fixture|reason - oracle crash records (no reference output).`n"
if ($excLines.Count -gt 0) { $excText += (($excLines -join "`n") + "`n") }
[System.IO.File]::WriteAllText((Join-Path $stage "exceptions.txt"), $excText,
    (New-Object System.Text.UTF8Encoding($false)))

if ($modeSet.ContainsKey("link")) {
    $nondetText = ""
    if ($nondet.Count -gt 0) { $nondetText = (($nondet | Sort-Object) -join "`n") + "`n" }
    [System.IO.File]::WriteAllText((Join-Path $stage "link/nondet.txt"), $nondetText,
        (New-Object System.Text.UTF8Encoding($false)))
}

# The oracle binary is not versioned; record its SHA-256 plus the runtime object
# the gate must use, so a golden tree can be traced back to one baseline.
$rtPath = Join-Path $repo "build\rt.obj"
$manifest = @(
    "format=1",
    "fixtures=$($all.Count)",
    "modes=$Modes",
    "interp_fixtures=$($interpFixtures.Count)",
    "link_fixtures=$($linkFixtures.Count)",
    "nondet=$($nondet.Count)",
    "oracle_sha256=$oracleSha",
    "runtime_sha256=$(Get-Sha $rtPath)",
    "runtime_size=$(Get-Size $rtPath)",
    "note=dump text is CRLF-normalized; objects, preprocessed files and run output are byte-exact"
)
[System.IO.File]::WriteAllText((Join-Path $stage "manifest.txt"), (($manifest -join "`n") + "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

# --- 7. swap the staging tree into place ---------------------------------------
if (Test-Path -LiteralPath $target) { Remove-Item -Recurse -Force $target }
Move-Item -LiteralPath $stage -Destination $target

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue

$totalBytes = (Get-ChildItem -LiteralPath $target -Recurse -File | Measure-Object Length -Sum).Sum
Write-Output ("frozen: " + $all.Count + " fixtures, " + $totalBytes + " bytes -> " + $target)
Write-Output ("oracle: " + $oracleSha)
Write-Output "GOLDEN FROZEN"
exit 0
