# Rebuilds only the test target, then runs it.
#
# Why a separate entry point: a top-level "ninja: no work to do" says nothing about whether the
# test binary was rebuilt. This session's extension DLL was already current while the test binary
# predated the shader change, so the test build has to be its own explicit step, otherwise a stale
# result gets compared against a fresh baseline and reads as a regression (or hides one).
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build\windows-editor"

& (Join-Path $PSScriptRoot "sandbox_msvc_env.ps1") -Build | Out-Null

& cmake --build $build --target tiles3d_tests --parallel 2>&1 |
    Tee-Object -FilePath (Join-Path $root "tests_build.log") | Select-Object -Last 8

$exe = Join-Path $build "tests\tiles3d_tests.exe"
if (-not (Test-Path $exe)) { throw "test binary missing: $exe" }
Write-Host ("exe mtime: " + (Get-Item $exe).LastWriteTime)

& $exe 2>&1 | Tee-Object -FilePath (Join-Path $root "tests_run.log") | Select-Object -Last 12
exit $LASTEXITCODE
