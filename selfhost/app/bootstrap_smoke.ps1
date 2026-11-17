# selfhost/app/bootstrap_smoke.ps1 - H8.1: `ivy` compiles itself (F2).
#
# Stage 0 is the C++-built oracle (`build/Release/ivy.exe`).  It compiles every
# selfhost module plus `selfhost/app/main.ivy` into a fresh TEMP directory and
# the resulting objects are linked (clang + LLVM libraries + libivyrt) into the
# **stage 1** compiler.  Stage 1 then compiles the very same sources into a
# second directory (**stage 2**).  F2 holds when
#
#   * every stage 2 object is byte-identical to the stage 1 object,
#   * every stage 2 `.ivm` is byte-identical to the stage 1 `.ivm`, and
#   * the stage 2 executable is byte-identical to the stage 1 executable.
#
# The selfhost compiler cannot link an executable by itself (that needs the LLVM
# libraries), so the harness links both stages the same way
# `selfhost/codegen/build_object_smoke.ps1` does.  The link is made
# deterministic with `-Wl,/Brepro`; without it two links of the same objects
# differ in the two PE `TimeDateStamp` fields.
#
# Nothing is written into the repository: every object, `.ivm` and executable
# lives in a per-run directory under -TempRoot (or the platform temp dir), which
# is also the children's TEMP/TMP.
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/app/bootstrap_smoke.ps1
#   powershell -NoProfile -File selfhost/app/bootstrap_smoke.ps1 -TempRoot C:\scratch\h81
#   powershell -NoProfile -File selfhost/app/bootstrap_smoke.ps1 -LlvmRoot "C:\Program Files\LLVM"
#
# -LlvmRoot selects the LLVM installation; without it the script uses
# $env:LLVM_ROOT, then D:\Cpp\Clang, then `llvm-config` on PATH.
# -Stage1File writes the path of the freshly linked stage 1 compiler there (the
# dogfood gate reuses it instead of building a second one).
#
# Exit 0 iff F2 holds.

param(
    [string]$TempRoot = "",
    [string]$LlvmRoot = "",
    [string]$Stage1File = ""
)

$ErrorActionPreference = "Stop"

function Fail([string]$msg) {
    Write-Output ("FAILED: " + $msg)
    exit 1
}

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$ivy = Join-Path $repo "build\Release\ivy.exe"
if (-not (Test-Path -LiteralPath $ivy)) { Fail ("oracle not found: " + $ivy) }
$ivyrt = Join-Path $repo "build\Release\ivyrt.lib"
if (-not (Test-Path -LiteralPath $ivyrt)) { Fail ("runtime library not found: " + $ivyrt) }

# Locate the LLVM installation: -LlvmRoot first, then $env:LLVM_ROOT, then the
# historical D:\Cpp\Clang, then whatever `llvm-config` is on PATH.  The gate in
# CI passes -LlvmRoot explicitly; a developer machine usually just has one of
# the other three.
function Test-LlvmRoot([string]$root) {
    if ($root -eq "") { return $false }
    return (Test-Path -LiteralPath (Join-Path $root "bin\clang.exe")) -and
           (Test-Path -LiteralPath (Join-Path $root "bin\llvm-config.exe"))
}
if (-not (Test-LlvmRoot $LlvmRoot)) {
    $found = ""
    if (Test-LlvmRoot $env:LLVM_ROOT) {
        $found = $env:LLVM_ROOT
    } elseif (Test-LlvmRoot "D:\Cpp\Clang") {
        $found = "D:\Cpp\Clang"
    } else {
        $cmd = Get-Command "llvm-config" -ErrorAction SilentlyContinue
        if ($cmd) {
            $prefix = (& $cmd.Source "--prefix")
            if ($prefix) {
                $prefix = ([string]$prefix).Trim()
                if (Test-LlvmRoot $prefix) { $found = $prefix }
            }
        }
    }
    if ($found -eq "") {
        Fail ("LLVM not found: pass -LlvmRoot, set LLVM_ROOT, or put llvm-config on PATH")
    }
    $LlvmRoot = $found
}

$clang = Join-Path $LlvmRoot "bin\clang.exe"
$llvmConfig = Join-Path $LlvmRoot "bin\llvm-config.exe"
$llvmLib = Join-Path $LlvmRoot "lib"
if (-not (Test-Path -LiteralPath $clang)) { Fail ("clang not found: " + $clang) }
if (-not (Test-Path -LiteralPath $llvmConfig)) { Fail ("llvm-config not found: " + $llvmConfig) }

$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$runDir = Join-Path $tempBase ("h81_bootstrap_" + $stamp + "_" + $PID)
$stage1 = Join-Path $runDir "stage1"
$stage2 = Join-Path $runDir "stage2"
New-Item -ItemType Directory -Force -Path $stage1, $stage2 | Out-Null

# Every compiler process inherits this isolated TEMP/TMP (the driver writes its
# captured IR to a scratch file there).
$env:TEMP = $runDir
$env:TMP = $runDir
# Forward slashes: this is the form the oracle bakes in at build time, so the
# linker command line matches the oracle's verbatim.
$env:IVYRT_LIB_PATH = ($ivyrt -replace '\\', '/')

function Quote-Arg([string]$s) {
    if ($s -match '[\s"]') { return '"' + ($s -replace '"', '\"') + '"' }
    return $s
}

# Run one process with separate stdout/stderr files and return its exit status.
function Invoke-Separate([string]$exe, [string[]]$argList, [string]$outPath,
                         [string]$errPath) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = $repo
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $false
    $psi.CreateNoWindow = $true
    $psi.Arguments = (@($argList | ForEach-Object { Quote-Arg $_ }) -join " ")

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    if (-not $p.Start()) { throw ("cannot start " + $exe) }

    $fsOut = [System.IO.File]::Create($outPath)
    $fsErr = [System.IO.File]::Create($errPath)
    $tOut = $p.StandardOutput.BaseStream.CopyToAsync($fsOut)
    $tErr = $p.StandardError.BaseStream.CopyToAsync($fsErr)
    $p.WaitForExit()
    $tOut.Wait()
    $tErr.Wait()
    $fsOut.Dispose()
    $fsErr.Dispose()
    $code = $p.ExitCode
    $p.Dispose()
    return $code
}

function Test-BytesEqual([string]$a, [string]$b) {
    if (-not (Test-Path -LiteralPath $a) -or -not (Test-Path -LiteralPath $b)) { return $false }
    $ba = [System.IO.File]::ReadAllBytes($a)
    $bb = [System.IO.File]::ReadAllBytes($b)
    if ($ba.Length -ne $bb.Length) { return $false }
    for ($i = 0; $i -lt $ba.Length; $i++) {
        if ($ba[$i] -ne $bb[$i]) { return $false }
    }
    return $true
}

# --- the compiler's own sources ------------------------------------------------
$moduleDirs = @("selfhost\common", "selfhost\parsing", "selfhost\hir", "selfhost\mir",
                "selfhost\codegen", "selfhost\app", "std")
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
    @("selfhost\codegen", "codegen"),
    @("selfhost\app", "pipeline")
)
$mainSrc = Join-Path $repo "selfhost\app\main.ivy"

# Include path: the run directory first (the `.ivm` files written there), then
# the source directories.
function Include-Args([string]$outDir) {
    $a = @("-I", $outDir)
    foreach ($md in $moduleDirs) { $a += @("-I", (Join-Path $repo $md)) }
    return $a
}

# --- LLVM libraries (same shape as build_object_smoke.ps1) ---------------------
$llvmLibs = @((& $llvmConfig --libs x86) -split '\s+' | Where-Object { $_ -ne "" })
if ($llvmLibs.Count -eq 0) { Fail "llvm-config --libs x86 returned nothing" }

$systemArgs = @()
foreach ($n in @((& $llvmConfig --system-libs) -split '\s+' | Where-Object { $_ -ne "" })) {
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
        if (-not $installed) { continue }
        $systemArgs += ("-l" + $base)
    } else {
        $systemArgs += $n
    }
}

# Link `$outDir`'s objects into `$outDir\ivy_selfhost.exe`.
function Link-Stage([string]$outDir) {
    $objs = @((Join-Path $outDir "main.obj"))
    foreach ($m in $modules) { $objs += (Join-Path $outDir ($m[1] + ".obj")) }
    $exe = Join-Path $outDir "ivy_selfhost.exe"
    $largs = $objs + @($ivyrt) + $llvmLibs + $systemArgs + @("-Wl,/Brepro", "-o", $exe)
    $code = Invoke-Separate $clang $largs (Join-Path $outDir "link.out") (Join-Path $outDir "link.err")
    if ($code -ne 0) {
        # The MSVC linker reports the individual unresolved symbols on stdout and
        # the driver's summary on stderr, so show both (and the link line) before
        # failing: without them a CI failure is just "exit code 1120".
        Write-Output "LINK FAILED"
        Write-Output ("linker: " + $clang)
        Write-Output ("libraries: " + ($llvmLibs.Count + $systemArgs.Count))
        Write-Output "--- stderr ---"
        Get-Content (Join-Path $outDir "link.err") | Select-Object -First 40
        Write-Output "--- stdout ---"
        Get-Content (Join-Path $outDir "link.out") | Select-Object -First 40
        Write-Output "--- command line ---"
        Write-Output (@($largs | ForEach-Object { Quote-Arg $_ }) -join " ")
        exit 1
    }
    return $exe
}

# Compile every module + main.ivy with `$compiler` into `$outDir`.
function Build-Stage([string]$compiler, [string]$outDir) {
    $inc = Include-Args $outDir
    foreach ($m in $modules) {
        $name = $m[1]
        $src = Join-Path $repo (Join-Path $m[0] ($name + ".ivy"))
        $obj = Join-Path $outDir ($name + ".obj")
        $code = Invoke-Separate $compiler (@($src) + $inc + @("-c", "-o", $obj)) `
                                 (Join-Path $outDir ("b_" + $name + ".out")) `
                                 (Join-Path $outDir ("b_" + $name + ".err"))
        if ($code -ne 0) {
            Write-Output ("BUILD FAILED (" + $name + ", exit " + $code + ")")
            Get-Content (Join-Path $outDir ("b_" + $name + ".err")) | Select-Object -First 10
            exit 1
        }
    }
    $obj = Join-Path $outDir "main.obj"
    $code = Invoke-Separate $compiler (@($mainSrc) + $inc + @("-c", "-o", $obj)) `
                             (Join-Path $outDir "b_main.out") (Join-Path $outDir "b_main.err")
    if ($code -ne 0) {
        Write-Output ("BUILD FAILED (main, exit " + $code + ")")
        Get-Content (Join-Path $outDir "b_main.err") | Select-Object -First 10
        exit 1
    }
}

# --- 1. stage 0 (oracle) -> stage 1 -------------------------------------------
Write-Output "[1/3] stage 0 (oracle) builds stage 1 ..."
Build-Stage $ivy $stage1
$stage1Exe = Link-Stage $stage1
Write-Output ("      " + $stage1Exe)
# H8.2: the dogfood gate reuses this freshly built compiler instead of building
# it a second time.
if ($Stage1File -ne "") {
    Set-Content -LiteralPath $Stage1File -Value $stage1Exe -Encoding ascii
}

# --- 2. stage 1 -> stage 2 ----------------------------------------------------
Write-Output "[2/3] stage 1 builds stage 2 ..."
Build-Stage $stage1Exe $stage2

$fail = 0
$names = @()
foreach ($m in $modules) { $names += $m[1] }
$names += "main"
foreach ($name in $names) {
    $a = Join-Path $stage1 ($name + ".obj")
    $b = Join-Path $stage2 ($name + ".obj")
    $same = Test-BytesEqual $a $b
    Write-Output ("      OBJ " + $name.PadRight(14) + $(if ($same) { "identical" } else { "DIFFERENT" }))
    if (-not $same) { $fail++ }
    $ai = Join-Path $stage1 ($name + ".ivm")
    $bi = Join-Path $stage2 ($name + ".ivm")
    if ((Test-Path -LiteralPath $ai) -and (Test-Path -LiteralPath $bi)) {
        $same = Test-BytesEqual $ai $bi
        Write-Output ("      IVM " + $name.PadRight(14) + $(if ($same) { "identical" } else { "DIFFERENT" }))
        if (-not $same) { $fail++ }
    }
}
if ($fail -ne 0) { Fail "stage 1 and stage 2 disagree" }

# --- 3. stage 2 executable ----------------------------------------------------
Write-Output "[3/3] linking stage 2 ..."
$stage2Exe = Link-Stage $stage2
$same = Test-BytesEqual $stage1Exe $stage2Exe
Write-Output ("      EXE " + "stage1/stage2".PadRight(14) + $(if ($same) { "identical" } else { "DIFFERENT" }))
if (-not $same) { Fail "stage 1 and stage 2 executables differ" }

Write-Output "--- F2 PASS ---"
exit 0
