param([Parameter(Mandatory = $true)][string]$Imager)

$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) "disk_recovery_imager_test_$PID"
$source = Join-Path $testRoot 'source.bin'
$image = Join-Path $testRoot 'image.bin'
$chunkIndex = Join-Path $testRoot 'chunked.rci'
$chunkA = Join-Path $testRoot 'chunk-a'
$chunkB = Join-Path $testRoot 'chunk-b'

function Get-Sha256([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $inputStream = [IO.File]::OpenRead($Path)
        try { return [BitConverter]::ToString($sha.ComputeHash($inputStream)).Replace('-', '') }
        finally { $inputStream.Dispose() }
    }
    finally { $sha.Dispose() }
}

function Get-ChunkedSha256([string]$IndexPath) {
    $memory = [IO.MemoryStream]::new()
    try {
        foreach ($line in [IO.File]::ReadAllLines($IndexPath)) {
            if ($line -match '^part\s+\d+\s+\d+\s+"([^"]+)"') {
                $part = $Matches[1] -replace '/', '\'
                $stream = [IO.File]::OpenRead($part)
                try { $stream.CopyTo($memory) } finally { $stream.Dispose() }
            }
        }
        $sha = [Security.Cryptography.SHA256]::Create()
        try { return [BitConverter]::ToString($sha.ComputeHash($memory.ToArray())).Replace('-', '') }
        finally { $sha.Dispose() }
    }
    finally { $memory.Dispose() }
}

try {
    New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
    $bytes = [byte[]]::new(1MB)
    $rng = [Random]::new(123456)
    $rng.NextBytes($bytes)
    [IO.File]::WriteAllBytes($source, $bytes)

    & $Imager $source $image --sector 512 --read-size 128K --min-read-size 4K `
        --retries 1 --read-timeout-ms 5000 --skip-size 256K | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "initial imaging exited $LASTEXITCODE" }
    if ((Get-Sha256 $source) -ne (Get-Sha256 $image)) {
        throw 'image hash differs from source hash'
    }
    if (-not (Test-Path -LiteralPath "$image.map")) { throw 'rescue map was not created' }
    $flatReport = [IO.File]::ReadAllText("$image.report.json")
    if ($flatReport -notmatch '"finalized": true' -or
        $flatReport -notmatch '"pending_bytes": 0' -or
        $flatReport -notmatch '"generation": "[^"]+"' -or
        $flatReport -notmatch '"physical_sector_size": 512' -or
        $flatReport -notmatch '"transient_read_errors": 0') {
        throw 'final machine-readable report is incomplete'
    }

    $deferredMap = ([IO.File]::ReadAllText("$image.map") -replace '(?m)^0 1048576 \+\r?$', '0 1048576 *')
    if ($deferredMap -notmatch '(?m)^0 1048576 \*$') { throw 'could not construct deferred map fixture' }
    [IO.File]::WriteAllText("$image.map", $deferredMap)

    & $Imager $source $image --resume --sector 512 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "resume imaging exited $LASTEXITCODE" }
    if ((Get-Sha256 $source) -ne (Get-Sha256 $image)) {
        throw 'resumed image hash differs from source hash'
    }
    if ([IO.File]::ReadAllText("$image.map") -match '(?m)^\d+ \d+ \*$') {
        throw 'deferred fast-pass range was not trimmed/scraped on resume'
    }

    $otherSource = Join-Path $testRoot 'other-source.bin'
    $otherBytes = [byte[]]::new(1MB)
    ([Random]::new(654321)).NextBytes($otherBytes)
    [IO.File]::WriteAllBytes($otherSource, $otherBytes)
    $imageHashBeforeMismatch = Get-Sha256 $image
    & $Imager $otherSource $image --resume --sector 512 | Out-Host
    if ($LASTEXITCODE -eq 0) { throw 'resume accepted a map for another source' }
    if ((Get-Sha256 $image) -ne $imageHashBeforeMismatch) {
        throw 'identity mismatch modified the existing image'
    }

    & $Imager $source $chunkIndex --sector 512 --chunk-size 256K `
        --chunk-dest $chunkA 512K --chunk-dest $chunkB 512K | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "chunked imaging exited $LASTEXITCODE" }
    if ((Get-Sha256 $source) -ne (Get-ChunkedSha256 $chunkIndex)) {
        throw 'chunked image hash differs from source hash'
    }
    $indexText = [IO.File]::ReadAllText($chunkIndex)
    if (($indexText -split "`npart ").Count -ne 5 -or $indexText -notmatch ' sha256 [0-9a-f]{64}') {
        throw 'chunk index does not contain four checksummed parts'
    }

    & $Imager $source $chunkIndex --resume --sector 512 --chunk-size 256K `
        --chunk-dest $chunkA 512K --chunk-dest $chunkB 512K | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "chunked resume exited $LASTEXITCODE" }
    if ((Get-Sha256 $source) -ne (Get-ChunkedSha256 $chunkIndex)) {
        throw 'resumed chunked image hash differs from source hash'
    }

    $tooSmallIndex = Join-Path $testRoot 'too-small.rci'
    $tooSmallRoot = Join-Path $testRoot 'too-small'
    & $Imager $source $tooSmallIndex --sector 512 --chunk-size 256K `
        --chunk-dest $tooSmallRoot 512K | Out-Host
    if ($LASTEXITCODE -eq 0) { throw 'insufficient chunk capacity was not rejected' }
    if (Test-Path -LiteralPath $tooSmallIndex) { throw 'failed capacity preflight left an index' }
    if ((Get-ChildItem -LiteralPath $tooSmallRoot -File -ErrorAction SilentlyContinue).Count -ne 0) {
        throw 'failed capacity preflight created part files'
    }

    # A generated part must never be allowed to alias and truncate the source.
    $aliasRoot = Join-Path $testRoot 'alias'
    New-Item -ItemType Directory -Force -Path $aliasRoot | Out-Null
    $aliasSource = Join-Path $aliasRoot 'danger.part-000000.bin'
    [IO.File]::WriteAllBytes($aliasSource, $bytes)
    $aliasHash = Get-Sha256 $aliasSource
    & $Imager $aliasSource (Join-Path $testRoot 'danger.rci') --sector 512 `
        --chunk-size 256K --chunk-dest $aliasRoot 1M | Out-Host
    if ($LASTEXITCODE -eq 0) { throw 'chunk source/part alias was not rejected' }
    if ((Get-Sha256 $aliasSource) -ne $aliasHash) { throw 'alias rejection modified source data' }

    Remove-Item -LiteralPath "$image.map" -Force
    if (Test-Path -LiteralPath "$image.map.bak") {
        Remove-Item -LiteralPath "$image.map.bak" -Force
    }
    & $Imager $source $image --resume --sector 512 | Out-Host
    if ($LASTEXITCODE -eq 0) { throw 'resume without a valid map was not rejected' }
    if ((Get-Sha256 $source) -ne (Get-Sha256 $image)) {
        throw 'rejected resume modified the existing image'
    }
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
        $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if (-not $resolvedTestRoot.StartsWith($resolvedTemp, [StringComparison]::OrdinalIgnoreCase)) {
            throw "refusing to remove test path outside temp: $resolvedTestRoot"
        }
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
