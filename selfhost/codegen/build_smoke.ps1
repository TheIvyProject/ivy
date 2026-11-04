# selfhost/codegen/build_smoke.ps1 - H6.1 build smoke for the LLVM C API binding.
#
# Compiles the binding module (codegen.ivy) and the smoke driver (smoke.ivy)
# with the frozen ivy.exe (`-c`, object only), links both objects manually with
# clang + the LLVM libraries (the Ivy CLI has no -l/-L flags), then runs the
# executable and checks its exact stdout marker and exit status.
#
# Every artefact (.obj/.ivm/.exe) is written under a fresh per-run directory in
# TEMP; nothing is written into the repository working tree.
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/codegen/build_smoke.ps1
#   powershell -NoProfile -File selfhost/codegen/build_smoke.ps1 -TempRoot C:\scratch\h61
#
# -TempRoot is the PARENT for the per-run scratch directory; platform temp is
# used when empty. -LlvmRoot defaults to D:\Cpp\Clang.
#
# Exit 0 iff the build, link, and smoke run all pass.

param(
    [string]$TempRoot = "",
    [string]$LlvmRoot = "D:\Cpp\Clang"
)

$ErrorActionPreference = "Stop"

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$ivy = Join-Path $repo "build\Release\ivy.exe"
$clang = Join-Path $LlvmRoot "bin\clang.exe"
$llvmConfig = Join-Path $LlvmRoot "bin\llvm-config.exe"
$llvmLib = Join-Path $LlvmRoot "lib"

function Fail([string]$message) {
    Write-Output ("FAIL: " + $message)
    exit 1
}

if (-not (Test-Path -LiteralPath $ivy)) { Fail ("oracle not found: " + $ivy) }
if (-not (Test-Path -LiteralPath $clang)) { Fail ("clang not found: " + $clang) }
if (-not (Test-Path -LiteralPath $llvmConfig)) { Fail ("llvm-config not found: " + $llvmConfig) }

$tempBase = $TempRoot
if ($tempBase -eq "") { $tempBase = [System.IO.Path]::GetTempPath() }
New-Item -ItemType Directory -Force -Path $tempBase | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$runDir = Join-Path $tempBase ("h61smoke_" + $stamp + "_" + $PID)
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

# Every compiler/linker/run process inherits this isolated TEMP/TMP.
$env:TEMP = $runDir
$env:TMP = $runDir

function Quote-Arg([string]$s) {
    if ($s -match '[\s"]') {
        return '"' + ($s -replace '"', '\"') + '"'
    }
    return $s
}

# Run a tool with byte-exact, separate stdout/stderr files. Returns its exit
# status; the caller inspects the files.
function Invoke-Separate([string]$exe, [string[]]$argList,
                         [string]$outPath, [string]$errPath) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = $repo
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $false
    $psi.CreateNoWindow = $true
    $quoted = @($argList | ForEach-Object { Quote-Arg $_ })
    $psi.Arguments = ($quoted -join " ")

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    if (-not $p.Start()) { throw "cannot start $exe" }

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

function Show-Tool-Error([string]$label, [string]$outPath, [string]$errPath) {
    Write-Output ("      " + $label + " stdout:")
    if (Test-Path -LiteralPath $outPath) { Write-Output ([System.IO.File]::ReadAllText($outPath)) }
    Write-Output ("      " + $label + " stderr:")
    if (Test-Path -LiteralPath $errPath) { Write-Output ([System.IO.File]::ReadAllText($errPath)) }
}

# ---------------------------------------------------------------------------
# [1/4] Compile the binding module (also produces codegen.ivm in the run dir).
# ---------------------------------------------------------------------------
Write-Output "[1/4] compiling binding module ..."
$inc = @("-I", $runDir, "-I", (Join-Path $repo "selfhost\codegen"))
$codegenObj = Join-Path $runDir "codegen.obj"
$cOut = Join-Path $runDir "codegen.build.out"
$cErr = Join-Path $runDir "codegen.build.err"
$cArgs = @((Join-Path $repo "selfhost\codegen\codegen.ivy")) + $inc + @("-c", "-o", $codegenObj)
$code = Invoke-Separate $ivy $cArgs $cOut $cErr
if ($code -ne 0 -or -not (Test-Path -LiteralPath $codegenObj)) {
    Show-Tool-Error "codegen" $cOut $cErr
    Fail ("binding module compile failed (exit " + $code + ")")
}
$ivm = Join-Path $runDir "codegen.ivm"
if (-not (Test-Path -LiteralPath $ivm)) { Fail ("module interface missing: " + $ivm) }

# ---------------------------------------------------------------------------
# [2/4] Compile the smoke driver against the binding interface.
# ---------------------------------------------------------------------------
Write-Output "[2/4] compiling smoke driver ..."
$smokeObj = Join-Path $runDir "smoke.obj"
$sOut = Join-Path $runDir "smoke.build.out"
$sErr = Join-Path $runDir "smoke.build.err"
$sArgs = @((Join-Path $repo "selfhost\codegen\smoke.ivy")) + $inc + @("-c", "-o", $smokeObj)
$code = Invoke-Separate $ivy $sArgs $sOut $sErr
if ($code -ne 0 -or -not (Test-Path -LiteralPath $smokeObj)) {
    Show-Tool-Error "smoke" $sOut $sErr
    Fail ("smoke driver compile failed (exit " + $code + ")")
}

# ---------------------------------------------------------------------------
# [3/4] Link with clang + LLVM static libs + system libs.
#
# `llvm-config --libs` already prints absolute paths for the LLVM archives.
# `--system-libs` prints bare `*.lib` names, which the clang GNU driver treats
# as file inputs; they are rewritten to `-l<name>` so the MSVC linker searches
# LIB. Libraries listed by llvm-config but absent from the installation (e.g.
# libxml2s.lib in this toolchain) are skipped and reported.
# ---------------------------------------------------------------------------
Write-Output "[3/4] linking smoke executable ..."
$llvmLibs = @((& $llvmConfig --libs core target irreader) -split '\s+' | Where-Object { $_ -ne "" })
if ($llvmLibs.Count -eq 0) { Fail "llvm-config --libs returned nothing" }

$systemArgs = @()
$systemNames = @((& $llvmConfig --system-libs) -split '\s+' | Where-Object { $_ -ne "" })
foreach ($n in $systemNames) {
    if ($n -match '\.lib$') {
        $base = $n.Substring(0, $n.Length - 4)
        $installed = Test-Path -LiteralPath (Join-Path $llvmLib $n)
        if (-not $installed -and $env:LIB) {
            foreach ($dir in ($env:LIB -split ';')) {
                if ($dir -and (Test-Path -LiteralPath (Join-Path $dir $n))) {
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

$exe = Join-Path $runDir "h61_smoke.exe"
$lOut = Join-Path $runDir "link.out"
$lErr = Join-Path $runDir "link.err"
$lArgs = @($smokeObj, $codegenObj) + $llvmLibs + $systemArgs + @("-o", $exe)
$code = Invoke-Separate $clang $lArgs $lOut $lErr
if ($code -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
    Show-Tool-Error "link" $lOut $lErr
    Fail ("link failed (exit " + $code + ")")
}

# ---------------------------------------------------------------------------
# [4/4] Run and assert exact stdout marker + exit status 0.
# ---------------------------------------------------------------------------
Write-Output "[4/4] running smoke executable ..."
$rOut = Join-Path $runDir "run.out"
$rErr = Join-Path $runDir "run.err"
$code = Invoke-Separate $exe @() $rOut $rErr

$outBytes = [System.IO.File]::ReadAllBytes($rOut)
$errBytes = [System.IO.File]::ReadAllBytes($rErr)
$expected = "h61 smoke ok"
$stdout = [System.Text.Encoding]::ASCII.GetString($outBytes)
$stderr = [System.Text.Encoding]::ASCII.GetString($errBytes)

# Accept the platform line ending only: strip one trailing CRLF or LF.
$trimmed = $stdout
if ($trimmed.EndsWith("`r`n")) {
    $trimmed = $trimmed.Substring(0, $trimmed.Length - 2)
} elseif ($trimmed.EndsWith("`n")) {
    $trimmed = $trimmed.Substring(0, $trimmed.Length - 1)
}

Write-Output ("      scratch: " + $runDir)
Write-Output ("      stdout:  " + $trimmed)
Write-Output ("      stderr:  " + ($stderr -replace "`r?`n", " | "))
Write-Output ("      exit:    " + $code)

if ($code -ne 0) { Fail ("smoke exit " + $code + " (expected 0)") }
if ($trimmed -cne $expected) {
    Write-Output ("      stdout bytes: " + [BitConverter]::ToString($outBytes))
    Fail ("stdout marker mismatch (expected '" + $expected + "')")
}
if ($errBytes.Length -ne 0) {
    Fail "stderr is not empty"
}

Write-Output "PASS: h61 smoke ok (LLVM C API binding build + run)"
exit 0
