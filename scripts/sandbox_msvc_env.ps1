# sandbox_msvc_env.ps1
#
# Sets the MSVC + Windows SDK build environment natively in the current PowerShell
# session, instead of shelling out to vcvars64.bat.
#
# Why this exists: scripts/msvc_env.bat drives vswhere.exe, which in turn calls
# reg.exe. This machine's sandbox blacklists reg.exe, so the bat file fails *silently*
# and the build dies later with `fatal error C1083: cannot open stddef.h` - the
# symptom of a missing UCRT/Windows SDK INCLUDE path, not a code problem. Writing the
# paths out by hand removes the dependency entirely.
#
# ORDERING IS LOAD-BEARING. This script only *sets variables*. Build with -Build, which
# runs after the assignments below. Putting the build first compiles with no INCLUDE and
# fails with `C1083: cannot open include file: 'cassert'` - and because ninja prints
# "no work to do" when the object is up to date, that failure is easy to miss and leaves
# you believing a stale DLL is current.
#
# The versions below must match build/windows-editor/CMakeCache.txt. They are also the
# only pair this machine's toolchain was validated against; changing them without a
# fresh CMake configure produces a mismatched-ABI link or, worse, a build that links
# against the wrong runtime.
#
# Usage:
#   . scripts\sandbox_msvc_env.ps1                 # set env only
#   . scripts\sandbox_msvc_env.ps1 -Build          # set env, then build + install
#   . scripts\sandbox_msvc_env.ps1 -Build -NoInstall

param(
    # Build after setting the environment. The install step is not optional in practice:
    # Godot resolves
    #   windows.x86_64 = "res://addons/lib/Windows-AMD64/godot-3dtiles-d.dll"
    # so a bare build leaves the demo running the PREVIOUS binary. Every shader and uniform
    # change then appears to do nothing and the render is byte-identical run to run, which
    # reads exactly like "my fix has no effect". Symptom to recognise: build output looks
    # fresh, but demo/globe_preview.png keeps an unchanged size across different inputs.
    [switch]$Build,
    [switch]$NoInstall
)

$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$msvcVersion  = '14.36.32532'
$sdkVersion   = '10.0.22621.0'
$vsRoot       = 'D:\Program Files\Microsoft Visual Studio\2022\Community'
$sdkRoot      = 'C:\Program Files (x86)\Windows Kits\10'

$msvcRoot = Join-Path $vsRoot "VC\Tools\MSVC\$msvcVersion"
if (-not (Test-Path $msvcRoot)) {
    throw "MSVC $msvcVersion not found at $msvcRoot - update the version to match CMakeCache.txt"
}
$sdkInclude = Join-Path $sdkRoot "Include\$sdkVersion"
if (-not (Test-Path $sdkInclude)) {
    throw "Windows SDK $sdkVersion not found at $sdkInclude"
}

# The x64 host toolchain is the only one configured, so everything is Hostx64\x64.
$bin = @(
    (Join-Path $msvcRoot 'bin\Hostx64\x64'),
    (Join-Path $sdkRoot "bin\$sdkVersion\x64"),
    (Join-Path $sdkRoot 'bin\x64')
)
$env:PATH = ($bin -join ';') + ';' + $env:PATH

$include = @(
    (Join-Path $msvcRoot 'include'),
    (Join-Path $sdkInclude 'ucrt'),
    (Join-Path $sdkInclude 'shared'),
    (Join-Path $sdkInclude 'um'),
    (Join-Path $sdkInclude 'winrt'),
    (Join-Path $sdkInclude 'cppwinrt')
)
$env:INCLUDE = $include -join ';'

$lib = @(
    (Join-Path $msvcRoot 'lib\x64'),
    (Join-Path $sdkRoot "Lib\$sdkVersion\ucrt\x64"),
    (Join-Path $sdkRoot "Lib\$sdkVersion\um\x64")
)
$env:LIB = $lib -join ';'

$env:VSCMD_ARG_HOST_ARCH = 'x64'
$env:VSCMD_ARG_TGT_ARCH  = 'x64'
$env:Platform            = 'x64'

# cl.exe reads this to find its own CRTs when invoked outside a developer prompt.
$env:VCToolsInstallDir = $msvcRoot + '\'
$env:WindowsSdkDir     = $sdkRoot + '\'

Write-Host "MSVC $msvcVersion + SDK $sdkVersion environment set (x64)."

if (-not $Build) {
    return
}

cmake --build build/windows-editor --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

if (-not $NoInstall) {
    cmake --install build/windows-editor
    if ($LASTEXITCODE -ne 0) { throw "install failed ($LASTEXITCODE)" }
    $dll = 'demo\addons\lib\Windows-AMD64\godot-3dtiles-d.dll'
    if (-not (Test-Path $dll)) {
        throw "install reported success but $dll is missing"
    }
    $f = Get-Item $dll
    Write-Host ("installed {0}  {1} bytes  {2:HH:mm:ss}" -f $f.Name, $f.Length, $f.LastWriteTime)
}
