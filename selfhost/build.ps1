# selfhost/build.ps1 - H9.5: bootstrap and F2 without the C++ oracle.
#
# Same chain as `selfhost/app/bootstrap_smoke.ps1`, but stage 0 is the
# self-hosted seed `selfhost/bootstrap/ivy-stage0.exe` instead of
# `build/Release/ivy.exe`.  Nothing here reads `src/**`, CMake or the oracle:
#
#   stage 0 (seed)  ->  stage 1  (F2: stage 1 and stage 2 agree)
#                   ->  stage 2
#
# F2 holds when every stage 2 object and `.ivm` is byte-identical to stage 1's,
# the runtime object stage 1 builds from `std/rt.ivy` is byte-identical to the
# one stage 0 built, and the stage 2 executable is byte-identical to stage 1's.
# The two stages are linked by the harness (clang + the LLVM libraries +
# `rt.obj`) with `-Wl,/Brepro`, exactly like `bootstrap_smoke.ps1`; `-SelfLink`
# additionally has stage 1 link stage 2 by itself (`--self-link`, H9.4).
#
# `-Golden` then runs `selfhost/check_golden.ps1 -Driver <stage 1>`, the
# oracle-free parity gate over the frozen baseline in `selfhost/golden/`.
#
# Nothing is written into the repository: every object, `.ivm`, executable and
# scratch file lives in a per-run directory under -TempRoot (or the platform
# temp dir), which is also the children's TEMP/TMP.
#
# Usage (from the repository root, with LLVM installed):
#   powershell -NoProfile -File selfhost/build.ps1 -Golden
#   powershell -NoProfile -File selfhost/build.ps1 -TempRoot C:\scratch\h95 -SelfLink -Golden
#   powershell -NoProfile -File selfhost/build.ps1 -Stage0 C:\elsewhere\ivy.exe
#
# -LlvmRoot selects the LLVM installation; without it the script uses
# $env:LLVM_ROOT, then D:\Cpp\Clang, then `llvm-config` on PATH.
# -Stage0File writes the path of the freshly built stage 1 compiler there.
# -SkipHashCheck accepts the seed without comparing it to its pinned SHA-256.
#
# Exit 0 iff the bootstrap chain (and the optional gates) pass.

param(
    [string]$TempRoot = "",
    [string]$LlvmRoot = "",
    [string]$Stage0 = "",
    [string]$Stage1File = "",
    [switch]$SelfLink = $false,
    [switch]$Golden = $false,
    [switch]$SkipHashCheck = $false
)

$ErrorActionPreference = "Stop"

function Fail([string]$msg) {
    Write-Output ("FAILED: " + $msg)
    exit 1
}

$repo = Split-Path $PSScriptRoot -Parent

# --- stage 0: the pinned Ivy-built seed ---------------------------------------
# The seed was produced by `bootstrap_smoke.ps1 -SelfLink` (see
# `bootstrap/README.md`); the hash makes a corrupted or swapped binary obvious
# before it gets a chance to emit objects.
$defaultSeed = Join-Path $PSScriptRoot "bootstrap\ivy-stage0.exe"
$seedFile = $defaultSeed
if ($Stage0 -ne "") { $seedFile = $Stage0 }
if (-not (Test-Path -LiteralPath $seedFile)) { Fail ("stage 0 not found: " + $seedFile) }
$seedFile = (Resolve-Path -LiteralPath $seedFile).Path
if ($SkipHashCheck) {
    Write-Output "      seed hash: not checked (-SkipHashCheck)"
} elseif ($Stage0 -ne "") {
    Write-Output "      seed hash: not checked (custom -Stage0)"
} else {
    $shaLine = (Get-Content -LiteralPath (Join-Path $PSScriptRoot "bootstrap\ivy-stage0.sha256") -Raw).Trim()
    $want = ($shaLine -split '\s+')[0]
    $have = (Get-FileHash -LiteralPath $seedFile -Algorithm SHA256).Hash
    if ($want -ne $have) {
        Fail ("stage 0 hash mismatch: expected " + $want + ", got " + $have)
    }
    Write-Output ("      seed hash: " + $have)
}

# Locate the LLVM installation: -LlvmRoot first, then $env:LLVM_ROOT, then the
# historical D:\Cpp\Clang, then whatever `llvm-config` is on PATH.
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
# Export it: the driver's `find_linker()` (`where clang++`) and the `--self-link`
# library collection both run in child processes, and CI relies on this being
# the authoritative installation.  `IVY_LINKER` (when set) pins the driver to
# the same clang++ the harness links with.
$env:LLVM_ROOT = $LlvmRoot
if (-not $env:IVY_LINKER) { $env:IVY_LINKER = $clang }

$clang = Join-Path $LlvmRoot "bin\clang.exe"
$llvmConfig = Join-Path $LlvmRoot "bin\llvm-config.exe"
$llvmLib = Join-Path $LlvmRoot "lib"
if (-not (Test-Path -LiteralPath $clang)) { Fail ("clang not found: " + $clang) }
if (-not (Test-Path -LiteralPath $llvmConfig)) { Fail ("llvm-config not found: " + $llvmConfig) }

$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$runDir = Join-Path $tempBase ("h95_bootstrap_" + $stamp + "_" + $PID)
$stage1 = Join-Path $runDir "stage1"
$stage2 = Join-Path $runDir "stage2"
New-Item -ItemType Directory -Force -Path $stage1, $stage2 | Out-Null
Write-Output ("run directory: " + $runDir)

# Every compiler process inherits this isolated TEMP/TMP (the driver writes its
# captured IR to a scratch file there).
$env:TEMP = $runDir
$env:TMP = $runDir

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

# H9.1: the Ivy runtime (std/rt.ivy -> rt.obj) is built by stage 0 and linked
# into both stages; the stage 1 driver also uses it for `-o` through
# IVYRT_LIB_PATH, set right below.
$ivyrt = Join-Path $runDir "rt.obj"
$rtOut = Join-Path $runDir "build_rt.out"
$rtErr = Join-Path $runDir "build_rt.err"
$rtArgs = @("-c", (Join-Path $repo "std\rt.ivy"),
            "-I", (Join-Path $repo "std"), "-o", $ivyrt)
$rtCode = Invoke-Separate $seedFile $rtArgs $rtOut $rtErr
if ($rtCode -ne 0 -or -not (Test-Path -LiteralPath $ivyrt)) {
    Fail ("runtime build failed (exit " + $rtCode + "); see " + $rtErr)
}
# Forward slashes: this is the form the oracle baked in, so the linker command
# line and the frozen `.ivm` hashes match verbatim.
$env:IVYRT_LIB_PATH = ($ivyrt -replace '\\', '/')

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
    @("selfhost\mir", "interpreter"),
    @("selfhost\codegen", "emitter"),
    @("selfhost\codegen", "codegen"),
    @("selfhost\app", "pipeline")
)
$mainSrc = Join-Path $repo "selfhost\app\main.ivy"

# H9.4: the order in which the module objects are handed to the linker.  It
# mirrors the `import` statements of `main.ivy` (which is also the order
# `--self-link` walks `cg.import_count()`), because the MSVC/lld layout of the
# PE depends on the input order.  `$modules` stays the *build* order: `io` is
# built first so every other module can read `io.ivm`.
$linkOrder = @("diagnostic", "token", "lexer", "preprocessor", "ast", "parser",
               "module_io", "imports", "cli", "hir", "hir_builder", "mir",
               "mir_builder", "interpreter", "pipeline", "emitter", "codegen",
               "io")

# Include path: the run directory first (the `.ivm` files written there), then
# the source directories.
function Include-Args([string]$outDir) {
    $a = @("-I", $outDir)
    foreach ($md in $moduleDirs) { $a += @("-I", (Join-Path $repo $md)) }
    return $a
}

# --- LLVM libraries (same shape as bootstrap_smoke.ps1) -----------------------
$llvmLibs = @((& $llvmConfig --libs x86) -split '\s+' | Where-Object { $_ -ne "" })
if ($llvmLibs.Count -eq 0) { Fail "llvm-config --libs x86 returned nothing" }

$systemArgs = @()
# `llvm-config --system-libs` lists two kinds of entries on Windows:
#   * libraries that ship with the Windows SDK / MSVC (`psapi.lib`, `ntdll.lib`,
#     ...).  clang resolves those through the search paths it sets up for the
#     linker, so they work even outside a developer prompt and `LIB` is unset;
#   * libraries the LLVM build was configured against but the Windows package
#     does not ship (LLVM 21.1.6: `libxml2s.lib`).
# Keep the former, drop the latter.
$sdkLibs = @("advapi32", "bcrypt", "comdlg32", "crypt32", "dbghelp", "gdi32",
             "imm32", "iphlpapi", "kernel32", "mpr", "mfuuid", "netapi32",
             "ntdll", "ole32", "oleacc", "oleaut32", "powrprof", "psapi",
             "secur32", "setupapi", "shell32", "shlwapi", "user32", "uuid",
             "version", "wbemuuid", "winhttp", "winmm", "ws2_32", "wsock32")
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
        if (-not $installed -and ($sdkLibs -notcontains $base.ToLowerInvariant())) { continue }
        $systemArgs += ("-l" + $base)
    } else {
        $systemArgs += $n
    }
}

# Link `$outDir`'s objects into `$outDir\ivy_selfhost.exe`.
function Link-Stage([string]$outDir) {
    $objs = @((Join-Path $outDir "main.obj"))
    foreach ($name in $linkOrder) { $objs += (Join-Path $outDir ($name + ".obj")) }
    $exe = Join-Path $outDir "ivy_selfhost.exe"
    $largs = $objs + @($ivyrt) + $llvmLibs + $systemArgs + @("-Wl,/Brepro", "-o", $exe)
    $code = Invoke-Separate $clang $largs (Join-Path $outDir "link.out") (Join-Path $outDir "link.err")
    if ($code -ne 0) {
        # The linker reports unresolved symbols on stdout, so show both sides
        # (and the link line) before failing.
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

$total = 3
if ($SelfLink) { $total++ }
if ($Golden) { $total++ }
$step = 0

# --- 1. stage 0 (seed) -> stage 1 ---------------------------------------------
$step++
Write-Output ("[" + $step + "/" + $total + "] stage 0 (seed) builds stage 1 ...")
Write-Output ("      seed: " + $seedFile)
Build-Stage $seedFile $stage1
$stage1Exe = Link-Stage $stage1
Write-Output ("      " + $stage1Exe)
# H8.2/H9.6: a caller (dogfood, CI, the golden gate) can reuse this compiler
# instead of building a second one.
if ($Stage1File -ne "") {
    Set-Content -LiteralPath $Stage1File -Value $stage1Exe -Encoding ascii
}

# --- 2. stage 1 -> stage 2 ----------------------------------------------------
$step++
Write-Output ("[" + $step + "/" + $total + "] stage 1 builds stage 2 ...")
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

# --- 2b. the runtime object, rebuilt by the Ivy compiler ----------------------
$rtSelf = Join-Path $stage2 "rt.obj"
$rtCode = Invoke-Separate $stage1Exe `
              (@((Join-Path $repo "std\rt.ivy"), "-I", (Join-Path $repo "std"),
                 "-c", "-o", $rtSelf)) `
              (Join-Path $stage2 "b_rt.out") (Join-Path $stage2 "b_rt.err")
if ($rtCode -ne 0 -or -not (Test-Path -LiteralPath $rtSelf)) {
    Write-Output ("BUILD FAILED (rt, exit " + $rtCode + ")")
    Get-Content (Join-Path $stage2 "b_rt.err") | Select-Object -First 10
    exit 1
}
$same = Test-BytesEqual $rtSelf $ivyrt
Write-Output ("      RT  " + "rt.obj".PadRight(14) + $(if ($same) { "identical" } else { "DIFFERENT" }))
if (-not $same) { Fail "stage 1 rt.obj differs from the stage 0 rt.obj" }

# --- 3. stage 2 executable ----------------------------------------------------
$step++
Write-Output ("[" + $step + "/" + $total + "] linking stage 2 ...")
$stage2Exe = Link-Stage $stage2
$same = Test-BytesEqual $stage1Exe $stage2Exe
Write-Output ("      EXE " + "stage1/stage2".PadRight(14) + $(if ($same) { "identical" } else { "DIFFERENT" }))
if (-not $same) { Fail "stage 1 and stage 2 executables differ" }

# --- 4. self-link: stage 1 links stage 2 by itself (H9.4) --------------------
if ($SelfLink) {
    $step++
    Write-Output ("[" + $step + "/" + $total + "] stage 1 links stage 2 by itself ...")
    $selfExe = Join-Path $stage2 "ivy_selflink.exe"
    $inc = Include-Args $stage2
    $code = Invoke-Separate $stage1Exe (@($mainSrc) + $inc + @("--self-link", "-o", $selfExe)) `
             (Join-Path $stage2 "selflink.out") (Join-Path $stage2 "selflink.err")
    if ($code -ne 0) {
        Write-Output ("SELF-LINK FAILED (exit " + $code + ")")
        Get-Content (Join-Path $stage2 "selflink.err") | Select-Object -First 20
        exit 1
    }
    $same = Test-BytesEqual $selfExe $stage2Exe
    Write-Output ("      EXE " + "self-link".PadRight(14) + $(if ($same) { "identical" } else { "DIFFERENT" }))
    if (-not $same) { Fail "self-linked executable differs from the harness link" }
}

# --- 5. golden parity (oracle-free) ------------------------------------------
if ($Golden) {
    $step++
    Write-Output ("[" + $step + "/" + $total + "] golden parity of stage 1 (no oracle) ...")
    $check = Join-Path $PSScriptRoot "check_golden.ps1"
    if (-not (Test-Path -LiteralPath $check)) { Fail ("missing " + $check) }
    & powershell -NoProfile -File $check -Driver $stage1Exe `
                 -Runtime $ivyrt -TempRoot (Join-Path $runDir "golden")
    if ($LASTEXITCODE -ne 0) { Fail ("golden check failed (exit " + $LASTEXITCODE + ")") }
}

Write-Output "--- BOOTSTRAP PASS ---"
exit 0