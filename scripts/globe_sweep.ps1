# Runs several globe_capture variants and keeps every log + PNG side by side.
#
# Why this exists: the question "is the surface glow too weak" cannot be answered by sweeping an
# intensity until a screenshot looks right - that converges on whatever the bug is, not on the
# reference. It can only be answered by decomposing the output into the stages that produce it and
# measuring each one, so each variant here differs from the baseline in exactly ONE input.
#
# The three that matter for the ground pass:
#   albedo    - the raw texture fetch, no lighting and no scattering. The floor.
#   groundoff - day/night Lambert only. What the imagery looks like without any air in front.
#   base      - the full composite. What we ship.
# If base is much darker than albedo, the loss is in the decode/exposure chain, not in the amount
# of scattering.
#
# Usage (from the repo root, PowerShell):
#   pwsh -File scripts\globe_sweep.ps1
#   pwsh -File scripts\globe_sweep.ps1 -Variants base,groundoff -Frames 420

param(
    [string[]]$Variants = @("base", "albedo", "groundoff"),
    [int]$Frames = 420,
    [int]$TimeoutSeconds = 90,
    [string]$Tag = ""
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# name -> environment variables to set for that run
$spec = @{
    "base"      = @{}
    "albedo"    = @{ GLOBE_DEBUG_ALBEDO = "1" }
    "decode"    = @{ GLOBE_DEBUG_ALBEDO = "2" }   # isolates the sRGB round trip
    "groundoff" = @{ GLOBE_GROUND_OFF    = "1" }
    "force"     = @{ GLOBE_FORCE_SHELL  = "1" }   # negative control: must FAIL
    "noveil"    = @{ GLOBE_DEBUG_PURE    = "1" }
    "atmo50"    = @{ GLOBE_ATMO_INTENSITY = "50" }   # the reference's own value
    "atmo70"    = @{ GLOBE_ATMO_INTENSITY = "70" }   # one step up, to see the response slope
}

# Every variant pins the same settle length. A control that takes 30 s per iteration is a control
# that stops getting run, and an unrun control is indistinguishable from a passing one.
$stamp = if ($Tag -ne "") { $Tag } else { Get-Date -Format "HHmmss" }

foreach ($v in $Variants) {
    if (-not $spec.ContainsKey($v)) {
        Write-Warning "unknown variant '$v' - known: $($spec.Keys -join ', ')"
        continue
    }
    $png = "demo/_sw_${v}_${stamp}.png"
    $log = "sweep_${v}_${stamp}.log"

    # Clear every knob first. Inherited values from an earlier iteration would silently make this
    # run a two-variable experiment, which is exactly the failure this script exists to prevent.
    foreach ($k in @("GLOBE_DEBUG_ALBEDO", "GLOBE_GROUND_OFF", "GLOBE_FORCE_SHELL",
                     "GLOBE_DEBUG_PURE", "GLOBE_HIDE_SURFACE", "GLOBE_ATMO_INTENSITY", "GLOBE_GROUND_INTENSITY", "GLOBE_FRAMES", "GLOBE_OUT")) {
        if (Test-Path "Env:\$k") { Remove-Item "Env:\$k" -ErrorAction SilentlyContinue }
    }
    foreach ($k in $spec[$v].Keys) { Set-Item -Path "Env:\$k" -Value $spec[$v][$k] }
    Set-Item -Path "Env:\GLOBE_FRAMES" -Value "$Frames"
    Set-Item -Path "Env:\GLOBE_OUT"    -Value "res://_sw_${v}_${stamp}.png"

    Write-Host "--- $v  -> $log / $png"
    # The runner takes the output path from the project root's cwd; give it the log path there too.
    & (Join-Path $PSScriptRoot "globe_capture.ps1") -TimeoutSeconds $TimeoutSeconds -LogPath $log
    Write-Host "    exit=$LASTEXITCODE"
}

# Leave no knob behind for the next thing that runs in this shell.
foreach ($k in @("GLOBE_DEBUG_ALBEDO", "GLOBE_GROUND_OFF", "GLOBE_FORCE_SHELL",
                 "GLOBE_DEBUG_PURE", "GLOBE_HIDE_SURFACE", "GLOBE_ATMO_INTENSITY", "GLOBE_GROUND_INTENSITY", "GLOBE_FRAMES", "GLOBE_OUT")) {
    if (Test-Path "Env:\$k") { Remove-Item "Env:\$k" -ErrorAction SilentlyContinue }
}

Write-Host ""
Write-Host "logs:  $root\sweep_*_${stamp}.log"
Write-Host "pngs:  $root\demo\_sw_*_${stamp}.png"

# Diff consecutive variants pairwise. The audit samples a dozen points, so two renders can differ
# measurably and still report identical numbers - that happened at 45 / 50 / 70, where only 0.27%
# of pixels moved, all in the ring outside the silhouette, below the visible 8-bit step. The PNG
# is the render; the audit is twelve samples of it. When they disagree, the PNG is the arbiter,
# and this is how you find out by how much.
$python = "C:\Users\weidx\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if (Test-Path $python) {
    Write-Host ""
    Write-Host "---- pairwise PNG diff ----"
    $done = @()
    foreach ($v in $Variants) {
        $cur = Join-Path $root "demo\_sw_${v}_${stamp}.png"
        if (-not (Test-Path $cur)) { continue }
        if ($done.Count -gt 0) {
            $prev = $done[-1]
            & $python (Join-Path $PSScriptRoot "png_diff.py") $prev $cur --top 4 |
                Select-Object -Skip 3 | Select-Object -First 8
        }
        $done += $cur
    }
}
