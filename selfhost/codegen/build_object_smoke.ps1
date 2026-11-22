# selfhost/codegen/build_object_smoke.ps1 - H6.3 object-emission harness.
#
# End-to-end check of `codegen::ObjEmit` (mirror of CodeGen::emitObject in
# src/codegen/codegen.cpp:2729-2804):
#
#   source -> selfhost frontend/emitter (cgdrive) -> IR text
#          -> selfhost ObjEmit (objdrive)          -> COFF object
#          -> clang + std/rt.ivy (rt.obj)          -> native executable
#          -> run                                  -> stdout/exit status
#
# For every fixture in -Dir the harness also runs the oracle twice
# (`ivy --llvm` and `ivy -c`) and requires:
#   * selfhost IR text == oracle IR text (raw bytes), and
#   * selfhost object  == oracle object  (raw bytes).
# The object files are byte-identical because both sides parse the same IR
# text and configure the host TargetMachine with the same settings.
#
# Then, for the files listed in -SmokeFiles, the harness links the selfhost
# object with the H9.1 runtime (`rt.obj`, built from std/rt.ivy), runs it, and
# compares stdout/stderr/exit status
# against the oracle-built executable (`ivy -o`). This is the "link smoke".
#
# Every artefact (.obj/.ivm/.exe) is written under a fresh per-run directory
# in TEMP; nothing is written into the repository working tree.
#
# Usage (from the repository root):
#   powershell -NoProfile -File selfhost/codegen/build_object_smoke.ps1
#   powershell -NoProfile -File selfhost/codegen/build_object_smoke.ps1 -Dir selfhost/corpus/codegen
#   powershell -NoProfile -File selfhost/codegen/build_object_smoke.ps1 -TempRoot C:\scratch\h63
#
# Exit 0 iff every fixture object matches and every smoke executable matches.

param(
    [string]$Dir = "selfhost/corpus/codegen",
    [string]$TempRoot = "",
    [string]$LlvmRoot = "D:\Cpp\Clang",
    [string[]]$SmokeFiles = @(
        "01_return_const.ivy",
        "02_int_ops.ivy",
        "07_ops.ivy",
        "101_struct_array_float.ivy",
        "105_print_ref_char.ivy",
        "164_printf_basic.ivy",
        "235_inherit_virtual_dtor.ivy",
        "242_move_struct.ivy"
    )
)

$ErrorActionPreference = "Stop"

# `powershell -File ... -SmokeFiles a.ivy,b.ivy` arrives as one comma-joined
# string, so normalise it back to an array.
$SmokeFiles = @($SmokeFiles | ForEach-Object { $_ -split ',' } |
                Where-Object { $_ -ne "" })

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
# H9.9: the C++ oracle no longer ships in the tree; set IVY_ORACLE to a
# compiler built from tag `0.5-baseline` to run this parity harness.
$ivy = $env:IVY_ORACLE
if (-not $ivy) {
    Write-Output "BUILD FAILED (this parity harness needs the C++ oracle: set IVY_ORACLE)"
    exit 1
}
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
$runDir = Join-Path $tempBase ("h63object_" + $stamp + "_" + $PID)
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
# LLVM libraries + system libraries. `--libs x86` already pulls in core,
# target, irreader, codegen, asmprinter, ... (the X86 target is required by
# H6.3). System libs are rewritten to -l<name>; missing ones are skipped, except
# the ones that ship with the Windows SDK / MSVC: clang resolves those through
# the search paths it gives the linker, so they are available even when this
# script runs outside a developer prompt and `LIB` is unset.
# ---------------------------------------------------------------------------
$llvmLibs = @((& $llvmConfig --libs x86) -split '\s+' | Where-Object { $_ -ne "" })
if ($llvmLibs.Count -eq 0) { Fail "llvm-config --libs x86 returned nothing" }

$sdkLibs = @("advapi32", "bcrypt", "comdlg32", "crypt32", "dbghelp", "gdi32",
             "imm32", "iphlpapi", "kernel32", "mpr", "mfuuid", "netapi32",
             "ntdll", "ole32", "oleacc", "oleaut32", "powrprof", "psapi",
             "secur32", "setupapi", "shell32", "shlwapi", "user32", "uuid",
             "version", "wbemuuid", "winhttp", "winmm", "ws2_32", "wsock32")
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
        if (-not $installed -and ($sdkLibs -notcontains $base.ToLowerInvariant())) {
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
# [2/6] Build codegen.ivy (binding + ObjEmit) and objdrive.ivy.
# ---------------------------------------------------------------------------
Write-Output "[2/6] building object-emission driver ..."
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

$objdriveObj = Join-Path $runDir "objdrive.obj"
$oOut = Join-Path $runDir "build_objdrive.out"
$oErr = Join-Path $runDir "build_objdrive.err"
$oArgs = @((Join-Path $repo "selfhost\codegen\objdrive.ivy")) + $inc + @("-c", "-o", $objdriveObj)
$code = Invoke-Separate $ivy $oArgs "" $oOut $oErr
if ($code -ne 0 -or -not (Test-Path -LiteralPath $objdriveObj)) {
    Write-Output ("BUILD FAILED (objdrive, exit " + $code + ")")
    if (Test-Path -LiteralPath $oErr) { Write-Output ([System.IO.File]::ReadAllText($oErr)) }
    exit 1
}

# ---------------------------------------------------------------------------
# [3/6] Build the Ivy runtime (std/rt.ivy -> rt.obj) and link objdrive.exe.
# ---------------------------------------------------------------------------
Write-Output "[3/6] linking object-emission driver ..."
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

$objdriveExe = Join-Path $runDir "objdrive.exe"
$lOut = Join-Path $runDir "link_objdrive.out"
$lErr = Join-Path $runDir "link_objdrive.err"
$lArgs = @($objdriveObj, $codegenObj, $ivyrtObj) + $llvmLibs + $systemArgs +
         @("-o", $objdriveExe)
$code = Invoke-Separate $clang $lArgs "" $lOut $lErr
if ($code -ne 0 -or -not (Test-Path -LiteralPath $objdriveExe)) {
    Write-Output ("BUILD FAILED (objdrive link, exit " + $code + ")")
    if (Test-Path -LiteralPath $lErr) { Write-Output ([System.IO.File]::ReadAllText($lErr)) }
    exit 1
}
Write-Output ("      scratch: " + $runDir)

# ---------------------------------------------------------------------------
# [4/6] Collect the corpus, sorted by file name.
# ---------------------------------------------------------------------------
Write-Output "[4/6] collecting corpus ..."
$dirAbs = $Dir
if (-not [System.IO.Path]::IsPathRooted($dirAbs)) { $dirAbs = Join-Path $repo $Dir }
if (-not (Test-Path -LiteralPath $dirAbs)) {
    Write-Output ("no corpus directory: " + $dirAbs)
    exit 1
}
$files = @(Get-ChildItem -LiteralPath $dirAbs -Filter *.ivy -File | Sort-Object Name)
if ($files.Count -eq 0) {
    Write-Output ("no corpus files in " + $Dir)
    exit 1
}

$repoPrefix = $repo.TrimEnd('\') + '\'
function Get-FixtureArg([string]$full) {
    if ($full.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($repoPrefix.Length).Replace('\', '/')
    }
    return $full
}

# ---------------------------------------------------------------------------
# [5/6] Per fixture: IR text parity + object byte parity.
# ---------------------------------------------------------------------------
Write-Output ("[5/6] emitting " + $files.Count + " fixtures ...")
$pass = 0
$rejectedCount = 0
$fail = 0
$failures = New-Object System.Collections.Generic.List[string]

foreach ($f in $files) {
    $id = $f.Name
    $arg = Get-FixtureArg $f.FullName
    $safe = $f.BaseName -replace '[^A-Za-z0-9_.-]', '_'

    $oIr = Join-Path $runDir ($safe + ".oracle.ll")
    $oIrErr = Join-Path $runDir ($safe + ".oracle.ll.err")
    $sIr = Join-Path $runDir ($safe + ".self.ll")
    $sIrErr = Join-Path $runDir ($safe + ".self.ll.err")
    $oObj = Join-Path $runDir ($safe + ".oracle.obj")
    $oObjErr = Join-Path $runDir ($safe + ".oracle.obj.err")
    $sObj = Join-Path $runDir ($safe + ".self.obj")
    $sObjErr = Join-Path $runDir ($safe + ".self.obj.err")
    $drvIn = Join-Path $runDir ($safe + ".driver.in")

    $oCode = Invoke-Separate $ivy @("--llvm", $arg) "" $oIr $oIrErr
    # cgdrive needs the fixture path on stdin; write it once and reuse below.
    $cgIn = Join-Path $runDir ($safe + ".cg.in")
    [System.IO.File]::WriteAllText($cgIn, ($arg + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    $sCode = Invoke-Separate $cgdrive @() $cgIn $sIr $sIrErr

    $ocCode = Invoke-Separate $ivy @("-c", "-o", $oObj, $arg) "" `
                  (Join-Path $runDir ($safe + ".oc.out")) $oObjErr

    [System.IO.File]::WriteAllText($drvIn, ($sIr + "`n" + $sObj + "`n" + $arg + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    $scCode = Invoke-Separate $objdriveExe @() $drvIn `
                  (Join-Path $runDir ($safe + ".sc.out")) $sObjErr

    $irOk = $false
    if ((Test-Path -LiteralPath $oIr) -and (Test-Path -LiteralPath $sIr)) {
        $irOk = Test-BytesEqual ([System.IO.File]::ReadAllBytes($oIr)) `
                                ([System.IO.File]::ReadAllBytes($sIr))
    }

    # `ivy -c` parses the IR it just generated. A handful of bug-compat
    # fixtures emit IR that LLVM rejects (e.g. `sext ptr` from io::print on a
    # reference), so the oracle itself fails there. In that case the selfhost
    # driver must fail the same way with byte-identical stderr; on success the
    # object bytes must match.
    $rejected = ($ocCode -ne 0)
    $stageOk = $false
    $diagOk = $false
    if ($rejected) {
        $diagOk = Test-BytesEqual ([System.IO.File]::ReadAllBytes($oObjErr)) `
                                  ([System.IO.File]::ReadAllBytes($sObjErr))
        $stageOk = ($oCode -eq 0) -and ($sCode -eq 0) -and ($ocCode -eq $scCode) -and $diagOk
    } else {
        $objOk = $false
        if ((Test-Path -LiteralPath $oObj) -and (Test-Path -LiteralPath $sObj)) {
            $objOk = Test-BytesEqual ([System.IO.File]::ReadAllBytes($oObj)) `
                                     ([System.IO.File]::ReadAllBytes($sObj))
        }
        $stageOk = ($oCode -eq 0) -and ($sCode -eq 0) -and ($scCode -eq 0) -and $objOk
    }

    if ($irOk -and $stageOk) {
        if ($rejected) {
            Write-Output ("PASS " + $id + " (oracle rejects IR; diagnostics match)")
            $rejectedCount = $rejectedCount + 1
        } else {
            Write-Output ("PASS " + $id)
        }
        $pass = $pass + 1
    } else {
        Write-Output ("FAIL " + $id)
        if (($oCode -ne 0) -or ($sCode -ne 0) -or ($ocCode -ne $scCode) -or
            ((-not $rejected) -and ($scCode -ne 0))) {
            Write-Output ("     exit: llvm=" + $oCode + " cgdrive=" + $sCode +
                          " c=" + $ocCode + " objdrive=" + $scCode)
        }
        if (-not $irOk) {
            if ((Test-Path -LiteralPath $oIr) -and (Test-Path -LiteralPath $sIr)) {
                $a = [System.IO.File]::ReadAllBytes($oIr)
                $b = [System.IO.File]::ReadAllBytes($sIr)
                Write-Output ("     IR diff at byte " + (First-Diff $a $b))
                Write-Output ("     oracle IR: " + (Show-Bytes $a))
                Write-Output ("     self IR:   " + (Show-Bytes $b))
            } else {
                Write-Output "     IR file missing"
            }
        }
        if ($rejected) {
            if (-not $diagOk) {
                $a = [System.IO.File]::ReadAllBytes($oObjErr)
                $b = [System.IO.File]::ReadAllBytes($sObjErr)
                Write-Output ("     stderr diff at byte " + (First-Diff $a $b))
                Write-Output ("     oracle stderr: " + (Show-Bytes $a))
                Write-Output ("     self stderr:   " + (Show-Bytes $b))
            }
        } elseif (-not $stageOk) {
            if ((Test-Path -LiteralPath $oObj) -and (Test-Path -LiteralPath $sObj)) {
                $a = [System.IO.File]::ReadAllBytes($oObj)
                $b = [System.IO.File]::ReadAllBytes($sObj)
                Write-Output ("     object diff at byte " + (First-Diff $a $b) +
                              " (oracle " + $a.Length + " B, self " + $b.Length + " B)")
            } else {
                Write-Output "     object file missing"
            }
        }
        $fail = $fail + 1
        $failures.Add($id)
    }
}

# ---------------------------------------------------------------------------
# [6/6] Link smoke: selfhost object + rt.obj -> exe, run, compare with the
# oracle-built executable (`ivy -o`).
# ---------------------------------------------------------------------------
Write-Output "[6/6] link smoke ..."
$smokePass = 0
$smokeFail = 0
$smokeSkipped = 0

foreach ($name in $SmokeFiles) {
    $src = Join-Path $dirAbs $name
    if (-not (Test-Path -LiteralPath $src)) {
        Write-Output ("SKIP " + $name + " (not in corpus)")
        $smokeSkipped = $smokeSkipped + 1
        continue
    }
    $safe = [System.IO.Path]::GetFileNameWithoutExtension($name) -replace '[^A-Za-z0-9_.-]', '_'
    $selfObj = Join-Path $runDir ($safe + ".self.obj")
    if (-not (Test-Path -LiteralPath $selfObj)) {
        Write-Output ("SKIP " + $name + " (object not produced)")
        $smokeSkipped = $smokeSkipped + 1
        continue
    }

    $selfExe = Join-Path $runDir ($safe + ".self.exe")
    $sLinkOut = Join-Path $runDir ($safe + ".self.link.out")
    $sLinkErr = Join-Path $runDir ($safe + ".self.link.err")
    $sLinkArgs = @($selfObj, $ivyrtObj) + $systemArgs + @("-o", $selfExe)
    $linkCode = Invoke-Separate $clang $sLinkArgs "" $sLinkOut $sLinkErr

    $arg = Get-FixtureArg $src
    $oracleExe = Join-Path $runDir ($safe + ".oracle.exe")
    $oBuildOut = Join-Path $runDir ($safe + ".oracle.build.out")
    $oBuildErr = Join-Path $runDir ($safe + ".oracle.build.err")
    $oBuildCode = Invoke-Separate $ivy @("-o", $oracleExe, $arg) "" $oBuildOut $oBuildErr

    if ($linkCode -ne 0 -or $oBuildCode -ne 0) {
        Write-Output ("FAIL " + $name + " (link " + $linkCode + ", oracle build " + $oBuildCode + ")")
        if (Test-Path -LiteralPath $sLinkErr) {
            Write-Output ([System.IO.File]::ReadAllText($sLinkErr))
        }
        if (Test-Path -LiteralPath $oBuildErr) {
            Write-Output ([System.IO.File]::ReadAllText($oBuildErr))
        }
        $smokeFail = $smokeFail + 1
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
        Write-Output ("PASS " + $name + " run exit=" + $sRunCode +
                      " out=" + ([System.IO.File]::ReadAllBytes($sRunOut).Length) + "B")
        $smokePass = $smokePass + 1
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
        $smokeFail = $smokeFail + 1
    }
}

# ---------------------------------------------------------------------------
# Summary.
# ---------------------------------------------------------------------------
Write-Output ("      object PASS " + $pass + " / " + $files.Count +
              " (" + $rejectedCount + " oracle-rejected IR, diagnostics match)" +
              ", run PASS " + $smokePass + " / " + ($smokePass + $smokeFail) +
              " (skipped " + $smokeSkipped + ")")
if ($fail -ne 0) {
    Write-Output ("      FAIL: " + ($failures -join ", "))
    exit 1
}
if ($smokeFail -ne 0) {
    exit 1
}
Write-Output "PASS: IR text, object bytes, and linked executables match the oracle"
exit 0
