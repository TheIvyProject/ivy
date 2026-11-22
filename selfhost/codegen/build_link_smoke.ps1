# selfhost/codegen/build_link_smoke.ps1 - H6.4 linker-orchestration harness.
#
# End-to-end check of `codegen::Linker` (mirror of CodeGen::linkExecutable in
# src/codegen/codegen.cpp:2810-3063):
#
#   source -> selfhost frontend/emitter (cgdrive) -> IR text
#          -> selfhost Linker (linkdrive)          -> temp object -> linker
#          -> native executable -> run             -> stdout/exit status
#
# For every file in -SmokeFiles the harness builds the executable twice - once
# through `linkdrive` and once through the oracle (`ivy -o`) - runs both, and
# compares stdout, stderr and exit status as RAW BYTES. It also checks that the
# `<stem>__tmp.obj` object is deleted after a successful link.
#
# Two negative checks, both compared byte-for-byte against the oracle:
#   * link failure: `IVY_LINKER=where.exe` makes the linker exit non-zero; the
#     oracle-style `linker failed (exit code N): <command>` diagnostic must
#     match, and the temporary object must be kept for debugging;
#   * object-emission failure: a fixture whose IR the oracle itself rejects
#     (`sext ptr` bug-compat) must fail with the same stderr and leave no
#     temporary object.
#
# One edge check: the same fixture is also linked into a directory whose name
# contains a space, because `system()` runs through cmd.exe and the quoting in
# `Linker::link_text` must survive it.
#
# H9.8: fixtures that use `import cpp "local.hpp"` / `import c "local.h"` are
# linked with those local headers too.  The harness resolves them the way the
# oracle does (source directory first), passes them to `linkdrive`, and the
# selfhost linker compiles them with `-x c++ -c` / `-x c -c` exactly like
# `codegen.cpp:2869-2960`.  The comparison stays end-to-end: both executables
# must print the same bytes and exit with the same status.
#
# Every artefact (.obj/.ivm/.exe) is written under a fresh per-run directory in
# TEMP; nothing is written into the repository working tree.
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/codegen/build_link_smoke.ps1
#   powershell -NoProfile -File selfhost/codegen/build_link_smoke.ps1 -TempRoot C:\scratch\h64
#
# Exit 0 iff every executable, the link-failure diagnostic, and the
# emission-failure diagnostic match the oracle.

param(
    [string]$Dir = "selfhost/corpus/codegen",
    [string]$TempRoot = "",
    [string]$LlvmRoot = "D:\Cpp\Clang",
    [string]$IvyrtLib = "",
    [string[]]$SmokeFiles = @(
        "01_return_const.ivy",
        "02_int_ops.ivy",
        "07_ops.ivy",
        "101_struct_array_float.ivy",
        "105_print_ref_char.ivy",
        "164_printf_basic.ivy",
        "235_inherit_virtual_dtor.ivy",
        "242_move_struct.ivy",
        "243_import_cpp.ivy",
        "244_import_c.ivy"
    ),
    [string]$NegativeFile = "01_return_const.ivy",
    [string]$EmitFailFile = "103_print_ref.ivy"
)

$ErrorActionPreference = "Stop"

# `powershell -File ... -SmokeFiles a.ivy,b.ivy` arrives as one comma-joined
# string, so normalise it back to an array.
$SmokeFiles = @($SmokeFiles | ForEach-Object { $_ -split ',' } |
                Where-Object { $_ -ne "" })

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$ivy = Join-Path $repo "build\Release\ivy.exe"
$clang = Join-Path $LlvmRoot "bin\clang.exe"
$llvmConfig = Join-Path $LlvmRoot "bin\llvm-config.exe"
$llvmLib = Join-Path $LlvmRoot "lib"

# The oracle bakes IVYRT_LIB_PATH at CMake configure time (the pre-H9.1
# archive `build/Release/ivyrt.lib`); `linkdrive` receives the H9.1 runtime
# object.  H9.1: the runtime is std/rt.ivy built to build/rt.obj.
if ($IvyrtLib -eq "") {
    $IvyrtLib = $repo.Replace('\', '/') + "/build/rt.obj"
}

# Link-failure diagnostics print the linker command line verbatim, including
# the runtime path.  The two binaries were built with different runtime
# spellings, so normalize both to `<RT>` before comparing the bytes.
$rtSpellings = @(($repo.Replace('\', '/') + "/build/Release/ivyrt.lib"),
                 ($IvyrtLib -replace '\\', '/'), $IvyrtLib)
function Normalize-Rt([string]$text) {
    foreach ($p in $rtSpellings) { $text = $text.Replace($p, "<RT>") }
    # H9.7: the selfhost linker passes `-Wl,/Brepro` (reproducible PE) and the
    # oracle does not, so the command line in a link-failure diagnostic has one
    # extra flag on the selfhost side.
    $text = $text.Replace(" -Wl,/Brepro", "")
    return $text
}

function Fail([string]$message) {
    Write-Output ("FAIL: " + $message)
    exit 1
}

if (-not (Test-Path -LiteralPath $ivy)) { Fail ("oracle not found: " + $ivy) }
if (-not (Test-Path -LiteralPath $clang)) { Fail ("clang not found: " + $clang) }
if (-not (Test-Path -LiteralPath $llvmConfig)) { Fail ("llvm-config not found: " + $llvmConfig) }
if (-not (Test-Path -LiteralPath $IvyrtLib)) { Fail ("runtime object not found: " + $IvyrtLib) }

$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$runDir = Join-Path $tempBase ("h64link_" + $stamp + "_" + $PID)
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$outDir = Join-Path $runDir "out"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Every compiler/linker/run process inherits this isolated TEMP/TMP.
$env:TEMP = $runDir
$env:TMP = $runDir
# Never let an inherited IVY_LINKER change the success path.
Remove-Item Env:\IVY_LINKER -ErrorAction SilentlyContinue

function Quote-Arg([string]$s) {
    if ($s -match '[\s"]') {
        return '"' + ($s -replace '"', '\"') + '"'
    }
    return $s
}

# Run one process with byte-exact, separate stdout/stderr files and return its
# exit status. Optional stdin is a file whose text bytes are forwarded.
# $TimeoutMs = 0 waits forever; otherwise the process is killed on timeout and
# -1 is returned.
function Invoke-Separate([string]$exe, [string[]]$argList, [string]$stdinPath,
                         [string]$outPath, [string]$errPath,
                         [int]$TimeoutMs = 0) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = $repo
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

function Show-Bytes([byte[]]$b) {
    if ($b.Length -eq 0) { return "(empty)" }
    return [BitConverter]::ToString($b)
}

# ---------------------------------------------------------------------------
# LLVM libraries + system libraries. `--libs x86` pulls in core, target,
# irreader, codegen, asmprinter, ... (required by H6.3/H6.4). System libs are
# rewritten to -l<name>; missing ones are skipped.
# ---------------------------------------------------------------------------
$llvmLibs = @((& $llvmConfig --libs x86) -split '\s+' | Where-Object { $_ -ne "" })
if ($llvmLibs.Count -eq 0) { Fail "llvm-config --libs x86 returned nothing" }

$systemArgs = @()
$systemNames = @((& $llvmConfig --system-libs) -split '\s+' | Where-Object { $_ -ne "" })
foreach ($n in $systemNames) {
    if ($n -match '\.lib$') {
        $base = $n.Substring(0, $n.Length - 4)
        $installed = Test-Path -LiteralPath (Join-Path $llvmLib $n)
        if (-not $installed -and $env:LIB) {
            foreach ($libDir in ($env:LIB -split ';')) {
                if ($libDir -and (Test-Path -LiteralPath (Join-Path $libDir $n))) {
                    $installed = $true
                    break
                }
            }
        }
        if (-not $installed) {
            Write-Output ("      note: skipping missing system lib " + $n)
            continue
        }
        $systemArgs += ("-l" + $base)
    } else {
        $systemArgs += $n
    }
}

# ---------------------------------------------------------------------------
# [1/6] Build the selfhost frontend/emitter chain and cgdrive.
# ---------------------------------------------------------------------------
Write-Output "[1/6] building selfhost IR emitter driver ..."
$moduleDirs = @("selfhost\common", "selfhost\parsing", "selfhost\hir", "selfhost\mir",
                "selfhost\codegen", "selfhost\app", "std")
$inc = @("-I", $runDir)
foreach ($md in $moduleDirs) { $inc += @("-I", (Join-Path $repo $md)) }

$modules = @(
    @("std", "io"),
    @("selfhost\common", "diagnostic"),
    @("selfhost\parsing", "token"),
    @("selfhost\parsing", "lexer"),
    @("selfhost\parsing", "preprocessor"),
    @("selfhost\parsing", "ast"),
    @("selfhost\parsing", "parser"),
    @("selfhost\parsing", "module_io"),
    @("selfhost\app", "cli"),
    @("selfhost\app", "imports"),
    @("selfhost\hir", "hir"),
    @("selfhost\hir", "hir_builder"),
    @("selfhost\mir", "mir"),
    @("selfhost\mir", "mir_builder"),
    @("selfhost\codegen", "emitter"),
    @("selfhost\app", "pipeline")
)

foreach ($m in $modules) {
    $name = $m[1]
    $src = Join-Path $repo (Join-Path $m[0] ($name + ".ivy"))
    $obj = Join-Path $runDir ($name + ".obj")
    $bOut = Join-Path $runDir ("build_" + $name + ".out")
    $bErr = Join-Path $runDir ("build_" + $name + ".err")
    $bArgs = @($src) + $inc + @("-c", "-o", $obj)
    $code = Invoke-Separate $ivy $bArgs "" $bOut $bErr
    if ($code -ne 0) {
        Write-Output ("BUILD FAILED (" + $name + ", exit " + $code + ")")
        if (Test-Path -LiteralPath $bErr) { Write-Output ([System.IO.File]::ReadAllText($bErr)) }
        exit 1
    }
}

$cgdrive = Join-Path $runDir "cgdrive.exe"
$dOut = Join-Path $runDir "build_cgdrive.out"
$dErr = Join-Path $runDir "build_cgdrive.err"
$dArgs = @((Join-Path $repo "selfhost\codegen\cgdrive.ivy")) + $inc + @("-o", $cgdrive)
$code = Invoke-Separate $ivy $dArgs "" $dOut $dErr
if ($code -ne 0) {
    Write-Output ("BUILD FAILED (cgdrive, exit " + $code + ")")
    if (Test-Path -LiteralPath $dErr) { Write-Output ([System.IO.File]::ReadAllText($dErr)) }
    exit 1
}

# ---------------------------------------------------------------------------
# [2/6] Build codegen.ivy (binding + ObjEmit + Linker) and linkdrive.ivy.
# ---------------------------------------------------------------------------
Write-Output "[2/6] building linker driver ..."
$codegenObj = Join-Path $runDir "codegen.obj"
$cOut = Join-Path $runDir "build_codegen.out"
$cErr = Join-Path $runDir "build_codegen.err"
$cArgs = @((Join-Path $repo "selfhost\codegen\codegen.ivy")) + $inc + @("-c", "-o", $codegenObj)
$code = Invoke-Separate $ivy $cArgs "" $cOut $cErr
if ($code -ne 0 -or -not (Test-Path -LiteralPath $codegenObj)) {
    Write-Output ("BUILD FAILED (codegen, exit " + $code + ")")
    if (Test-Path -LiteralPath $cErr) { Write-Output ([System.IO.File]::ReadAllText($cErr)) }
    exit 1
}
$codegenIvm = Join-Path $runDir "codegen.ivm"
if (-not (Test-Path -LiteralPath $codegenIvm)) { Fail ("module interface missing: " + $codegenIvm) }

$linkdriveObj = Join-Path $runDir "linkdrive.obj"
$oOut = Join-Path $runDir "build_linkdrive.out"
$oErr = Join-Path $runDir "build_linkdrive.err"
$oArgs = @((Join-Path $repo "selfhost\codegen\linkdrive.ivy")) + $inc + @("-c", "-o", $linkdriveObj)
$code = Invoke-Separate $ivy $oArgs "" $oOut $oErr
if ($code -ne 0 -or -not (Test-Path -LiteralPath $linkdriveObj)) {
    Write-Output ("BUILD FAILED (linkdrive, exit " + $code + ")")
    if (Test-Path -LiteralPath $oErr) { Write-Output ([System.IO.File]::ReadAllText($oErr)) }
    exit 1
}

# ---------------------------------------------------------------------------
# [3/6] Build the Ivy runtime (std/rt.ivy -> rt.obj) and link linkdrive.exe.
# ---------------------------------------------------------------------------
Write-Output "[3/6] linking linker driver ..."
$ivyrtObj = Join-Path $runDir "rt.obj"
$rOut = Join-Path $runDir "build_rt.out"
$rErr = Join-Path $runDir "build_rt.err"
$rArgs = @("-c", (Join-Path $repo "std\rt.ivy"), "-I", (Join-Path $repo "std"),
           "-o", $ivyrtObj)
$code = Invoke-Separate $ivy $rArgs "" $rOut $rErr
if ($code -ne 0 -or -not (Test-Path -LiteralPath $ivyrtObj)) {
    Write-Output ("BUILD FAILED (rt, exit " + $code + ")")
    if (Test-Path -LiteralPath $rErr) { Write-Output ([System.IO.File]::ReadAllText($rErr)) }
    exit 1
}

$linkdriveExe = Join-Path $runDir "linkdrive.exe"
$lOut = Join-Path $runDir "link_linkdrive.out"
$lErr = Join-Path $runDir "link_linkdrive.err"
$lArgs = @($linkdriveObj, $codegenObj, $ivyrtObj) + $llvmLibs + $systemArgs +
         @("-o", $linkdriveExe)
$code = Invoke-Separate $clang $lArgs "" $lOut $lErr
if ($code -ne 0 -or -not (Test-Path -LiteralPath $linkdriveExe)) {
    Write-Output ("BUILD FAILED (linkdrive link, exit " + $code + ")")
    if (Test-Path -LiteralPath $lErr) { Write-Output ([System.IO.File]::ReadAllText($lErr)) }
    exit 1
}
Write-Output ("      scratch: " + $runDir)

$dirAbs = $Dir
if (-not [System.IO.Path]::IsPathRooted($dirAbs)) { $dirAbs = Join-Path $repo $Dir }
if (-not (Test-Path -LiteralPath $dirAbs)) {
    Write-Output ("no corpus directory: " + $dirAbs)
    exit 1
}

$repoPrefix = $repo.TrimEnd('\') + '\'
function Get-FixtureArg([string]$full) {
    if ($full.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($repoPrefix.Length).Replace('\', '/')
    }
    return $full
}

# H9.8: local headers named by `import cpp` / `import c`, resolved like the
# oracle (`main.cpp:1207-1260`): `<source dir>/<name>` first; an angle header
# that is not on disk is a system header and stays declaration-only.  Returns
# the paths in the spelling the oracle would use (relative to the repository).
function Get-ImportHeaders([string]$srcArg, [string]$fullSrc) {
    $text = [System.IO.File]::ReadAllText($fullSrc)
    $parent = Split-Path $srcArg -Parent
    $cpp = New-Object System.Collections.Generic.List[string]
    $c = New-Object System.Collections.Generic.List[string]
    foreach ($m in [regex]::Matches($text, 'import\s+cpp\s+(?:<([^>]*)>|"([^"]*)")')) {
        $n = if ($m.Groups[1].Success) { $m.Groups[1].Value } else { $m.Groups[2].Value }
        if (-not $n) { continue }
        $cand = if ($parent) { Join-Path $parent $n } else { $n }
        if (Test-Path -LiteralPath (Join-Path $repo $cand)) { $cpp.Add($cand) }
    }
    foreach ($m in [regex]::Matches($text, 'import\s+c\s+(?:<([^>]*)>|"([^"]*)")')) {
        $n = if ($m.Groups[1].Success) { $m.Groups[1].Value } else { $m.Groups[2].Value }
        if (-not $n) { continue }
        $cand = if ($parent) { Join-Path $parent $n } else { $n }
        if (Test-Path -LiteralPath (Join-Path $repo $cand)) { $c.Add($cand) }
    }
    return @{ Cpp = $cpp; C = $c }
}

# ---------------------------------------------------------------------------
# [4/6] Link smoke: selfhost Linker vs `ivy -o`, then run both executables.
# ---------------------------------------------------------------------------
Write-Output "[4/6] link smoke ..."
$pass = 0
$fail = 0
$skipped = 0
$failures = New-Object System.Collections.Generic.List[string]

foreach ($name in $SmokeFiles) {
    $src = Join-Path $dirAbs $name
    if (-not (Test-Path -LiteralPath $src)) {
        Write-Output ("SKIP " + $name + " (not in corpus)")
        $skipped = $skipped + 1
        continue
    }
    $arg = Get-FixtureArg $src
    $safe = [System.IO.Path]::GetFileNameWithoutExtension($name) -replace '[^A-Za-z0-9_.-]', '_'

    # IR text through the selfhost emitter.
    $irPath = Join-Path $runDir ($safe + ".self.ll")
    $cgIn = Join-Path $runDir ($safe + ".cg.in")
    [System.IO.File]::WriteAllText($cgIn, ($arg + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    $cgCode = Invoke-Separate $cgdrive @() $cgIn $irPath `
                  (Join-Path $runDir ($safe + ".cg.err"))

    # Selfhost link: linkdrive emits <stem>__tmp.obj then runs clang++.
    $selfExe = Join-Path $outDir ($safe + ".self.exe")
    $ldIn = Join-Path $runDir ($safe + ".link.in")
    $hdr = Get-ImportHeaders $arg $src
    $hdrLines = $hdr.Cpp.Count.ToString() + "`n"
    foreach ($h in $hdr.Cpp) { $hdrLines += $h + "`n" }
    $hdrLines += $hdr.C.Count.ToString() + "`n"
    foreach ($h in $hdr.C) { $hdrLines += $h + "`n" }
    [System.IO.File]::WriteAllText($ldIn,
        ($irPath + "`n" + $selfExe + "`n" + $arg + "`n" + $IvyrtLib + "`n" + $hdrLines),
        (New-Object System.Text.UTF8Encoding($false)))
    $ldCode = Invoke-Separate $linkdriveExe @() $ldIn `
                  (Join-Path $runDir ($safe + ".link.out")) `
                  (Join-Path $runDir ($safe + ".link.err"))
    $tmpObj = Join-Path $outDir ($safe + ".self__tmp.obj")
    $tmpGone = -not (Test-Path -LiteralPath $tmpObj)

    # Oracle link.
    $oracleExe = Join-Path $outDir ($safe + ".oracle.exe")
    $oBuildCode = Invoke-Separate $ivy @("-o", $oracleExe, $arg) "" `
                      (Join-Path $runDir ($safe + ".oracle.build.out")) `
                      (Join-Path $runDir ($safe + ".oracle.build.err"))

    if ($cgCode -ne 0 -or $ldCode -ne 0 -or $oBuildCode -ne 0 -or
        -not (Test-Path -LiteralPath $selfExe) -or -not $tmpGone) {
        Write-Output ("FAIL " + $name + " (build: cgdrive=" + $cgCode +
                      " linkdrive=" + $ldCode + " oracle=" + $oBuildCode +
                      " exe=" + (Test-Path -LiteralPath $selfExe) +
                      " tmp_removed=" + $tmpGone + ")")
        foreach ($e in @(($safe + ".link.err"), ($safe + ".oracle.build.err"))) {
            $p = Join-Path $runDir $e
            if (Test-Path -LiteralPath $p) { Write-Output ([System.IO.File]::ReadAllText($p)) }
        }
        $fail = $fail + 1
        $failures.Add($name)
        continue
    }

    $sRunOut = Join-Path $runDir ($safe + ".self.run.out")
    $sRunErr = Join-Path $runDir ($safe + ".self.run.err")
    $oRunOut = Join-Path $runDir ($safe + ".oracle.run.out")
    $oRunErr = Join-Path $runDir ($safe + ".oracle.run.err")
    $sRunCode = Invoke-Separate $selfExe @() "" $sRunOut $sRunErr 15000
    $oRunCode = Invoke-Separate $oracleExe @() "" $oRunOut $oRunErr 15000

    $outOk = Test-BytesEqual ([System.IO.File]::ReadAllBytes($oRunOut)) `
                             ([System.IO.File]::ReadAllBytes($sRunOut))
    $errOk = Test-BytesEqual ([System.IO.File]::ReadAllBytes($oRunErr)) `
                             ([System.IO.File]::ReadAllBytes($sRunErr))
    $runOk = ($sRunCode -eq $oRunCode) -and ($sRunCode -ge 0)

    if ($outOk -and $errOk -and $runOk) {
        Write-Output ("PASS " + $name + " exit=" + $sRunCode +
                      " out=" + ([System.IO.File]::ReadAllBytes($sRunOut).Length) + "B")
        $pass = $pass + 1
    } else {
        Write-Output ("FAIL " + $name + " run (self=" + $sRunCode + " oracle=" + $oRunCode + ")")
        if (-not $outOk) {
            Write-Output ("     stdout diff at byte " +
                (First-Diff ([System.IO.File]::ReadAllBytes($oRunOut)) `
                            ([System.IO.File]::ReadAllBytes($sRunOut))))
        }
        if (-not $errOk) {
            Write-Output ("     stderr diff at byte " +
                (First-Diff ([System.IO.File]::ReadAllBytes($oRunErr)) `
                            ([System.IO.File]::ReadAllBytes($sRunErr))))
        }
        $fail = $fail + 1
        $failures.Add($name)
    }
}

# ---------------------------------------------------------------------------
# [5/6] Negative checks: link failure and object-emission failure.
# ---------------------------------------------------------------------------
Write-Output "[5/6] negative diagnostics ..."
$negPass = 0
$negFail = 0
$edgePass = 0
$edgeFail = 0

# (a) Link failure: `where.exe` exits 2 and prints its own error. Both sides
# must keep the temp object and produce the same stderr bytes.
$negSrc = Join-Path $dirAbs $NegativeFile
if (Test-Path -LiteralPath $negSrc) {
    $negArg = Get-FixtureArg $negSrc
    $negIr = Join-Path $runDir "neg.self.ll"
    $negCgIn = Join-Path $runDir "neg.cg.in"
    [System.IO.File]::WriteAllText($negCgIn, ($negArg + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    $negCg = Invoke-Separate $cgdrive @() $negCgIn $negIr `
                 (Join-Path $runDir "neg.cg.err")

    $negExe = Join-Path $outDir "neg__link_fail.exe"
    $negObj = Join-Path $outDir "neg__link_fail__tmp.obj"
    Remove-Item -LiteralPath $negExe, $negObj -Force -ErrorAction SilentlyContinue

    $env:IVY_LINKER = "C:\Windows\System32\where.exe"
    $oNegOut = Join-Path $runDir "neg.oracle.out"
    $oNegErr = Join-Path $runDir "neg.oracle.err"
    $oNegCode = Invoke-Separate $ivy @("-o", $negExe, $negArg) "" $oNegOut $oNegErr
    $oNegTmp = Test-Path -LiteralPath $negObj
    # Start the selfhost run from the same state the oracle started from.
    Remove-Item -LiteralPath $negObj -Force -ErrorAction SilentlyContinue

    $negIn = Join-Path $runDir "neg.link.in"
    [System.IO.File]::WriteAllText($negIn,
        ($negIr + "`n" + $negExe + "`n" + $negArg + "`n" + $IvyrtLib + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    $sNegOut = Join-Path $runDir "neg.self.out"
    $sNegErr = Join-Path $runDir "neg.self.err"
    $sNegCode = Invoke-Separate $linkdriveExe @() $negIn $sNegOut $sNegErr
    $sNegTmp = Test-Path -LiteralPath $negObj
    Remove-Item Env:\IVY_LINKER -ErrorAction SilentlyContinue

    $oNegText = Normalize-Rt ([System.Text.Encoding]::GetEncoding(28591).GetString(
        [System.IO.File]::ReadAllBytes($oNegErr)))
    $sNegText = Normalize-Rt ([System.Text.Encoding]::GetEncoding(28591).GetString(
        [System.IO.File]::ReadAllBytes($sNegErr)))
    $diagOk = ($oNegText -eq $sNegText)
    if ($negCg -eq 0 -and $oNegCode -eq 1 -and $sNegCode -eq 1 -and
        $oNegTmp -and $sNegTmp -and $diagOk) {
        Write-Output ("PASS link-failure diagnostics (" + $NegativeFile +
                      ", exit 1, temp object kept)")
        $negPass = $negPass + 1
    } else {
        Write-Output ("FAIL link-failure diagnostics (cgdrive=" + $negCg +
                      " oracle=" + $oNegCode + " self=" + $sNegCode +
                      " oracle_tmp=" + $oNegTmp + " self_tmp=" + $sNegTmp + ")")
        if (-not $diagOk) {
            Write-Output ("     oracle stderr: " + $oNegText)
            Write-Output ("     self stderr:   " + $sNegText)
        }
        $negFail = $negFail + 1
    }
} else {
    Write-Output ("SKIP link-failure diagnostics (" + $NegativeFile + " not in corpus)")
}

# (b) Emission failure: the oracle's own IR is rejected by LLVM, so both sides
# must fail before creating any object.
$efSrc = Join-Path $dirAbs $EmitFailFile
if (Test-Path -LiteralPath $efSrc) {
    $efArg = Get-FixtureArg $efSrc
    $efIr = Join-Path $runDir "ef.self.ll"
    $efCgIn = Join-Path $runDir "ef.cg.in"
    [System.IO.File]::WriteAllText($efCgIn, ($efArg + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    $efCg = Invoke-Separate $cgdrive @() $efCgIn $efIr `
                (Join-Path $runDir "ef.cg.err")

    $efExe = Join-Path $outDir "ef__emit_fail.exe"
    $efObj = Join-Path $outDir "ef__emit_fail__tmp.obj"
    Remove-Item -LiteralPath $efExe, $efObj -Force -ErrorAction SilentlyContinue

    $oEfCode = Invoke-Separate $ivy @("-o", $efExe, $efArg) "" `
                   (Join-Path $runDir "ef.oracle.out") `
                   (Join-Path $runDir "ef.oracle.err")
    $oEfObj = Test-Path -LiteralPath $efObj

    $efIn = Join-Path $runDir "ef.link.in"
    [System.IO.File]::WriteAllText($efIn,
        ($efIr + "`n" + $efExe + "`n" + $efArg + "`n" + $IvyrtLib + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    $sEfCode = Invoke-Separate $linkdriveExe @() $efIn `
                   (Join-Path $runDir "ef.self.out") `
                   (Join-Path $runDir "ef.self.err")
    $sEfObj = Test-Path -LiteralPath $efObj

    $efDiagOk = Test-BytesEqual ([System.IO.File]::ReadAllBytes((Join-Path $runDir "ef.oracle.err"))) `
                                ([System.IO.File]::ReadAllBytes((Join-Path $runDir "ef.self.err")))
    if ($efCg -eq 0 -and $oEfCode -eq 1 -and $sEfCode -eq 1 -and
        -not $oEfObj -and -not $sEfObj -and $efDiagOk) {
        Write-Output ("PASS emission-failure diagnostics (" + $EmitFailFile +
                      ", exit 1, no temp object)")
        $negPass = $negPass + 1
    } else {
        Write-Output ("FAIL emission-failure diagnostics (cgdrive=" + $efCg +
                      " oracle=" + $oEfCode + " self=" + $sEfCode +
                      " oracle_obj=" + $oEfObj + " self_obj=" + $sEfObj + ")")
        if (-not $efDiagOk) {
            Write-Output ("     oracle stderr: " + (Show-Bytes ([System.IO.File]::ReadAllBytes((Join-Path $runDir "ef.oracle.err")))))
            Write-Output ("     self stderr:   " + (Show-Bytes ([System.IO.File]::ReadAllBytes((Join-Path $runDir "ef.self.err")))))
        }
        $negFail = $negFail + 1
    }
} else {
    Write-Output ("SKIP emission-failure diagnostics (" + $EmitFailFile + " not in corpus)")
}

# (c) Paths with spaces: system() runs through cmd.exe, so the quoting fix in
# Linker::link_text must keep the command line intact. The exe lives in a
# directory whose name contains a space.
$spSrc = Join-Path $dirAbs $NegativeFile
if (Test-Path -LiteralPath $spSrc) {
    $spArg = Get-FixtureArg $spSrc
    $spIr = Join-Path $runDir "space.self.ll"
    $spCgIn = Join-Path $runDir "space.cg.in"
    [System.IO.File]::WriteAllText($spCgIn, ($spArg + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    $spCg = Invoke-Separate $cgdrive @() $spCgIn $spIr `
                (Join-Path $runDir "space.cg.err")

    $spaceDir = Join-Path $outDir "space dir"
    New-Item -ItemType Directory -Force -Path $spaceDir | Out-Null
    $spSelfExe = Join-Path $spaceDir "space_self.exe"
    $spOracleExe = Join-Path $spaceDir "space_oracle.exe"
    $spTmp = Join-Path $spaceDir "space_self__tmp.obj"
    Remove-Item -LiteralPath $spSelfExe, $spOracleExe, $spTmp -Force -ErrorAction SilentlyContinue

    $spIn = Join-Path $runDir "space.link.in"
    [System.IO.File]::WriteAllText($spIn,
        ($spIr + "`n" + $spSelfExe + "`n" + $spArg + "`n" + $IvyrtLib + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    $spCode = Invoke-Separate $linkdriveExe @() $spIn `
                  (Join-Path $runDir "space.self.out") `
                  (Join-Path $runDir "space.self.err")
    $spTmpGone = -not (Test-Path -LiteralPath $spTmp)

    $oSpCode = Invoke-Separate $ivy @("-o", $spOracleExe, $spArg) "" `
                   (Join-Path $runDir "space.oracle.out") `
                   (Join-Path $runDir "space.oracle.err")

    if ($spCg -eq 0 -and $spCode -eq 0 -and $oSpCode -eq 0 -and $spTmpGone -and
        (Test-Path -LiteralPath $spSelfExe) -and (Test-Path -LiteralPath $spOracleExe)) {
        $spSelfOut = Join-Path $runDir "space.self.run.out"
        $spSelfErr = Join-Path $runDir "space.self.run.err"
        $spOracleOut = Join-Path $runDir "space.oracle.run.out"
        $spOracleErr = Join-Path $runDir "space.oracle.run.err"
        $spSelfRun = Invoke-Separate $spSelfExe @() "" $spSelfOut $spSelfErr 15000
        $spOracleRun = Invoke-Separate $spOracleExe @() "" $spOracleOut $spOracleErr 15000
        $spOk = ($spSelfRun -eq $spOracleRun) -and
                (Test-BytesEqual ([System.IO.File]::ReadAllBytes($spOracleOut)) `
                                 ([System.IO.File]::ReadAllBytes($spSelfOut))) -and
                (Test-BytesEqual ([System.IO.File]::ReadAllBytes($spOracleErr)) `
                                 ([System.IO.File]::ReadAllBytes($spSelfErr)))
        if ($spOk) {
            Write-Output ("PASS space-path link (" + $NegativeFile +
                          ", exit " + $spSelfRun + ", temp object removed)")
            $edgePass = $edgePass + 1
        } else {
            Write-Output ("FAIL space-path run (self=" + $spSelfRun +
                          " oracle=" + $spOracleRun + ")")
            $edgeFail = $edgeFail + 1
        }
    } else {
        Write-Output ("FAIL space-path link (cgdrive=" + $spCg +
                      " linkdrive=" + $spCode + " oracle=" + $oSpCode +
                      " tmp_removed=" + $spTmpGone + ")")
        $p = Join-Path $runDir "space.self.err"
        if (Test-Path -LiteralPath $p) { Write-Output ([System.IO.File]::ReadAllText($p)) }
        $edgeFail = $edgeFail + 1
    }
} else {
    Write-Output ("SKIP space-path link (" + $NegativeFile + " not in corpus)")
}

# ---------------------------------------------------------------------------
# [6/6] Summary.
# ---------------------------------------------------------------------------
$total = $pass + $fail
Write-Output ("[6/6] summary: run PASS " + $pass + " / " + $total +
              " (skipped " + $skipped + "), negative PASS " +
              $negPass + " / " + ($negPass + $negFail) +
              ", space PASS " + $edgePass + " / " + ($edgePass + $edgeFail))
if ($fail -ne 0) {
    Write-Output ("      FAIL: " + ($failures -join ", "))
    exit 1
}
if ($negFail -ne 0) {
    exit 1
}
if ($edgeFail -ne 0) {
    exit 1
}
Write-Output "PASS: linked executables and failure diagnostics match the oracle"
exit 0
