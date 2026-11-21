# selfhost/check_golden.ps1 - H9.3: the oracle-free parity gate.
#
# Compares a built `ivy_selfhost.exe` against the frozen baseline in
# `selfhost/golden/` (see `selfhost/freeze_golden.ps1`).  The C++ oracle
# (`build\Release\ivy.exe`) is NEVER run or required here: this is the gate that
# still works after the C++ tree is deleted (H9.9).
#
# Sections (all enabled by default, -Modes limits them):
#
#   lex, tokens, ast, hir, mir, llvm   dump text + exit status, every fixture
#   c                                  -c object/.ivm SHA-256 + merged output + exit
#   o                                  -o *.i file SHA-256 + merged output + exit
#   run                                --run raw stdout/stderr + exit (interp corpus)
#   link                               link stderr (<OUT>/<RT> normalized) + the
#                                      linked executable's raw stdout/stderr/exit
#                                      and every file it writes
#
# `golden/exceptions.txt` lists the records whose *oracle* run crashed (there is
# no reference output).  The gate skips the byte comparison for those records but
# still fails if the port also crashes on them.
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/check_golden.ps1 -Driver <path>\ivy_selfhost.exe
#   powershell -NoProfile -File selfhost/check_golden.ps1 -Driver ... -Modes lex,ast
#   powershell -NoProfile -File selfhost/check_golden.ps1 -Driver ... -Filter 01_*
#   powershell -NoProfile -File selfhost/check_golden.ps1 -Driver ... -Golden C:\scratch\golden
#
# Exit 0 iff every selected check matches the frozen baseline.

param(
    [Parameter(Mandatory=$true)][string]$Driver,
    [string]$Golden = "selfhost/golden",
    [string]$TempRoot = "",
    # Runtime object the driver links with.  Defaults to $env:IVYRT_LIB_PATH,
    # then `build\rt.obj`.  Its path is normalised to `<RT>` in the `link`
    # diagnostics so the baseline does not depend on where the object lives.
    [string]$Runtime = "",
    [int]$TimeoutSec = 120,
    [string]$Modes = "lex,tokens,ast,hir,mir,llvm,c,o,run,link",
    [string]$Filter = "",
    # How many mismatches to describe per section.
    [int]$MaxReport = 10
)

$ErrorActionPreference = "Stop"

function Fail([string]$msg) {
    Write-Output ("FAILED: " + $msg)
    exit 1
}

$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo

if (-not (Test-Path -LiteralPath $Driver)) { Fail ("driver not found: " + $Driver) }
$driverPath = (Resolve-Path -LiteralPath $Driver).Path

$goldenDir = $Golden
if (-not [System.IO.Path]::IsPathRooted($goldenDir)) { $goldenDir = Join-Path $repo $goldenDir }
if (-not (Test-Path -LiteralPath $goldenDir)) { Fail ("golden tree not found: " + $goldenDir) }

$manifestPath = Join-Path $goldenDir "manifest.txt"
if (-not (Test-Path -LiteralPath $manifestPath)) { Fail "manifest.txt missing from the golden tree" }
$manifest = @{}
foreach ($line in (Get-Content -LiteralPath $manifestPath)) {
    $eq = $line.IndexOf("=")
    if ($eq -gt 0) { $manifest[$line.Substring(0, $eq)] = $line.Substring($eq + 1) }
}
if ($manifest["format"] -ne "1") { Fail ("unsupported golden format: " + $manifest["format"]) }

$corpusPath = Join-Path $goldenDir "corpus.txt"
if (-not (Test-Path -LiteralPath $corpusPath)) { Fail "corpus.txt missing from the golden tree" }
$all = @(Get-Content -LiteralPath $corpusPath | Where-Object { $_ -ne "" })
if ($Filter -ne "") { $all = @($all | Where-Object { (Split-Path $_ -Leaf) -like $Filter }) }
if ($all.Count -eq 0) { Fail "no fixtures matched" }

$modeSet = @{}
foreach ($m in $Modes.Split(",")) { $k = $m.Trim(); if ($k -ne "") { $modeSet[$k] = $true } }

# Records whose *oracle* run crashed have no reference output (freeze_golden.ps1
# writes exceptions.txt).  The gate skips the byte comparison for those but still
# requires that the self-host compiler handles the input without crashing.
$script:Exceptions = @{}
$exceptionsPath = Join-Path $goldenDir "exceptions.txt"
if (Test-Path -LiteralPath $exceptionsPath) {
    foreach ($line in (Get-Content -LiteralPath $exceptionsPath)) {
        $t = $line.Trim()
        if ($t -eq "" -or $t.StartsWith("#")) { continue }
        $f = $t.Split("|")
        if ($f.Count -ge 2) { $script:Exceptions[$f[0] + "|" + $f[1]] = $t }
    }
}

# The link section needs the runtime object.  The oracle has its archive path
# baked in; the self-host driver reads IVYRT_LIB_PATH (H9.1).  `-Runtime` wins,
# then the caller's environment, then the historical `build\rt.obj` (the path
# the baseline was frozen with, and the only one that exists while `src/**` is
# still around).
$defaultRt = Join-Path $repo "build\rt.obj"
$rtUsed = $Runtime
if (-not $rtUsed) { $rtUsed = $env:IVYRT_LIB_PATH }
if (-not $rtUsed -and (Test-Path -LiteralPath $defaultRt)) { $rtUsed = $defaultRt }
if ($rtUsed) { $env:IVYRT_LIB_PATH = $rtUsed }
if ($manifest["runtime_sha256"] -ne "-" -and $rtUsed -and
    (Test-Path -LiteralPath $rtUsed)) {
    $rtSha = (Get-FileHash -LiteralPath $rtUsed -Algorithm SHA256).Hash
    if ($rtSha -ne $manifest["runtime_sha256"]) {
        Write-Output ("warning: runtime differs from the frozen one (" + $rtSha + " vs " +
                      $manifest["runtime_sha256"] + "): " + $rtUsed)
    }
}

# --- helpers -------------------------------------------------------------------
$script:Latin1 = [System.Text.Encoding]::GetEncoding(28591)

function Normalize-Lines([byte[]]$bytes) {
    if ($bytes.Length -eq 0) { return ,[byte[]]@() }
    $s = $script:Latin1.GetString($bytes)
    $s = $s.Replace("`r`n", "`n")
    if ($s.EndsWith("`n")) { $s = $s.Substring(0, $s.Length - 1) }
    return ,$script:Latin1.GetBytes($s + "`n")
}

$script:RtPaths = @(
    (Join-Path $repo "build\Release\ivyrt.lib"),
    ((Join-Path $repo "build\Release\ivyrt.lib") -replace '\\', '/'),
    (Join-Path $repo "build\rt.obj"),
    ((Join-Path $repo "build\rt.obj") -replace '\\', '/')
)
# H9.5: an Ivy-only bootstrap links with `rt.obj` in a per-run temp directory,
# so normalise whatever runtime the driver actually uses to `<RT>` as well.
if ($rtUsed) {
    $script:RtPaths += $rtUsed
    $script:RtPaths += ($rtUsed -replace '\\', '/')
}

function Normalize-Paths([byte[]]$bytes, [string]$root) {
    $s = $script:Latin1.GetString($bytes)
    $s = $s.Replace($root, "<OUT>")
    $s = $s.Replace(($root -replace '\\', '/'), "<OUT>")
    foreach ($p in $script:RtPaths) { $s = $s.Replace($p, "<RT>") }
    return Normalize-Lines ($script:Latin1.GetBytes($s))
}

function Quote-Arg([string]$s) {
    if ($s -match '[\s"]') { return '"' + ($s -replace '"', '\"') + '"' }
    return $s
}

function Invoke-Merged([string]$exe, [string[]]$argList, [string]$outPath) {
    Remove-Item -LiteralPath $outPath -ErrorAction SilentlyContinue
    $line = '"' + $exe + '"'
    foreach ($a in $argList) { $line += " " + (Quote-Arg $a) }
    cmd /c "$line > `"$outPath`" 2>&1"
    return $LASTEXITCODE
}

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

function Test-BytesEqual([byte[]]$a, [byte[]]$b) {
    if ($a.Length -ne $b.Length) { return $false }
    for ($i = 0; $i -lt $a.Length; $i++) {
        if ($a[$i] -ne $b[$i]) { return $false }
    }
    return $true
}

function Read-Bytes([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return ,[byte[]]@() }
    return ,[System.IO.File]::ReadAllBytes($path)
}

function Get-Sha([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return "-" }
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

function Get-Size([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return 0 }
    return (Get-Item -LiteralPath $path).Length
}

# Read a length-prefixed record file: "@@ <key> <nbytes> <exit>\n" + n bytes.
function Read-Records([string]$path) {
    $map = @{}
    if (-not (Test-Path -LiteralPath $path)) { return $map }
    $b = [System.IO.File]::ReadAllBytes($path)
    $i = 0
    while ($i -lt $b.Length) {
        $j = $i
        while ($j -lt $b.Length -and $b[$j] -ne 10) { $j++ }
        if ($j -ge $b.Length) { throw ("truncated record header in " + $path) }
        $hdr = [System.Text.Encoding]::UTF8.GetString($b, $i, $j - $i)
        $parts = $hdr.Split(' ')
        if ($parts.Count -ne 4 -or $parts[0] -ne "@@") { throw ("bad record header in " + $path + ": " + $hdr) }
        $key = $parts[1]
        $n = [int]$parts[2]
        $exit = [int]$parts[3]
        $start = $j + 1
        if ($start + $n -gt $b.Length) { throw ("truncated record body in " + $path + ": " + $key) }
        $seg = New-Object byte[] $n
        if ($n -gt 0) { [Array]::Copy($b, $start, $seg, 0, $n) }
        $map[$key] = @($exit, $seg)
        $i = $start + $n
    }
    return $map
}

function Read-Tsv([string]$path) {
    $map = @{}
    if (-not (Test-Path -LiteralPath $path)) { return $map }
    foreach ($line in (Get-Content -LiteralPath $path)) {
        if ($line -eq "") { continue }
        $f = $line.Split("`t")
        $map[$f[0]] = $f
    }
    return $map
}

# Show the first differing line of a byte pair (lossy decode is enough here).
function Describe-Diff([byte[]]$expected, [byte[]]$actual) {
    $n = [Math]::Min($expected.Length, $actual.Length)
    $off = -1
    for ($i = 0; $i -lt $n; $i++) {
        if ($expected[$i] -ne $actual[$i]) { $off = $i; break }
    }
    if ($off -lt 0) { $off = $n }
    $es = $script:Latin1.GetString($expected)
    $as = $script:Latin1.GetString($actual)
    $prefix = $es.Substring(0, [Math]::Min($off, $es.Length))
    $line = 1
    for ($i = 0; $i -lt $prefix.Length; $i++) { if ($prefix[$i] -eq "`n") { $line++ } }
    $el = if (($line - 1) -lt $es.Split("`n").Count) { $es.Split("`n")[$line - 1] } else { "<EOF>" }
    $al = if (($line - 1) -lt $as.Split("`n").Count) { $as.Split("`n")[$line - 1] } else { "<EOF>" }
    $msg = "byte " + $off + " (line " + $line + "; expected " + $expected.Length + " B, actual " + $actual.Length + " B)"
    return @($msg, $el, $al)
}

function Get-FixtureDir([string]$rel) {
    return [System.IO.Path]::GetDirectoryName($rel).Replace('\', '/')
}

function Get-ModeArgs([string]$mode, [string]$rel, [string]$objPath, [string]$outFile) {
    $dir = Get-FixtureDir $rel
    switch ($mode) {
        "lex"    { return @("--lex", $rel, "-I", $dir) }
        "tokens" { return @("--tokens", $rel, "-I", $dir) }
        "ast"    { return @("--ast", $rel, "-I", $dir) }
        "hir"    { return @("--hir", $rel, "-I", $dir) }
        "mir"    { return @("--mir", $rel, "-I", $dir) }
        "llvm"   { return @("--llvm", $rel, "-I", $dir) }
        "c"      { return @($rel, "-I", $dir, "-c", "-o", $objPath) }
        "o"      { return @($rel, "-I", $dir, "-o", $outFile) }
        "run"    { return @("--run", $rel) }
        "link"   { return @($rel, "-I", $dir, "-o", $objPath) }
    }
    throw ("unknown mode " + $mode)
}

# --- scratch -------------------------------------------------------------------
$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$work = Join-Path $tempBase ("h93check_" + $stamp + "_" + $PID)
New-Item -ItemType Directory -Force -Path $work | Out-Null
$env:TEMP = $work
$env:TMP = $work
$capture = Join-Path $work "capture.txt"
$objPath = Join-Path $work "out.obj"
$ivmPath = Join-Path $work "out.ivm"
$preprocPath = Join-Path $work "out.i"

$script:Total = 0
$script:Failed = 0
$script:Section = ""
$script:Reported = 0

function Begin-Section([string]$name) {
    $script:Section = $name
    $script:Reported = 0
}

function Report-Fail([string]$rel, [string]$detail, [string]$extra, [string]$extra2) {
    $script:Failed++
    if ($script:Reported -lt $MaxReport) {
        Write-Output ("  FAIL [" + $script:Section + "] " + $rel + ": " + $detail)
        if ($extra -ne $null -and $extra -ne "") { Write-Output ("       exp: " + $extra) }
        if ($extra2 -ne $null -and $extra2 -ne "") { Write-Output ("       act: " + $extra2) }
        $script:Reported++
    } elseif ($script:Reported -eq $MaxReport) {
        Write-Output ("  ... further [" + $script:Section + "] failures not shown")
        $script:Reported++
    }
}

function Check-Record([string]$rel, $goldenRec, [int]$exit, [byte[]]$actual) {
    $script:Total++
    if ($goldenRec -eq $null) {
        Report-Fail $rel "no golden record" "" ""
        return
    }
    # An exception record is an oracle crash: there is no reference output, so
    # the only requirement is that the port does not reproduce the crash.
    $section = $script:Section.TrimStart('-')
    $exKey = $section + "|" + $rel
    if ($script:Exceptions.ContainsKey($exKey)) {
        if ($goldenRec[0] -ge 0) {
            Report-Fail $rel ("exception recorded but golden exit is " + $goldenRec[0]) "" ""
            return
        }
        if ($exit -lt 0) {
            Report-Fail $rel ("port crashed (" + $exit + ") on an oracle-crash fixture") "" ""
        }
        return
    }
    if ($goldenRec[0] -ne $exit) {
        Report-Fail $rel ("exit expected " + $goldenRec[0] + ", actual " + $exit) "" ""
        return
    }
    if (-not (Test-BytesEqual $goldenRec[1] $actual)) {
        $d = Describe-Diff $goldenRec[1] $actual
        Report-Fail $rel ("output differs at " + $d[0]) $d[1] $d[2]
        return
    }
}

function Check-Hash([string]$rel, [string]$what, [string]$expectedSha, [long]$expectedSize,
                    [string]$path) {
    $script:Total++
    if ($expectedSha -eq "-") {
        if (Test-Path -LiteralPath $path) {
            Report-Fail $rel ($what + " should not exist, but was written") "" ""
        }
        return
    }
    if (-not (Test-Path -LiteralPath $path)) {
        Report-Fail $rel ($what + " missing") "" ""
        return
    }
    $sha = Get-Sha $path
    $size = Get-Size $path
    if ($sha -ne $expectedSha -or $size -ne $expectedSize) {
        Report-Fail $rel ($what + " differs") ($expectedSha + " (" + $expectedSize + " B)") ($sha + " (" + $size + " B)")
    }
}

# --- 1. dump modes -------------------------------------------------------------
$dumpModes = @("lex", "tokens", "ast", "hir", "mir", "llvm")
foreach ($mode in $dumpModes) {
    if (-not $modeSet.ContainsKey($mode)) { continue }
    $file = Join-Path $goldenDir ("dump/" + $mode + ".txt")
    if (-not (Test-Path -LiteralPath $file)) { Fail ("golden section missing: dump/" + $mode + ".txt") }
    Begin-Section ("--" + $mode)
    Write-Output ("[" + $mode + "] " + $all.Count + " fixtures ...")
    $recs = Read-Records $file
    $n = 0
    foreach ($rel in $all) {
        $code = Invoke-Merged $driverPath (Get-ModeArgs $mode $rel "" "") $capture
        $text = Normalize-Paths (Read-Bytes $capture) $work
        Check-Record $rel $recs[$rel] $code $text
        $n++
        if ($n % 250 -eq 0) { Write-Output ("       " + $n + " / " + $all.Count) }
    }
}

# --- 2. -c: merged output + object/.ivm hash -----------------------------------
if ($modeSet.ContainsKey("c")) {
    $file = Join-Path $goldenDir "compile/c.txt"
    $tsvPath = Join-Path $goldenDir "compile/objects.tsv"
    if (-not (Test-Path -LiteralPath $file)) { Fail "golden section missing: compile/c.txt" }
    Begin-Section "-c"
    Write-Output ("[c] " + $all.Count + " fixtures ...")
    $recs = Read-Records $file
    $tsv = Read-Tsv $tsvPath
    foreach ($rel in $all) {
        Remove-Item -LiteralPath $objPath, $ivmPath -ErrorAction SilentlyContinue
        $code = Invoke-Merged $driverPath (Get-ModeArgs "c" $rel $objPath "") $capture
        $text = Normalize-Paths (Read-Bytes $capture) $work
        Check-Record $rel $recs[$rel] $code $text
        $row = $tsv[$rel]
        if ($row -eq $null) { Report-Fail $rel "no golden object row" "" ""; continue }
        Check-Hash $rel "object" $row[1] ([long]$row[2]) $objPath
        Check-Hash $rel "module interface" $row[3] ([long]$row[4]) $ivmPath
    }
}

# --- 3. -o *.i: merged output + preprocessed file hash -------------------------
if ($modeSet.ContainsKey("o")) {
    $file = Join-Path $goldenDir "compile/o.txt"
    $tsvPath = Join-Path $goldenDir "compile/preproc.tsv"
    if (-not (Test-Path -LiteralPath $file)) { Fail "golden section missing: compile/o.txt" }
    Begin-Section "-o"
    Write-Output ("[o] " + $all.Count + " fixtures ...")
    $recs = Read-Records $file
    $tsv = Read-Tsv $tsvPath
    foreach ($rel in $all) {
        Remove-Item -LiteralPath $preprocPath -ErrorAction SilentlyContinue
        $code = Invoke-Merged $driverPath (Get-ModeArgs "o" $rel "" $preprocPath) $capture
        $text = Normalize-Paths (Read-Bytes $capture) $work
        Check-Record $rel $recs[$rel] $code $text
        $row = $tsv[$rel]
        if ($row -eq $null) { Report-Fail $rel "no golden preprocessed row" "" ""; continue }
        Check-Hash $rel "preprocessed file" $row[1] ([long]$row[2]) $preprocPath
    }
}

# --- 4. --run ------------------------------------------------------------------
if ($modeSet.ContainsKey("run")) {
    $fileOut = Join-Path $goldenDir "interp/stdout.txt"
    $fileErr = Join-Path $goldenDir "interp/stderr.txt"
    if (-not (Test-Path -LiteralPath $fileOut)) { Fail "golden section missing: interp/stdout.txt" }
    Begin-Section "--run"
    $interp = @($all | Where-Object { $_ -like "selfhost/corpus/interp/*" })
    Write-Output ("[run] " + $interp.Count + " fixtures ...")
    $recsOut = Read-Records $fileOut
    $recsErr = Read-Records $fileErr
    $oOut = Join-Path $work "i.out"
    $oErr = Join-Path $work "i.err"
    $idx = 0
    foreach ($rel in $interp) {
        $cwd = Join-Path $work ("interp_" + $idx)
        New-Item -ItemType Directory -Force -Path $cwd | Out-Null
        $code = Invoke-Separate $driverPath (Get-ModeArgs "run" $rel "" "") $oOut $oErr $cwd
        Check-Record ($rel + " (stdout)") $recsOut[$rel] $code (Read-Bytes $oOut)
        Check-Record ($rel + " (stderr)") $recsErr[$rel] $code (Read-Bytes $oErr)
        $idx++
    }
}

# --- 5. link + run -------------------------------------------------------------
if ($modeSet.ContainsKey("link")) {
    $fileDiag = Join-Path $goldenDir "link/diag.txt"
    $fileOut = Join-Path $goldenDir "link/stdout.txt"
    $fileErr = Join-Path $goldenDir "link/stderr.txt"
    $fileSide = Join-Path $goldenDir "link/sideeffects.txt"
    $nondetPath = Join-Path $goldenDir "link/nondet.txt"
    if (-not (Test-Path -LiteralPath $fileDiag)) { Fail "golden section missing: link/diag.txt" }
    Begin-Section "link"
    $linkFixtures = @($all | Where-Object { $_ -like "selfhost/corpus/codegen/*" })
    Write-Output ("[link] " + $linkFixtures.Count + " fixtures ...")
    $recsDiag = Read-Records $fileDiag
    $recsOut = Read-Records $fileOut
    $recsErr = Read-Records $fileErr
    $recsSide = Read-Records $fileSide
    $nondet = @{}
    if (Test-Path -LiteralPath $nondetPath) {
        foreach ($line in (Get-Content -LiteralPath $nondetPath)) {
            if ($line -ne "") { $nondet[$line] = $true }
        }
    }
    $exe = Join-Path $work "app.exe"
    $oOut = Join-Path $work "l.out"
    $oErr = Join-Path $work "l.err"
    $idx = 0
    foreach ($rel in $linkFixtures) {
        Remove-Item -LiteralPath $exe -ErrorAction SilentlyContinue
        $code = Invoke-Merged $driverPath (Get-ModeArgs "link" $rel $exe "") $capture
        Check-Record ($rel + " (link)") $recsDiag[$rel] $code (Normalize-Paths (Read-Bytes $capture) $work)
        if ($recsDiag.ContainsKey($rel) -and $recsDiag[$rel][0] -eq 0 -and $code -eq 0 -and
            -not $nondet.ContainsKey($rel)) {
            if (-not (Test-Path -LiteralPath $exe)) {
                Report-Fail $rel "link succeeded but wrote no executable" "" ""
            } else {
                $cwd = Join-Path $work ("exec_" + $idx)
                New-Item -ItemType Directory -Force -Path $cwd | Out-Null
                $rc = Invoke-Separate $exe @() $oOut $oErr $cwd
                Check-Record ($rel + " (stdout)") $recsOut[$rel] $rc (Read-Bytes $oOut)
                Check-Record ($rel + " (stderr)") $recsErr[$rel] $rc (Read-Bytes $oErr)
                # Every file the executable wrote, and nothing else.
                $actual = @{}
                foreach ($f in (Get-ChildItem -LiteralPath $cwd -Recurse -File | Sort-Object FullName)) {
                    $name = $f.FullName.Substring($cwd.Length).TrimStart('\', '/').Replace('\', '/')
                    $actual[$name] = [System.IO.File]::ReadAllBytes($f.FullName)
                }
                $expectedNames = New-Object System.Collections.Generic.List[string]
                foreach ($k in $recsSide.Keys) {
                    $bar = $k.IndexOf("|")
                    if ($bar -gt 0 -and $k.Substring(0, $bar) -eq $rel) {
                        $expectedNames.Add($k.Substring($bar + 1))
                    }
                }
                foreach ($name in $expectedNames) {
                    $script:Total++
                    if (-not $actual.ContainsKey($name)) {
                        Report-Fail $rel ("side-effect file missing: " + $name) "" ""
                    } elseif (-not (Test-BytesEqual $recsSide[$rel + "|" + $name][1] $actual[$name])) {
                        Report-Fail $rel ("side-effect file differs: " + $name) "" ""
                    }
                }
                foreach ($name in $actual.Keys) {
                    if (-not $expectedNames.Contains($name)) {
                        $script:Total++
                        Report-Fail $rel ("unexpected side-effect file: " + $name) "" ""
                    }
                }
            }
        }
        $idx++
    }
}

# --- summary -------------------------------------------------------------------
Write-Output ("checks: " + $script:Total + ", failures: " + $script:Failed)
if ($script:Failed -ne 0) {
    Write-Output "--- GOLDEN CHECK FAIL ---"
    exit 1
}
Write-Output ("--- GOLDEN CHECK PASS (" + $all.Count + " fixtures) ---")
exit 0
