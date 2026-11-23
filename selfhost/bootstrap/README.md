# Stage 0 — the bootstrap seed

`ivy-stage0.exe` is a compiler built **by Ivy**, used by `selfhost/build.ps1`
to rebuild the compiler without the C++ oracle (`build\Release\ivy.exe`).

It is checked in so a fresh checkout can bootstrap with only LLVM + the MSVC
CRT/SDK installed.  The file is a fixed seed: it does not need to change when
the sources change, only when the compiler stops being able to compile the
current sources (i.e. when a new language feature is used before the seed
knows it) or when a fix must reach the first generation.

| | |
|---|---|
| size | 27 159 552 bytes |
| SHA-256 | `7FCFDE62EE757156C40D4A85A34CB928E82B1823B604989844B2B6380BE827FD` |
| built from | the H10.4 sources (parent commit `745deaf`, H10.3) |
| built by | `selfhost/build.ps1 -SelfLink` - re-bootstrapped from the previous seed until stage 1 and stage 2 agreed (H10.4 needs its own slot/GEP fixes in the first generation) |
| runtime | `rt.obj` from `std/rt.ivy` (H9.1), linked statically |
| linker | `clang++` + `llvm-config --libs x86` + `--system-libs`, `-Wl,/Brepro` |

`selfhost/build.ps1` verifies the hash before using the seed
(`-SkipHashCheck` bypasses it).  `ivy-stage0.sha256` is in `sha256sum` format.

## Regenerating the seed

Build stage 1, then copy it over the seed and refresh the hash:

```powershell
powershell -NoProfile -File selfhost\app\bootstrap_smoke.ps1 `
    -TempRoot "$env:TEMP\h95" -Stage1File "$env:TEMP\h95\stage1.txt" -SelfLink
Copy-Item (Get-Content "$env:TEMP\h95\stage1.txt") selfhost\bootstrap\ivy-stage0.exe
# update ivy-stage0.sha256 (see below) and the table above
```

Because the build is deterministic (`/Brepro`), rebuilding the same sources
with the same seed reproduces the same bytes; that is what `F2` in
`selfhost/build.ps1` asserts for generations 1 and 2.

## Version stamp and reproducibility (H9.7)

`selfhost/build.ps1` generates a `buildinfo` module before compiling the
stages, so the rebuilt compiler answers:

```powershell
> .\ivy.exe --version
ivy 0.5.0 (<short git hash>)
```

The checked-in `selfhost/app/buildinfo.ivy` is the fallback (`unknown`), used
by the development harnesses.  Pass `-GitHash <hash>` when building an
archive/export that has no `.git`.

Every link in the bootstrap passes `-Wl,/Brepro`, and the driver now passes it
for user programs too, so PE timestamps and PDB paths never leak into the
bytes.  LLVM records no absolute source path in these objects (the stages are
emitted without debug info), so two checkouts at different locations produce
identical files.  `selfhost/check_repro.ps1` bootstraps two such copies and
compares every `.obj`, `.ivm` and `.exe`; `.github/workflows/selfhost-repro.yml`
runs it weekly and on demand.

`build.ps1` prints the SHA-256 and size of the stage 1/2 and self-linked
executables, so a CI log or a release note can publish them next to the seed
digest above.  The digest covers the source + toolchain (the CRT/SDK libraries
are linked in), so quote it with the run ID or the LLVM/VS versions; within one
toolchain it is fully deterministic.

## Why an executable, not a source seed

An Ivy-only bootstrap needs *some* compiler to start from.  Committing the
seed makes the chain reproducible from the repository alone, with no network
download and no C++ toolchain, and lets CI (`selfhost-dogfood.yml`, H9.6) run
after `src/**` is deleted (H9.9).