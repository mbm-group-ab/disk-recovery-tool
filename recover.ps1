#Requires -Version 5.1
<#
.SYNOPSIS
  Interactive menu on top of the disk-recovery-tool executables.

.DESCRIPTION
  Every action prints the exact command line before it runs and appends it to
  <work-dir>\recover.log, so a run can be repeated by hand. The script never
  writes to the source disk; the executables open it read-only.

  Usage:  right-click PowerShell -> Run as Administrator, then
          .\recover.ps1 [-Bin <dir with the exes>] [-WorkDir <dir for map/plan/logs>]
#>
[CmdletBinding()]
param(
    [string]$Bin = '',
    [string]$WorkDir = ''
)

$ErrorActionPreference = 'Stop'
$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $Bin) { $Bin = Join-Path $ScriptDir 'build' }

# ---------------------------------------------------------------- tool paths
function Find-Tool([string]$Name) {
    foreach ($candidate in @(
        (Join-Path $Bin $Name),
        (Join-Path $Bin "imager\$Name"),
        (Join-Path $Bin "exfat-parser\$Name"),
        (Join-Path $Bin "carver\$Name"),
        (Join-Path $Bin "map-tool\$Name"),
        (Join-Path $ScriptDir $Name))) {
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    return $null
}

$Tools = @{
    Imager  = Find-Tool 'imager.exe'
    Parser  = Find-Tool 'exfat_recovery.exe'
    Carver  = Find-Tool 'recovery_carver.exe'
    MapTool = Find-Tool 'rescue_map_tool.exe'
}

# ---------------------------------------------------------------- state
$State = @{
    Source      = ''      # \\.\PhysicalDriveN or an image/.rci path
    SourceBytes = 0
    Image       = ''      # destination image (.img or .rci)
    Plan        = ''
    Recovered   = ''
}

function Get-WorkDir {
    if ($WorkDir) { return $WorkDir }
    if ($State.Image) { return (Split-Path -Parent $State.Image) }
    return $ScriptDir
}

function Write-Log([string]$Text) {
    $dir = Get-WorkDir
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $line = "{0}  {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Text
    Add-Content -LiteralPath (Join-Path $dir 'recover.log') -Value $line -Encoding UTF8
}

function Format-Bytes([double]$Bytes) {
    if ($Bytes -ge 1TB) { return ('{0:N2} TB' -f ($Bytes / 1TB)) }
    if ($Bytes -ge 1GB) { return ('{0:N2} GB' -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ('{0:N2} MB' -f ($Bytes / 1MB)) }
    return ('{0:N0} B' -f $Bytes)
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Quote-Arg([string]$Value) {
    if ($Value -match '[\s"]') { return '"' + ($Value -replace '"', '\"') + '"' }
    return $Value
}

# Runs an executable, echoing and logging the exact command. Returns exit code.
function Invoke-Tool([string]$Exe, [string[]]$Arguments, [switch]$Confirm) {
    if (-not $Exe) { Write-Host 'executable not found; build first (see README)' -ForegroundColor Red; return -1 }
    $display = (Quote-Arg $Exe) + ' ' + (($Arguments | ForEach-Object { Quote-Arg $_ }) -join ' ')
    Write-Host ''
    Write-Host 'command:' -ForegroundColor Cyan
    Write-Host "  $display"
    if ($Confirm) {
        $answer = Read-Host 'run it? [y/N]'
        if ($answer -notmatch '^[yY]') { Write-Host 'cancelled'; return -2 }
    }
    Write-Log "RUN  $display"
    $started = Get-Date
    & $Exe @Arguments
    $code = $LASTEXITCODE
    $elapsed = (Get-Date) - $started
    Write-Log ("EXIT {0}  after {1:hh\:mm\:ss}" -f $code, $elapsed)
    Write-Host ''
    Write-Host ("exit code {0}  ({1:hh\:mm\:ss})" -f $code, $elapsed) -ForegroundColor $(if ($code -eq 0) { 'Green' } else { 'Yellow' })
    return $code
}

# ---------------------------------------------------------------- disks
function Show-PhysicalDrives {
    Write-Host ''
    Write-Host 'physical drives:' -ForegroundColor Cyan
    $disks = Get-CimInstance Win32_DiskDrive | Sort-Object Index
    $systemDisk = (Get-Partition -DriveLetter C -ErrorAction SilentlyContinue).DiskNumber
    foreach ($d in $disks) {
        $letters = @()
        try {
            $parts = Get-Partition -DiskNumber $d.Index -ErrorAction Stop
            $letters = $parts | Where-Object DriveLetter | ForEach-Object { "$($_.DriveLetter):" }
        } catch {}
        $tag = ''
        if ($d.Index -eq $systemDisk) { $tag = '  <-- SYSTEM, never use as source' }
        Write-Host ('  [{0}] \\.\PhysicalDrive{0}  {1,-28} {2,10}  {3}  {4}{5}' -f
            $d.Index, $d.Model, (Format-Bytes $d.Size), $d.InterfaceType, ($letters -join ' '), $tag)
    }
}

function Show-Volumes {
    Write-Host ''
    Write-Host 'volumes (free space):' -ForegroundColor Cyan
    Get-CimInstance Win32_LogicalDisk | Where-Object { $_.Size } | Sort-Object DeviceID | ForEach-Object {
        Write-Host ('  {0}  {1,-6} {2,-12} free {3,10} of {4,10}' -f
            $_.DeviceID, $_.FileSystem, $_.VolumeName, (Format-Bytes $_.FreeSpace), (Format-Bytes $_.Size))
    }
}

function Get-SourceBytes([string]$Source) {
    if ($Source -match '^\\\\\.\\PhysicalDrive(\d+)$') {
        $d = Get-CimInstance Win32_DiskDrive -Filter "Index=$($Matches[1])"
        if ($d) { return [uint64]$d.Size }
        return 0
    }
    if (Test-Path -LiteralPath $Source) {
        if ($Source -like '*.rci') {
            foreach ($line in (Get-Content -LiteralPath $Source -TotalCount 20)) {
                if ($line -match '^total\s+(\d+)') { return [uint64]$Matches[1] }
            }
            return 0
        }
        return [uint64](Get-Item -LiteralPath $Source).Length
    }
    return 0
}

# ---------------------------------------------------------------- prompts
function Read-Default([string]$Prompt, [string]$Default) {
    $shown = if ($Default) { "$Prompt [$Default]" } else { $Prompt }
    $value = Read-Host $shown
    if ([string]::IsNullOrWhiteSpace($value)) { return $Default }
    return $value.Trim()
}

function Select-Source {
    Show-PhysicalDrives
    Write-Host ''
    Write-Host 'enter a disk number (e.g. 1), a full \\.\PhysicalDriveN, or the path of an existing .img/.rci'
    $value = Read-Default 'source' $State.Source
    if ($value -match '^\d+$') { $value = "\\.\PhysicalDrive$value" }
    if ($value -match '^\\\\\.\\PhysicalDrive(\d+)$') {
        $systemDisk = (Get-Partition -DriveLetter C -ErrorAction SilentlyContinue).DiskNumber
        if ([int]$Matches[1] -eq $systemDisk) {
            Write-Host 'that is the system disk; refusing.' -ForegroundColor Red
            return
        }
        if (-not (Test-Admin)) { Write-Host 'raw disk access needs an Administrator PowerShell' -ForegroundColor Yellow }
    } elseif (-not (Test-Path -LiteralPath $value)) {
        Write-Host "not found: $value" -ForegroundColor Red
        return
    }
    $State.Source = $value
    $State.SourceBytes = Get-SourceBytes $value
    Write-Host ("source = {0}  ({1})" -f $value, (Format-Bytes $State.SourceBytes)) -ForegroundColor Green
}

function Require-Source {
    if (-not $State.Source) { Select-Source }
    return [bool]$State.Source
}

# ---------------------------------------------------------------- actions
function Action-Image {
    if (-not (Require-Source)) { return }
    if ($State.Source -notmatch '^\\\\\.\\PhysicalDrive') {
        Write-Host 'note: source is a file, not a physical drive (fine for tests or for copying an image)' -ForegroundColor Yellow
    }
    Show-Volumes
    Write-Host ''
    Write-Host 'destination: a .img path (single file, needs free space >= source) or a .rci path (chunked, several destinations)'
    $dest = Read-Default 'destination image' $State.Image
    if (-not $dest) { return }
    $State.Image = $dest
    $args = @($State.Source, $dest)

    $resume = $false
    if (Test-Path -LiteralPath "$dest.map") {
        if ((Read-Default 'a map exists for this destination; resume? [Y/n]' 'y') -match '^[yY]') { $args += '--resume'; $resume = $true }
    }

    if ($dest -like '*.rci' -and -not $resume) {
        $chunk = Read-Default 'chunk size' '4G'
        $args += @('--chunk-size', $chunk)
        if ((Read-Default 'lazy parts (skip all-zero regions, needed when destinations are smaller than the disk)? [Y/n]' 'y') -match '^[yY]') { $args += '--lazy-parts' }
        Write-Host 'chunk destinations: directory and capacity, one per line; empty line to finish (e.g. E:\recovery-chunks 1000G)'
        while ($true) {
            $line = Read-Host '  dest'
            if ([string]::IsNullOrWhiteSpace($line)) { break }
            $parts = $line.Trim() -split '\s+', 2
            if ($parts.Count -ne 2) { Write-Host '  need: <dir> <capacity>'; continue }
            $args += @('--chunk-dest', $parts[0], $parts[1])
        }
    }

    Write-Host ''
    Write-Host 'drive-safety options (defaults are tuned for a flaky USB bridge; press Enter to accept)'
    $args += @('--max-consecutive-failures', (Read-Default 'stop after N consecutive failed reads' '20'))
    $args += @('--read-timeout-ms', (Read-Default 'read timeout ms' '30000'))
    $retry = Read-Default 'retry-bad passes over bad sectors at the end' '0'
    if ([int]$retry -gt 0) { $args += @('--retry-bad', $retry) }
    $minFree = Read-Default 'stop when destination free space drops below (e.g. 40G; empty = no limit)' ''
    if ($minFree) { $args += @('--min-dest-free', $minFree) }
    $pauseEvery = Read-Default 'cooling pause every (e.g. 100G; empty = none)' ''
    if ($pauseEvery) {
        $args += @('--pause-every', $pauseEvery, '--pause-seconds', (Read-Default 'pause seconds' '300'))
    }

    $code = Invoke-Tool $Tools.Imager $args -Confirm
    switch ($code) {
        0 { Write-Host 'imaging complete. next: 3 (scan) then 4 (recover) on the image' -ForegroundColor Green }
        3 { Write-Host 'stopped: destination low on space. free space or add a destination, then choose 2 again and resume' -ForegroundColor Yellow }
        4 { Write-Host 'stopped: drive not responding. unplug/replug (power-cycle) it, then choose 2 again and resume' -ForegroundColor Yellow }
        default { if ($code -gt 0) { Write-Host 'imaging failed; see messages above and recover.log' -ForegroundColor Red } }
    }
}

function Get-RecoverySource {
    # Prefer the image if one was made; allow direct-from-drive as second choice.
    if ($State.Image -and (Test-Path -LiteralPath $State.Image)) {
        $choice = Read-Default "read from image '$($State.Image)' (i) or directly from source '$($State.Source)' (s)?" 'i'
        if ($choice -match '^[iI]') { return $State.Image }
    }
    if (-not (Require-Source)) { return $null }
    return $State.Source
}

function Action-Scan {
    $src = Get-RecoverySource
    if (-not $src) { return }
    $defaultPlan = if ($State.Plan) { $State.Plan } else { Join-Path (Get-WorkDir) 'plan.csv' }
    $plan = Read-Default 'plan file to write' $defaultPlan
    $State.Plan = $plan
    $args = @($src, '--scan', $plan)
    if ((Read-Default 'include deleted files? [y/N]' 'n') -match '^[yY]') { $args += '--include-deleted' }
    $ext = Read-Default 'only extensions (comma separated, empty = all)' ''
    if ($ext) { $args += @('--only-ext', $ext) }
    $path = Read-Default 'path contains (comma separated, empty = all)' ''
    if ($path) { $args += @('--path-contains', $path) }
    $min = Read-Default 'min size (e.g. 1MB, empty = none)' ''
    if ($min) { $args += @('--min-size', $min) }
    $code = Invoke-Tool $Tools.Parser $args
    if ($code -eq 0) {
        Show-PlanSummary $plan
        Write-Host "edit the 'selected' column in $plan (0 = skip) if you want, then choose 4" -ForegroundColor Green
    }
}

function Show-PlanSummary([string]$Plan) {
    if (-not (Test-Path -LiteralPath $Plan)) { return }
    $rows = Import-Csv -LiteralPath $Plan
    $files = $rows | Where-Object { $_.selected -eq '1' }
    Write-Host ''
    Write-Host ("plan: {0} files, {1} total" -f $files.Count, (Format-Bytes (($files | Measure-Object -Property logical_size -Sum).Sum)))
    $files | Group-Object status | Sort-Object Count -Descending | ForEach-Object {
        $bytes = ($_.Group | Measure-Object -Property logical_size -Sum).Sum
        Write-Host ('  {0,-26} {1,8} files  {2,10}' -f $_.Name, $_.Count, (Format-Bytes $bytes))
    }
    $damaged = ($files | Measure-Object -Property damaged_bytes -Sum).Sum
    if ($damaged -gt 0) { Write-Host ("  damaged bytes inside selected files: {0}" -f (Format-Bytes $damaged)) -ForegroundColor Yellow }
}

function Action-Recover {
    $src = Get-RecoverySource
    if (-not $src) { return }
    Show-Volumes
    $usePlan = ''
    if ($State.Plan -and (Test-Path -LiteralPath $State.Plan)) {
        if ((Read-Default "use plan $($State.Plan)? [Y/n]" 'y') -match '^[yY]') { $usePlan = $State.Plan }
    } else {
        $p = Read-Default 'plan file (empty = recover everything the filters match)' ''
        if ($p) { if (Test-Path -LiteralPath $p) { $usePlan = $p; $State.Plan = $p } else { Write-Host "not found: $p" -ForegroundColor Red; return } }
    }
    if ($usePlan) { Show-PlanSummary $usePlan }

    Write-Host 'destinations: directory and cap, one per line; empty line to finish. cap 0 = unlimited (e.g. E:\recovered 900G)'
    $args = @($src)
    $first = ''
    while ($true) {
        $line = Read-Host '  dest'
        if ([string]::IsNullOrWhiteSpace($line)) { break }
        $parts = $line.Trim() -split '\s+', 2
        if ($parts.Count -eq 1) { $parts += '0' }
        if (-not $first) { $first = $parts[0] }
        $args += @('--dest', $parts[0], $parts[1])
    }
    if (-not $first) { Write-Host 'no destination given'; return }
    $State.Recovered = $first
    if ($usePlan) { $args += @('--from-plan', $usePlan) }
    else {
        if ((Read-Default 'include deleted files? [y/N]' 'n') -match '^[yY]') { $args += '--include-deleted' }
        $ext = Read-Default 'only extensions (comma separated, empty = all)' ''
        if ($ext) { $args += @('--only-ext', $ext) }
        $path = Read-Default 'path contains (comma separated, empty = all)' ''
        if ($path) { $args += @('--path-contains', $path) }
    }
    $code = Invoke-Tool $Tools.Parser $args -Confirm
    if ($code -eq 0) { Show-ManifestSummary (Join-Path $first 'manifest.csv') }
}

function Show-ManifestSummary([string]$Manifest) {
    if (-not (Test-Path -LiteralPath $Manifest)) { return }
    $rows = Import-Csv -LiteralPath $Manifest | Where-Object { $_.selected -eq '1' }
    Write-Host ''
    Write-Host "manifest: $Manifest"
    $rows | Group-Object status | Sort-Object Count -Descending | ForEach-Object {
        $bytes = ($_.Group | Measure-Object -Property written_size -Sum).Sum
        $color = if ($_.Name -match 'complete') { 'Green' } elseif ($_.Name -match 'partial|unreadable|damaged|error') { 'Yellow' } else { 'Gray' }
        Write-Host ('  {0,-28} {1,8} files  {2,10}' -f $_.Name, $_.Count, (Format-Bytes $bytes)) -ForegroundColor $color
    }
}

function Action-Carve {
    $src = Get-RecoverySource
    if (-not $src) { return }
    $out = Read-Default 'carve output directory' (Join-Path (Get-WorkDir) 'carved')
    $args = @($src, $out, '--whole-image')
    $cap = Read-Default 'max output (e.g. 200G, empty = none)' ''
    if ($cap) { $args += @('--max-output', $cap) }
    if ((Read-Default 'include candidates crossing bad/unknown ranges? [y/N]' 'n') -match '^[yY]') { $args += '--include-partial' }
    Invoke-Tool $Tools.Carver $args -Confirm | Out-Null
}

function Action-Status {
    Write-Host ''
    Write-Host 'session:' -ForegroundColor Cyan
    Write-Host ("  source     : {0}  {1}" -f $State.Source, $(if ($State.SourceBytes) { '(' + (Format-Bytes $State.SourceBytes) + ')' } else { '' }))
    Write-Host ("  image      : {0}" -f $State.Image)
    Write-Host ("  plan       : {0}" -f $State.Plan)
    Write-Host ("  recovered  : {0}" -f $State.Recovered)
    Write-Host ("  log        : {0}" -f (Join-Path (Get-WorkDir) 'recover.log'))
    Write-Host ''
    Write-Host 'tools:' -ForegroundColor Cyan
    foreach ($k in $Tools.Keys) { Write-Host ('  {0,-8} {1}' -f $k, $(if ($Tools[$k]) { $Tools[$k] } else { 'NOT FOUND' })) }

    if ($State.Image -and (Test-Path -LiteralPath "$($State.Image).report.json")) {
        Write-Host ''
        Write-Host "imaging report ($($State.Image).report.json):" -ForegroundColor Cyan
        $r = Get-Content -LiteralPath "$($State.Image).report.json" -Raw | ConvertFrom-Json
        $total = [double]$r.rescued_bytes + [double]$r.bad_bytes + [double]$r.pending_bytes
        Write-Host ('  rescued  {0,12}  ({1:N2}%)' -f (Format-Bytes $r.rescued_bytes), $(if ($total) { 100 * $r.rescued_bytes / $total } else { 0 }))
        Write-Host ('  bad      {0,12}  ({1} sectors confirmed)' -f (Format-Bytes $r.bad_bytes), $r.confirmed_bad_sectors)
        Write-Host ('  pending  {0,12}' -f (Format-Bytes $r.pending_bytes))
        Write-Host ('  transient read errors {0}, last Windows error {1}, finalized {2}' -f $r.transient_read_errors, $r.last_error, $r.finalized)
        if ($State.SourceBytes -and $Tools.MapTool) {
            Invoke-Tool $Tools.MapTool @('validate', "$($State.Image).map", "$($State.SourceBytes)") | Out-Null
        }
    }
    if ($State.Plan -and (Test-Path -LiteralPath $State.Plan)) { Show-PlanSummary $State.Plan }
    if ($State.Recovered -and (Test-Path -LiteralPath (Join-Path $State.Recovered 'manifest.csv'))) { Show-ManifestSummary (Join-Path $State.Recovered 'manifest.csv') }
}

function Action-RepairMap {
    $img = Read-Default 'image whose .map should be repaired' $State.Image
    if (-not $img) { return }
    $bytes = Read-Default 'source size in bytes' "$($State.SourceBytes)"
    Invoke-Tool $Tools.MapTool @('repair', "$img.map", $bytes) -Confirm | Out-Null
}

# ---------------------------------------------------------------- menu
if (-not (Test-Admin)) {
    Write-Host 'not running as Administrator: physical drives cannot be opened; image files still work.' -ForegroundColor Yellow
}
$missing = $Tools.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key }
if ($missing) { Write-Host ("missing executables: {0}  (build with cmake first, or pass -Bin)" -f ($missing -join ', ')) -ForegroundColor Yellow }

while ($true) {
    Write-Host ''
    Write-Host '================ disk recovery ================' -ForegroundColor Cyan
    Write-Host ("  source: {0}    image: {1}" -f $(if ($State.Source) { $State.Source } else { '(none)' }), $(if ($State.Image) { $State.Image } else { '(none)' }))
    Write-Host '  1  choose source disk / image'
    Write-Host '  2  image the disk            (stage 1, resumable)'
    Write-Host '  3  scan: inventory files -> plan.csv   (no data written)'
    Write-Host '  4  recover files by name    (stage 2, from plan or filters)'
    Write-Host '  5  carve unclaimed space    (stage 3, last resort)'
    Write-Host '  6  status / reports / validate map'
    Write-Host '  7  repair a damaged rescue map from its backup'
    Write-Host '  8  list drives and volumes'
    Write-Host '  q  quit'
    $choice = Read-Host 'choice'
    try {
        switch ($choice.Trim()) {
            '1' { Select-Source }
            '2' { Action-Image }
            '3' { Action-Scan }
            '4' { Action-Recover }
            '5' { Action-Carve }
            '6' { Action-Status }
            '7' { Action-RepairMap }
            '8' { Show-PhysicalDrives; Show-Volumes }
            'q' { return }
            default { }
        }
    } catch {
        Write-Host ("error: {0}" -f $_.Exception.Message) -ForegroundColor Red
        Write-Log ("ERROR {0}" -f $_.Exception.Message)
    }
}
