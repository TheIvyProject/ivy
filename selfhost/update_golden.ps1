# selfhost/update_golden.ps1 - H10.0: re-baseline part of selfhost/golden/ with
# a self-hosted compiler, after the C++ oracle was retired (H9.9).
#
# This is the post-oracle replacement for freeze_golden.ps1.  Instead of
# copying oracle behaviour, it runs a built `ivy_selfhost.exe` over an explicit
# fixture selection and rewrites only those records.  Every other byte in the
# golden tree is left untouched, so the diff is exactly the behaviour change
# under review.
#
# Records use the same format as freeze_golden.ps1 / check_golden.ps1:
#
#   "@@ <key> <nbytes> <exit>\n" followed by <nbytes> raw bytes
#
# and the same normalisation (CRLF -> LF + one final newline for text, `<OUT>`
# for the per-run directory, `<RT>` for the runtime object, `<CLANG>` for the
# linker, `-Wl,/Brepro` dropped).  Objects, `.i` files, `--run` output and the
# linked executable's output stay byte-exact.  check_golden.ps1 is unchanged:
# it compares every record bytewise, whatever its provenance.
#
# Provenance lives in `golden/manifest.txt`: records without a line are the
# original `oracle` baseline; every fixture updated here gets
#
#   provenance=<fixture>|expected:<H10.x>
#
# The script refuses to run without `-Reason H10.x`, never writes outside
# `-Golden`, and keeps all compiler output in a private TEMP directory.  A new
# fixture (not yet in `golden/corpus.txt`) may only be added with a full-mode
# update, otherwise check_golden.ps1 would lack records for the other modes.
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/update_golden.ps1 -Driver <stage1>\ivy_selfhost.exe `
#       -Reason H10.4 -Fixture "h10_C1_*" -IvmDir <std-ivm>
#   ... -Fixture 245_move_pointer.ivy -Modes llvm,link
#   ... -Fixture "*" -Modes c            # re-baseline one section everywhere
#   ... -DryRun                          # print the diff, write nothing
#
# -Filter is accepted as an alias for -Fixture to match freeze_golden.ps1.

param(
    [Parameter(Mandatory=$true)][string]$Driver,
    # Required provenance tag, e.g. H10.4.  Only H10.* is accepted.
    [Parameter(Mandatory=$true)][string]$Reason,
    # Fixture file-name wildcard (matched with -like), e.g. "h10_*" or "*".
    [Alias("Filter")][Parameter(Mandatory=$true)][string]$Fixture,
    [string]$Golden = "selfhost/golden",
    [string]$TempRoot = "",
    # Runtime object the driver links with.  Defaults to $env:IVYRT_LIB_PATH,
    # then `build\rt.obj` (the path the oracle baseline was frozen with).
    [string]$Runtime = "",
    # Linker whose path is folded to <CLANG>.  Defaults to $env:IVY_LINKER,
    # then the first `clang++` on PATH.
    [string]$Linker = "",
    # Directory holding the prebuilt `.ivm` interfaces the corpus imports
    # (`math`, `unique_ptr`, `vector`); appended to the fixture's own `-I`.
    [string]$IvmDir = "",
    [int]$TimeoutSec = 120,
    [string]$Modes = "lex,tokens,ast,hir,mir,llvm,c,o,run,link",
    # How many added/changed keys to list per file.
    [int]$MaxReport = 20,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Fail([string]$msg) {
    Write-Output ("FAILED: " + $msg)
    exit 1
}

$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo

# --- argument validation -------------------------------------------------------
if ($Reason -notmatch '^H10\.[0-9]+$') {
    Fail ("-Reason must look like H10.4 (got '" + $Reason + "'); this gate is for post-oracle fixes")
}
if (-not (Test-Path -LiteralPath $Driver)) { Fail ("driver not found: " + $Driver) }
$driverPath = (Resolve-Path -LiteralPath $Driver).Path

$goldenDir = $Golden
if (-not [System.IO.Path]::IsPathRooted($goldenDir)) { $goldenDir = Join-Path $repo $goldenDir }
$goldenDir = [System.IO.Path]::GetFullPath($goldenDir)
if (-not (Test-Path -LiteralPath $goldenDir)) { Fail ("golden tree not found: " + $goldenDir) }

$manifestPath = Join-Path $goldenDir "manifest.txt"
$corpusPath = Join-Path $goldenDir "corpus.txt"
foreach ($p in @($manifestPath, $corpusPath)) {
    if (-not (Test-Path -LiteralPath $p)) { Fail ("missing from the golden tree: " + $p) }
}
$manifest = @{}
foreach ($line in (Get-Content -LiteralPath $manifestPath)) {
    $eq = $line.IndexOf("=")
    if ($eq -gt 0) { $manifest[$line.Substring(0, $eq)] = $line.Substring($eq + 1) }
}
if ($manifest["format"] -ne "1") { Fail ("unsupported golden format: " + $manifest["format"]) }

$modeSet = @{}
foreach ($m in $Modes.Split(",")) { $k = $m.Trim(); if ($k -ne "") { $modeSet[$k] = $true } }
$allModes = @("lex", "tokens", "ast", "hir", "mir", "llvm", "c", "o", "run", "link")
$fullModes = $true
foreach ($m in $allModes) { if (-not $modeSet.ContainsKey($m)) { $fullModes = $false } }

# --- corpus selection ----------------------------------------------------------
$corpus = @(Get-Content -LiteralPath $corpusPath | Where-Object { $_ -ne "" })
$corpusSet = @{}
foreach ($rel in $corpus) { $corpusSet[$rel] = $true }

$corpusRoot = Join-Path $repo "selfhost\corpus"
$onDisk = @(Get-ChildItem -LiteralPath $corpusRoot -Recurse -Filter *.ivy -File |
            ForEach-Object { $_.FullName.Substring($repo.Length + 1).Replace('\', '/') })
$onDiskSet = @{}
foreach ($rel in $onDisk) { $onDiskSet[$rel] = $true }

# Existing fixtures come from corpus.txt (the authoritative golden list); new
# files on disk only enter the selection if -Fixture explicitly matches them.
$candidates = @($corpus)
foreach ($rel in ($onDisk | Sort-Object)) {
    if (-not $corpusSet.ContainsKey($rel)) { $candidates += $rel }
}
$selected = @($candidates | Sort-Object -Unique | Where-Object { (Split-Path $_ -Leaf) -like $Fixture })
if ($selected.Count -eq 0) { Fail ("no fixtures matched -Fixture '" + $Fixture + "'") }

$newFixtures = @($selected | Where-Object { -not $corpusSet.ContainsKey($_) })
foreach ($rel in $selected) {
    if (-not $onDiskSet.ContainsKey($rel)) { Fail ("selected fixture is missing from the corpus: " + $rel) }
}
if ($newFixtures.Count -gt 0 -and -not $fullModes) {
    Fail ("new fixture(s) need a full-mode update so every golden section has a record; drop -Modes or use the default")
}
Write-Output ("update: " + $selected.Count + " fixture(s), reason " + $Reason)
Write-Output ("golden: " + $goldenDir)
if ($newFixtures.Count -gt 0) {
    Write-Output ("new fixtures: " + ($newFixtures -join ", "))
}

# --- runtime / linker normalisation -------------------------------------------
$script:Latin1 = [System.Text.Encoding]::GetEncoding(28591)

$defaultRt = Join-Path $repo "build\rt.obj"
$rtUsed = $Runtime
if (-not $rtUsed) { $rtUsed = $env:IVYRT_LIB_PATH }
if (-not $rtUsed -and (Test-Path -LiteralPath $defaultRt)) { $rtUsed = $defaultRt }
if ($rtUsed) { $env:IVYRT_LIB_PATH = $rtUsed }

$script:RtPaths = @(
    (Join-Path $repo "build\Release\ivyrt.lib"),
    ((Join-Path $repo "build\Release\ivyrt.lib") -replace '\\', '/'),
    (Join-Path $repo "build\rt.obj"),
    ((Join-Path $repo "build\rt.obj") -replace '\\', '/')
)
if ($rtUsed) {
    $script:RtPaths += $rtUsed
    $script:RtPaths += ($rtUsed -replace '\\', '/')
}

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

function Normalize-Lines([byte[]]$bytes) {
    if ($bytes.Length -eq 0) { return ,[byte[]]@() }
    $s = $script:Latin1.GetString($bytes)
    $s = $s.Replace("`r`n", "`n")
    if ($s.EndsWith("`n")) { $s = $s.Substring(0, $s.Length - 1) }
    return ,$script:Latin1.GetBytes($s + "`n")
}

function Normalize-Paths([byte[]]$bytes, [string]$root) {
    $s = $script:Latin1.GetString($bytes)
    $s = $s.Replace($root, "<OUT>")
    $s = $s.Replace(($root -replace '\\', '/'), "<OUT>")
    foreach ($p in $script:RtPaths) { $s = $s.Replace($p, "<RT>") }
    foreach ($p in $script:ClangPaths) { $s = $s.Replace($p, "<CLANG>") }
    $s = $s.Replace(" -Wl,/Brepro", "")
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

function Get-FixtureDir([string]$rel) {
    return [System.IO.Path]::GetDirectoryName($rel).Replace('\', '/')
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

# --- scratch -------------------------------------------------------------------
$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$work = Join-Path $tempBase ("h100golden_" + $stamp + "_" + $PID)
New-Item -ItemType Directory -Force -Path $work | Out-Null
$env:TEMP = $work
$env:TMP = $work
Write-Output ("run directory: " + $work)
$capture = Join-Path $work "capture.txt"
$objPath = Join-Path $work "out.obj"
$ivmPath = Join-Path $work "out.ivm"
$preprocPath = Join-Path $work "out.i"

# --- record bookkeeping --------------------------------------------------------
$script:RecordUpdates = @{}   # file -> key -> full record bytes
$script:RecordRemovals = @{}  # file -> HashSet[key]
$script:TsvUpdates = @{}      # file -> fixture -> line
$script:DumpUpdated = @{}     # "mode|fixture" -> $true
$script:ChangedFixtures = @{} # fixture -> $true

function Set-Record([string]$file, [string]$key, [int]$exit, [byte[]]$data) {
    $hdr = [System.Text.Encoding]::UTF8.GetBytes("@@ " + $key + " " + $data.Length + " " + $exit + "`n")
    $rec = New-Object byte[] ($hdr.Length + $data.Length)
    [Array]::Copy($hdr, 0, $rec, 0, $hdr.Length)
    if ($data.Length -gt 0) { [Array]::Copy($data, 0, $rec, $hdr.Length, $data.Length) }
    if (-not $script:RecordUpdates.ContainsKey($file)) { $script:RecordUpdates[$file] = @{} }
    $script:RecordUpdates[$file][$key] = $rec
}

function Set-Tsv([string]$file, [string]$fixture, [string]$line) {
    if (-not $script:TsvUpdates.ContainsKey($file)) { $script:TsvUpdates[$file] = @{} }
    $script:TsvUpdates[$file][$fixture] = $line
}

function Get-RecordSpans([byte[]]$b, [string]$what) {
    $spans = New-Object System.Collections.Generic.List[object]
    $i = 0
    while ($i -lt $b.Length) {
        $j = $i
        while ($j -lt $b.Length -and $b[$j] -ne 10) { $j++ }
        if ($j -ge $b.Length) { throw ("truncated record header in " + $what) }
        $hdr = [System.Text.Encoding]::UTF8.GetString($b, $i, $j - $i)
        $parts = $hdr.Split(' ')
        if ($parts.Count -ne 4 -or $parts[0] -ne "@@") { throw ("bad record header in " + $what + ": " + $hdr) }
        $n = [int]$parts[2]
        $start = $j + 1
        if ($start + $n -gt $b.Length) { throw ("truncated record body in " + $what + ": " + $parts[1]) }
        $spans.Add([pscustomobject]@{
            Key = $parts[1]; Exit = [int]$parts[3]; Start = $i; End = $start + $n
        })
        $i = $start + $n
    }
    return ,$spans
}

function Add-FixtureRemovals([string]$file, [string]$fixture) {
    $path = Join-Path $goldenDir $file
    if (-not (Test-Path -LiteralPath $path)) { return }
    $spans = Get-RecordSpans (Read-Bytes $path) $path
    if (-not $script:RecordRemovals.ContainsKey($file)) {
        $script:RecordRemovals[$file] = New-Object 'System.Collections.Generic.HashSet[string]'
    }
    foreach ($s in $spans) {
        if ($s.Key -eq $fixture -or $s.Key.StartsWith($fixture + "|")) {
            [void]$script:RecordRemovals[$file].Add($s.Key)
            $script:ChangedFixtures[$fixture] = $true
        }
    }
}

function Merge-RecordFile([string]$file) {
    $path = Join-Path $goldenDir $file
    $b = Read-Bytes $path
    $spans = Get-RecordSpans $b $path
    $updates = @{}
    if ($script:RecordUpdates.ContainsKey($file)) { $updates = $script:RecordUpdates[$file] }
    $removals = New-Object 'System.Collections.Generic.HashSet[string]'
    if ($script:RecordRemovals.ContainsKey($file)) { $removals = $script:RecordRemovals[$file] }

    $out = New-Object System.Collections.Generic.List[byte]
    $used = @{}
    $added = 0; $changed = 0; $unchanged = 0; $removed = 0
    $listAdded = New-Object System.Collections.Generic.List[string]
    $listChanged = New-Object System.Collections.Generic.List[string]

    foreach ($s in $spans) {
        if ($updates.ContainsKey($s.Key)) {
            $new = [byte[]]$updates[$s.Key]
            $n = $s.End - $s.Start
            $old = New-Object byte[] $n
            [Array]::Copy($b, $s.Start, $old, 0, $n)
            if (Test-BytesEqual $old $new) {
                $unchanged++
            } else {
                $changed++
                $listChanged.Add($s.Key)
                $fixture = $s.Key
                $bar = $fixture.IndexOf("|")
                if ($bar -gt 0) { $fixture = $fixture.Substring(0, $bar) }
                $script:ChangedFixtures[$fixture] = $true
            }
            $out.AddRange($new)
            $used[$s.Key] = $true
        } elseif ($removals.Contains($s.Key)) {
            $removed++
            $fixture = $s.Key
            $bar = $fixture.IndexOf("|")
            if ($bar -gt 0) { $fixture = $fixture.Substring(0, $bar) }
            $script:ChangedFixtures[$fixture] = $true
            continue
        } else {
            $n = $s.End - $s.Start
            $seg = New-Object byte[] $n
            [Array]::Copy($b, $s.Start, $seg, 0, $n)
            $out.AddRange($seg)
        }
    }
    foreach ($key in ($updates.Keys | Sort-Object)) {
        if ($used.ContainsKey($key)) { continue }
        $out.AddRange([byte[]]$updates[$key])
        $added++
        $listAdded.Add($key)
        $fixture = $key
        $bar = $fixture.IndexOf("|")
        if ($bar -gt 0) { $fixture = $fixture.Substring(0, $bar) }
        $script:ChangedFixtures[$fixture] = $true
    }

    if ($added -eq 0 -and $changed -eq 0 -and $removed -eq 0) { return $null }
    return [pscustomobject]@{
        File = $file; Path = $path; Bytes = $out.ToArray()
        Added = $added; Changed = $changed; Removed = $removed; Unchanged = $unchanged
        AddedKeys = $listAdded; ChangedKeys = $listChanged
    }
}

function Merge-TsvFile([string]$file) {
    $path = Join-Path $goldenDir $file
    $lines = @()
    if (Test-Path -LiteralPath $path) { $lines = @(Get-Content -LiteralPath $path | Where-Object { $_ -ne "" }) }
    $updates = @{}
    if ($script:TsvUpdates.ContainsKey($file)) { $updates = $script:TsvUpdates[$file] }
    $out = New-Object System.Collections.Generic.List[string]
    $used = @{}
    $added = 0; $changed = 0; $unchanged = 0
    $listAdded = New-Object System.Collections.Generic.List[string]
    $listChanged = New-Object System.Collections.Generic.List[string]
    foreach ($line in $lines) {
        $tab = $line.IndexOf("`t")
        $key = if ($tab -ge 0) { $line.Substring(0, $tab) } else { $line }
        if ($updates.ContainsKey($key)) {
            $new = $updates[$key]
            if ($new -ceq $line) { $unchanged++ } else {
                $changed++; $listChanged.Add($key)
                $script:ChangedFixtures[$key] = $true
            }
            $out.Add($new)
            $used[$key] = $true
        } else {
            $out.Add($line)
        }
    }
    foreach ($key in ($updates.Keys | Sort-Object)) {
        if ($used.ContainsKey($key)) { continue }
        $out.Add($updates[$key])
        $added++
        $listAdded.Add($key)
        $script:ChangedFixtures[$key] = $true
    }
    if ($added -eq 0 -and $changed -eq 0) { return $null }
    $text = ($out -join "`n")
    if ($text -ne "") { $text += "`n" }
    return [pscustomobject]@{
        File = $file; Path = $path; Text = $text
        Added = $added; Changed = $changed; Removed = 0; Unchanged = $unchanged
        AddedKeys = $listAdded; ChangedKeys = $listChanged
    }
}

# --- existing exceptions / nondeterministic fixtures --------------------------
$exceptionsPath = Join-Path $goldenDir "exceptions.txt"
$script:Exceptions = @{}
$exceptionOrder = New-Object System.Collections.Generic.List[string]
if (Test-Path -LiteralPath $exceptionsPath) {
    foreach ($line in (Get-Content -LiteralPath $exceptionsPath)) {
        $t = $line.Trim()
        if ($t -eq "" -or $t.StartsWith("#")) { continue }
        $f = $t.Split("|")
        if ($f.Count -ge 2) {
            $script:Exceptions[$f[0] + "|" + $f[1]] = $t
            $exceptionOrder.Add($f[0] + "|" + $f[1])
        }
    }
}

$nondetPath = Join-Path $goldenDir "link/nondet.txt"
$nondet = @{}
if (Test-Path -LiteralPath $nondetPath) {
    foreach ($line in (Get-Content -LiteralPath $nondetPath)) {
        if ($line -ne "") { $nondet[$line] = $true }
    }
}

# --- 1. dump modes -------------------------------------------------------------
$dumpModes = @("lex", "tokens", "ast", "hir", "mir", "llvm")
foreach ($mode in $dumpModes) {
    if (-not $modeSet.ContainsKey($mode)) { continue }
    Write-Output ("[dump] --" + $mode + " over " + $selected.Count + " fixtures ...")
    $n = 0
    foreach ($rel in $selected) {
        $code = Invoke-Merged $driverPath (Get-ModeArgs $mode $rel "" "") $capture
        if ($code -lt 0) { Fail ("driver crashed (" + $code + ") on " + $rel + " --" + $mode) }
        $data = Normalize-Paths (Read-Bytes $capture) $work
        Set-Record ("dump/" + $mode + ".txt") $rel $code $data
        $script:DumpUpdated[$mode + "|" + $rel] = $true
        $n++
        if (($n % 200) -eq 0) { Write-Output ("       " + $n + " / " + $selected.Count) }
    }
}

# --- 2. -c: merged output + object/.ivm hashes ---------------------------------
if ($modeSet.ContainsKey("c")) {
    Write-Output ("[c] -c over " + $selected.Count + " fixtures ...")
    $n = 0
    foreach ($rel in $selected) {
        Remove-Item -LiteralPath $objPath, $ivmPath -ErrorAction SilentlyContinue
        $code = Invoke-Merged $driverPath (Get-ModeArgs "c" $rel $objPath "") $capture
        if ($code -lt 0) { Fail ("driver crashed (" + $code + ") on " + $rel + " -c") }
        $data = Normalize-Paths (Read-Bytes $capture) $work
        Set-Record "compile/c.txt" $rel $code $data
        Set-Tsv "compile/objects.tsv" $rel ($rel + "`t" + (Get-Sha $objPath) + "`t" + (Get-Size $objPath) +
            "`t" + (Get-Sha $ivmPath) + "`t" + (Get-Size $ivmPath))
        $n++
        if (($n % 200) -eq 0) { Write-Output ("       " + $n + " / " + $selected.Count) }
    }
}

# --- 3. -o *.i: merged output + preprocessed file hash -------------------------
if ($modeSet.ContainsKey("o")) {
    Write-Output ("[o] -o *.i over " + $selected.Count + " fixtures ...")
    $n = 0
    foreach ($rel in $selected) {
        Remove-Item -LiteralPath $preprocPath -ErrorAction SilentlyContinue
        $code = Invoke-Merged $driverPath (Get-ModeArgs "o" $rel "" $preprocPath) $capture
        if ($code -lt 0) { Fail ("driver crashed (" + $code + ") on " + $rel + " -o") }
        $data = Normalize-Paths (Read-Bytes $capture) $work
        Set-Record "compile/o.txt" $rel $code $data
        Set-Tsv "compile/preproc.tsv" $rel ($rel + "`t" + (Get-Sha $preprocPath) + "`t" + (Get-Size $preprocPath))
        $n++
        if (($n % 200) -eq 0) { Write-Output ("       " + $n + " / " + $selected.Count) }
    }
}

# --- 4. --run: raw stdout/stderr/exit of the interpreter -----------------------
$interpFixtures = @($selected | Where-Object { $_ -like "selfhost/corpus/interp/*" })
if ($modeSet.ContainsKey("run")) {
    Write-Output ("[run] --run over " + $interpFixtures.Count + " fixtures ...")
    $cwd = Join-Path $work "interp"
    New-Item -ItemType Directory -Force -Path $cwd | Out-Null
    $oOut = Join-Path $work "i.out"
    $oErr = Join-Path $work "i.err"
    foreach ($rel in $interpFixtures) {
        $code = Invoke-Separate $driverPath (Get-ModeArgs "run" $rel "" "") $oOut $oErr $cwd
        if ($code -lt 0) { Fail ("driver crashed (" + $code + ") on " + $rel + " --run") }
        Set-Record "interp/stdout.txt" $rel $code (Read-Bytes $oOut)
        Set-Record "interp/stderr.txt" $rel $code (Read-Bytes $oErr)
    }
}

# --- 5. link + run -------------------------------------------------------------
$linkFixtures = @($selected | Where-Object { $_ -like "selfhost/corpus/codegen/*" })
if ($modeSet.ContainsKey("link")) {
    Write-Output ("[link] link + run over " + $linkFixtures.Count + " fixtures ...")
    $oOut = Join-Path $work "l.out"
    $oErr = Join-Path $work "l.err"
    $exe = Join-Path $work "app.exe"
    $i = 0
    foreach ($rel in $linkFixtures) {
        Remove-Item -LiteralPath $exe -ErrorAction SilentlyContinue
        $code = Invoke-Merged $driverPath (Get-ModeArgs "link" $rel $exe "") $capture
        Set-Record "link/diag.txt" $rel $code (Normalize-Paths (Read-Bytes $capture) $work)
        if ($code -eq 0) {
            if (-not (Test-Path -LiteralPath $exe)) { Fail ("link succeeded without an executable: " + $rel) }
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
                # The fixture's own output is not reproducible: compare the link
                # step only and list it in link/nondet.txt (same as freeze).
                if (-not $nondet.ContainsKey($rel)) { $script:ChangedFixtures[$rel] = $true }
                $nondet[$rel] = $true
                Add-FixtureRemovals "link/stdout.txt" $rel
                Add-FixtureRemovals "link/stderr.txt" $rel
                Add-FixtureRemovals "link/sideeffects.txt" $rel
            } else {
                if ($nondet.ContainsKey($rel)) {
                    [void]$nondet.Remove($rel)
                    $script:ChangedFixtures[$rel] = $true
                }
                Set-Record "link/stdout.txt" $rel $runs[0][0] $runs[0][1]
                Set-Record "link/stderr.txt" $rel $runs[0][0] $runs[0][2]
                Add-FixtureRemovals "link/sideeffects.txt" $rel
                foreach ($k in ($runs[0][3].Keys | Sort-Object)) {
                    # The bytes live in the first run's directory.
                    $src = Join-Path (Join-Path $work ("exec\" + $i + "_1")) $k
                    Set-Record "link/sideeffects.txt" ($rel + "|" + $k) 0 (Read-Bytes $src)
                }
            }
        } else {
            # Link failed: there is no executable output for this fixture.
            Add-FixtureRemovals "link/stdout.txt" $rel
            Add-FixtureRemovals "link/stderr.txt" $rel
            Add-FixtureRemovals "link/sideeffects.txt" $rel
            if ($nondet.ContainsKey($rel)) {
                [void]$nondet.Remove($rel)
                $script:ChangedFixtures[$rel] = $true
            }
        }
        $i++
        if (($i % 50) -eq 0) { Write-Output ("       " + $i + " / " + $linkFixtures.Count) }
    }
}

# --- 6. exceptions: a re-run record is no longer an oracle crash ---------------
foreach ($key in $script:DumpUpdated.Keys) {
    if ($script:Exceptions.ContainsKey($key)) {
        $fixture = $key.Substring($key.IndexOf("|") + 1)
        [void]$script:Exceptions.Remove($key)
        $script:ChangedFixtures[$fixture] = $true
    }
}
function Write-Exceptions() {
    $lines = @($script:Exceptions.Keys | Sort-Object)
    $text = "# mode|fixture|reason - oracle crash records (no reference output).`n"
    if ($lines.Count -gt 0) { $text += (($lines | ForEach-Object { $script:Exceptions[$_] }) -join "`n") + "`n" }
    return $text
}

# --- 7. merge every touched section -------------------------------------------
$touched = New-Object System.Collections.Generic.List[string]
foreach ($f in $script:RecordUpdates.Keys) { if (-not $touched.Contains($f)) { $touched.Add($f) } }
foreach ($f in $script:RecordRemovals.Keys) { if (-not $touched.Contains($f)) { $touched.Add($f) } }

$results = New-Object System.Collections.Generic.List[object]
foreach ($file in ($touched | Sort-Object)) {
    $r = Merge-RecordFile $file
    if ($r -ne $null) { $results.Add($r) }
}
foreach ($file in ($script:TsvUpdates.Keys | Sort-Object)) {
    $r = Merge-TsvFile $file
    if ($r -ne $null) { $results.Add($r) }
}

$newCorpus = @($corpus)
foreach ($rel in $newFixtures) { $newCorpus += $rel }
$newCorpus = @($newCorpus | Sort-Object -Unique)
$corpusChanged = ($newFixtures.Count -gt 0)

$nondetText = ""
$nondetKeys = @($nondet.Keys | Sort-Object)
if ($nondetKeys.Count -gt 0) { $nondetText = ($nondetKeys -join "`n") + "`n" }
$oldNondetText = ""
if (Test-Path -LiteralPath $nondetPath) { $oldNondetText = [System.IO.File]::ReadAllText($nondetPath) }
$nondetChanged = $false
if ($modeSet.ContainsKey("link")) {
    $nondetChanged = ($nondetText -cne $oldNondetText)
}

$excText = Write-Exceptions
$oldExcText = ""
if (Test-Path -LiteralPath $exceptionsPath) {
    $oldExcText = (Get-Content -LiteralPath $exceptionsPath -Raw)
    if ($oldExcText -eq $null) { $oldExcText = "" }
}
$excChanged = ($excText -cne $oldExcText)

# --- 8. manifest: counts + provenance ------------------------------------------
$script:ManifestLines = @(Get-Content -LiteralPath $manifestPath)
$manifestText = ($script:ManifestLines -join "`n") + "`n"
$changedFixtures = @($script:ChangedFixtures.Keys | Sort-Object)
$manifestChanged = $false
function Set-ManifestLine([string]$key, [string]$value) {
    for ($i = 0; $i -lt $script:ManifestLines.Count; $i++) {
        if ($script:ManifestLines[$i].StartsWith($key + "=")) {
            $script:ManifestLines[$i] = $key + "=" + $value
            return
        }
    }
    $script:ManifestLines += ($key + "=" + $value)
}

if ($results.Count -gt 0 -or $corpusChanged -or $nondetChanged -or $excChanged -or $changedFixtures.Count -gt 0) {
    Set-ManifestLine "fixtures" ([string]$newCorpus.Count)
    Set-ManifestLine "interp_fixtures" ([string](@($newCorpus | Where-Object { $_ -like "selfhost/corpus/interp/*" }).Count))
    Set-ManifestLine "link_fixtures" ([string](@($newCorpus | Where-Object { $_ -like "selfhost/corpus/codegen/*" }).Count))
    Set-ManifestLine "nondet" ([string]$nondetKeys.Count)
    Set-ManifestLine "updated" ((Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"))
    Set-ManifestLine "updated_reason" $Reason

    # Replace the provenance line of every fixture touched by this run.
    $keep = New-Object System.Collections.Generic.List[string]
    foreach ($line in $script:ManifestLines) {
        if ($line.StartsWith("provenance=")) {
            $v = $line.Substring("provenance=".Length)
            $bar = $v.IndexOf("|")
            $fx = if ($bar -gt 0) { $v.Substring(0, $bar) } else { $v }
            if ($changedFixtures -contains $fx) { continue }
        }
        $keep.Add($line)
    }
    foreach ($fx in $changedFixtures) { $keep.Add("provenance=" + $fx + "|expected:" + $Reason) }
    $script:ManifestLines = @($keep)
    $newManifestText = ($script:ManifestLines -join "`n") + "`n"
    $manifestChanged = ($newManifestText -cne $manifestText)
}

# --- 9. report -----------------------------------------------------------------
$totalAdded = 0; $totalChanged = 0; $totalRemoved = 0
foreach ($r in $results) {
    $totalAdded += $r.Added; $totalChanged += $r.Changed; $totalRemoved += $r.Removed
    Write-Output ("[" + $r.File + "] +" + $r.Added + " ~" + $r.Changed + " -" + $r.Removed +
                  " (unchanged " + $r.Unchanged + ")")
    foreach ($k in @($r.AddedKeys | Select-Object -First $MaxReport)) { Write-Output ("  + " + $k) }
    foreach ($k in @($r.ChangedKeys | Select-Object -First $MaxReport)) { Write-Output ("  ~ " + $k) }
    if (($r.AddedKeys.Count + $r.ChangedKeys.Count) -gt $MaxReport) {
        Write-Output ("  ... " + (($r.AddedKeys.Count + $r.ChangedKeys.Count) - $MaxReport) + " more")
    }
}
Write-Output ("records: +" + $totalAdded + " ~" + $totalChanged + " -" + $totalRemoved)
if ($corpusChanged) { Write-Output ("corpus.txt: +" + $newFixtures.Count + " fixture(s)") }
if ($nondetChanged) { Write-Output ("link/nondet.txt: " + $nondetKeys.Count + " fixture(s)") }
if ($excChanged) { Write-Output ("exceptions.txt: " + $script:Exceptions.Count + " record(s)") }
if ($manifestChanged) { Write-Output ("manifest.txt: provenance -> expected:" + $Reason) }

$nothing = ($results.Count -eq 0 -and -not $corpusChanged -and -not $nondetChanged -and -not $excChanged)
if ($nothing) {
    Write-Output "no golden changes"
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    exit 0
}
if ($DryRun) {
    Write-Output "--- DRY RUN: nothing written ---"
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    exit 0
}

# Write only the files that actually differ; everything else keeps its bytes.
foreach ($r in $results) {
    $dir = Split-Path $r.Path -Parent
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    if ($r.PSObject.Properties.Name -contains "Text") {
        [System.IO.File]::WriteAllText($r.Path, $r.Text, (New-Object System.Text.UTF8Encoding($false)))
    } else {
        [System.IO.File]::WriteAllBytes($r.Path, $r.Bytes)
    }
}
if ($corpusChanged) {
    [System.IO.File]::WriteAllText($corpusPath, (($newCorpus -join "`n") + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
}
if ($nondetChanged) {
    New-Item -ItemType Directory -Force -Path (Split-Path $nondetPath -Parent) | Out-Null
    [System.IO.File]::WriteAllText($nondetPath, $nondetText, (New-Object System.Text.UTF8Encoding($false)))
}
if ($excChanged) {
    [System.IO.File]::WriteAllText($exceptionsPath, $excText, (New-Object System.Text.UTF8Encoding($false)))
}
if ($manifestChanged) {
    [System.IO.File]::WriteAllText($manifestPath, (($script:ManifestLines -join "`n") + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
}

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
Write-Output ("GOLDEN UPDATED (" + $changedFixtures.Count + " fixture(s), " + $Reason + ")")
exit 0
