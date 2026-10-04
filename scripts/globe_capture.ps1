# Runs the globe capture audit with a hard wall-clock bound and guaranteed cleanup.
#
# Why this exists: the capture is a GPU scene launched from a batch context. If it stalls,
# a bare `godot ...` call blocks the caller forever and there is no way to tell "slow" from
# "wedged". Two independent guards are used so neither is a single point of failure:
#   1. demo/globe_capture.gd carries its own in-engine watchdog that quits the process.
#   2. This script kills the process if it outlives -TimeoutSeconds regardless.
#
# Exit codes: 0 = audit ran, 2 = killed by the in-engine watchdog, 124 = killed by this
# script's bound, anything else = Godot's own failure.

param(
    [double]$AtmoIntensity = -1,
    [double]$AtmoScale = -1,
    [int]$TimeoutSeconds = 120,
    # Relative to the project root, and intentionally under log/: run output at the root buried
    # the files that are actually source.
    [string]$LogPath = "log/globe_capture.log"
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path (Join-Path $PWD "log") | Out-Null
$godot = "E:\Games\godot\Godot_v4.7.2-stable_win64_console.exe"
$demo = Join-Path $PSScriptRoot "..\demo"

if (-not (Test-Path $godot)) { throw "Godot not found: $godot" }

if ($AtmoIntensity -ge 0) { $env:GLOBE_ATMO_INTENSITY = "$AtmoIntensity" }
if ($AtmoScale -ge 0) { $env:GLOBE_ATMO_SCALE = "$AtmoScale" }

# System.Diagnostics.Process, not Start-Process.
#
# Start-Process with -RedirectStandardOutput rebuilds the child environment block from
# $env: and dies on a case-collision ("Key in dictionary: 'Path' Key being added: 'PATH'")
# that this machine's environment carries. Building the ProcessStartInfo by hand avoids that
# code path completely and gives a real, boundable WaitForExit(ms).
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $godot
$psi.Arguments = "--path `"$demo`" --rendering-driver opengl3 res://globe_capture.tscn"
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true
$psi.WorkingDirectory = $demo

$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi
[void]$proc.Start()

# Drain both pipes asynchronously: a full OS pipe buffer would otherwise deadlock a chatty
# child before it ever exits, which looks exactly like a hang.
$stdout = $proc.StandardOutput.ReadToEndAsync()
$stderr = $proc.StandardError.ReadToEndAsync()

$finished = $proc.WaitForExit($TimeoutSeconds * 1000)

if (-not $finished) {
    Write-Warning "capture exceeded ${TimeoutSeconds}s - killing PID $($proc.Id)"
    try { $proc.Kill() } catch {}
    $code = 124
} else {
    $code = $proc.ExitCode
}

# Godot writes console output as UTF-16LE; the async readers decode it as UTF-8 by default,
# so re-read the raw bytes and decode explicitly for a greppable log.
[System.IO.File]::WriteAllText((Join-Path $PWD $LogPath), $stdout.Result, [System.Text.Encoding]::Unicode)
[System.IO.File]::WriteAllText((Join-Path $PWD "$LogPath.err"), $stderr.Result, [System.Text.Encoding]::Unicode)
$proc.Dispose()

# Clear the overrides, but only if this invocation actually set them: removing an unset
# variable throws, and this shell's file-safety hook turns that into a hard error rather
# than the warning -ErrorAction normally would.
if ($AtmoIntensity -ge 0) { Remove-Item Env:\GLOBE_ATMO_INTENSITY -ErrorAction SilentlyContinue }
if ($AtmoScale -ge 0) { Remove-Item Env:\GLOBE_ATMO_SCALE -ErrorAction SilentlyContinue }

Write-Host "capture exit=$code  log=$LogPath"

# A GDScript parse error makes Godot load no scene at all: _ready() never runs, the in-engine
# watchdog never arms, and the process idles until the bound above kills it. That surfaces as
# a bare exit 124 with no clue as to why, so the log is scanned for it and reported here.
$errPath = "$LogPath.err"
if (Test-Path $errPath) {
    $errText = [System.IO.File]::ReadAllText($errPath, [System.Text.Encoding]::Unicode)
    foreach ($needle in @("Parse Error", "Failed to load script", "SCRIPT ERROR")) {
        if ($errText -match [regex]::Escape($needle)) {
            Write-Warning "Godot reported '$needle' - the scene never loaded. Fix the script,"
            Write-Warning "the exit code below is the watchdog, not the real failure."
            $lines = $errText -split "`r?`n" | Where-Object { $_ -match "Parse Error|SCRIPT ERROR" } | Select-Object -First 4
            $lines | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
            exit 3
        }
    }
}

exit $code
