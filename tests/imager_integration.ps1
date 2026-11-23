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

    # DR1: a second run without --resume must refuse and leave image, map and report untouched.
    $imageHashBefore = Get-Sha256 $image
    $mapBefore = [IO.File]::ReadAllText("$image.map")
    & $Imager $source $image --sector 512 | Out-Host
    if ($LASTEXITCODE -ne 6) { throw "rerun without --resume exited $LASTEXITCODE, expected refusal (6)" }
    if ((Get-Sha256 $image) -ne $imageHashBefore) { throw 'rerun without --resume changed the image' }
    if ([IO.File]::ReadAllText("$image.map") -ne $mapBefore) { throw 'rerun changed the map' }

    # A stray map or report alone (image missing) is also refused.
    $mapOnly = Join-Path $testRoot 'maponly.bin'
    [IO.File]::WriteAllText("$mapOnly.map", 'keep')
    & $Imager $source $mapOnly --sector 512 | Out-Host
    if ($LASTEXITCODE -ne 6 -or (Test-Path -LiteralPath $mapOnly)) { throw 'map-only collision was not refused' }
    if ([IO.File]::ReadAllText("$mapOnly.map") -ne 'keep') { throw 'map-only file was changed' }
    $reportOnly = Join-Path $testRoot 'reportonly.bin'
    $customReport = Join-Path $testRoot 'custom-report.json'
    [IO.File]::WriteAllText($customReport, 'keep')
    & $Imager $source $reportOnly --sector 512 --report $customReport | Out-Host
    if ($LASTEXITCODE -ne 6 -or (Test-Path -LiteralPath $reportOnly)) { throw 'report collision was not refused' }

    # A pending stop request is honoured before anything is read or written.
    [IO.File]::WriteAllText("$image.stop", '')
    & $Imager $source $image --resume --sector 512 | Out-Host
    if ($LASTEXITCODE -ne 5) { throw "pending stop request gave exit $LASTEXITCODE, expected 5" }
    if (Test-Path -LiteralPath "$image.stop") { throw 'stop request file was not consumed' }
    if ((Get-Sha256 $image) -ne $imageHashBefore) { throw 'stop request run changed the image' }

    # A hard link to the image under another name is an existing file too.
    $hardLink = Join-Path $testRoot 'image-link.bin'
    New-Item -ItemType HardLink -Path $hardLink -Target $image | Out-Null
    & $Imager $source $hardLink --sector 512 | Out-Host
    if ($LASTEXITCODE -ne 6) { throw 'hard-link alias was not refused' }
    if ((Get-Sha256 $image) -ne $imageHashBefore) { throw 'hard-link run changed the image' }

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

    $indexBefore = [IO.File]::ReadAllText($chunkIndex)
    & $Imager $source $chunkIndex --sector 512 --chunk-size 256K `
        --chunk-dest $chunkA 512K --chunk-dest $chunkB 512K | Out-Host
    if ($LASTEXITCODE -ne 6) { throw 'chunked rerun without --resume was not refused' }
    if ([IO.File]::ReadAllText($chunkIndex) -ne $indexBefore) { throw 'chunked rerun changed the index' }

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

    # DR5: source identity on resume. Fresh source/image pair, imaged in full first.
    $idSource = Join-Path $testRoot 'id-source.bin'
    $idImage = Join-Path $testRoot 'id-image.img'
    $idBytes = [byte[]]::new(512KB)
    ([Random]::new(777)).NextBytes($idBytes)
    [IO.File]::WriteAllBytes($idSource, $idBytes)
    & $Imager $idSource $idImage --sector 512 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "identity fixture imaging exited $LASTEXITCODE" }
    if ([IO.File]::ReadAllText("$idImage.map") -notmatch '(?m)^#fingerprint "file:[0-9a-f:]+"') {
        throw 'map does not record the source fingerprint'
    }
    $idImageHash = Get-Sha256 $idImage

    & $Imager $idSource $idImage --resume --sector 512 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "resume of the same source exited $LASTEXITCODE" }

    # Same path, same size, different content written in place (same file id): the samples catch it.
    $changed = [byte[]]::new(512KB)
    ([Random]::new(778)).NextBytes($changed)
    $stream = [IO.File]::Open($idSource, 'Open', 'Write')
    try { $stream.Write($changed, 0, $changed.Length) } finally { $stream.Dispose() }
    & $Imager $idSource $idImage --resume --sector 512 | Out-Host
    if ($LASTEXITCODE -ne 7) { throw "in-place changed source gave exit $LASTEXITCODE, expected 7" }
    if ((Get-Sha256 $idImage) -ne $idImageHash) { throw 'refused resume (changed content) modified the image' }

    # Deleted and recreated at the same path with the original bytes: new file id -> refused.
    Remove-Item -LiteralPath $idSource -Force
    [IO.File]::WriteAllBytes($idSource, $idBytes)
    & $Imager $idSource $idImage --resume --sector 512 | Out-Host
    if ($LASTEXITCODE -ne 7) { throw "replaced source gave exit $LASTEXITCODE, expected 7" }
    if ((Get-Sha256 $idImage) -ne $idImageHash) { throw 'refused resume (new id) modified the image' }

    # Same bytes behind a new id are accepted only with --allow-new-source-id.
    & $Imager $idSource $idImage --resume --sector 512 --allow-new-source-id | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "--allow-new-source-id with matching data exited $LASTEXITCODE" }

    # ...but never when the rescued samples differ.
    Remove-Item -LiteralPath $idSource -Force
    [IO.File]::WriteAllBytes($idSource, $changed)
    & $Imager $idSource $idImage --resume --sector 512 --allow-new-source-id | Out-Host
    if ($LASTEXITCODE -ne 7) { throw "--allow-new-source-id with other data gave exit $LASTEXITCODE, expected 7" }
    if ((Get-Sha256 $idImage) -ne $idImageHash) { throw 'refused resume (override) modified the image' }

    # Destination on the source's own disk (disk numbers faked in the test build).
    $sameDisk = Join-Path $testRoot 'same-disk.img'
    $env:IMAGER_TEST_SOURCE_DISK = '3'
    $env:IMAGER_TEST_DEST_DISK = '3'
    try {
        & $Imager $idSource $sameDisk --sector 512 | Out-Host
        if ($LASTEXITCODE -ne 8) { throw "destination on the source disk gave exit $LASTEXITCODE, expected 8" }
        if ((Test-Path -LiteralPath $sameDisk) -or (Test-Path -LiteralPath "$sameDisk.map")) {
            throw 'refused same-disk run created files'
        }
        $env:IMAGER_TEST_DEST_DISK = '4'
        & $Imager $idSource $sameDisk --sector 512 | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "destination on another disk exited $LASTEXITCODE" }
    }
    finally {
        Remove-Item Env:IMAGER_TEST_SOURCE_DISK -ErrorAction SilentlyContinue
        Remove-Item Env:IMAGER_TEST_DEST_DISK -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath "$sameDisk.lock") { throw 'imager left its lock file behind' }

    # Operation lock: a second run on the same image (new or --resume) stops with 9.
    $lockedImage = Join-Path $testRoot 'locked.img'
    $holder = [IO.File]::Open("$lockedImage.lock", 'OpenOrCreate', 'ReadWrite', 'None')
    try {
        & $Imager $idSource $lockedImage --sector 512 | Out-Host
        if ($LASTEXITCODE -ne 9) { throw "imager on a locked image gave exit $LASTEXITCODE, expected 9" }
        if ((Test-Path -LiteralPath $lockedImage) -or (Test-Path -LiteralPath "$lockedImage.map")) {
            throw 'imager wrote while another run held the lock'
        }
    }
    finally { $holder.Dispose() }
    $holder = [IO.File]::Open("$idImage.lock", 'OpenOrCreate', 'ReadWrite', 'None')
    try {
        & $Imager $idSource $idImage --resume --sector 512 --allow-new-source-id | Out-Host
        if ($LASTEXITCODE -ne 9) { throw "resume on a locked image gave exit $LASTEXITCODE, expected 9" }
        if ((Get-Sha256 $idImage) -ne $idImageHash) { throw 'busy resume modified the image' }
    }
    finally { $holder.Dispose() }

    # DR6 fault injection: the destination fills up mid-run (test build hook). The map must
    # exist, never claim bytes that were not written, and --resume must finish a correct image.
    foreach ($case in @(@{ Name = 'before-checkpoint'; Checkpoint = '' },
                        @{ Name = 'after-checkpoints'; Checkpoint = '65536' })) {
        $faultImage = Join-Path $testRoot "fault-$($case.Name).img"
        $env:IMAGER_TEST_FAIL_WRITE_AFTER = '300000'
        if ($case.Checkpoint) { $env:IMAGER_TEST_CHECKPOINT_BYTES = $case.Checkpoint }
        try {
            & $Imager $idSource $faultImage --sector 512 --read-size 32K --min-read-size 4K | Out-Host
            if ($LASTEXITCODE -ne 1) { throw "$($case.Name): full destination gave exit $LASTEXITCODE, expected 1" }
        }
        finally {
            Remove-Item Env:IMAGER_TEST_FAIL_WRITE_AFTER -ErrorAction SilentlyContinue
            Remove-Item Env:IMAGER_TEST_CHECKPOINT_BYTES -ErrorAction SilentlyContinue
        }
        if (-not (Test-Path -LiteralPath "$faultImage.map")) { throw "$($case.Name): no map after the fault" }
        $claimed = 0
        foreach ($line in [IO.File]::ReadAllLines("$faultImage.map")) {
            if ($line -match '^(\d+) (\d+) \+$') { $claimed += [int64]$Matches[2] }
        }
        if ($claimed -gt 300000) { throw "$($case.Name): map claims $claimed bytes, more than were written" }
        if ($case.Checkpoint -and $claimed -eq 0) { throw "$($case.Name): no checkpoint was taken before the fault" }
        if ($claimed -gt 0) {
            $imageBytes = [IO.File]::ReadAllBytes($faultImage)
            for ($i = 0; $i -lt $claimed; $i += 4096) {
                if ($imageBytes[$i] -ne $changed[$i]) { throw "$($case.Name): claimed byte $i is not in the image" }
            }
        }
        & $Imager $idSource $faultImage --resume --sector 512 | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "$($case.Name): resume after the fault exited $LASTEXITCODE" }
        if ((Get-Sha256 $faultImage) -ne (Get-Sha256 $idSource)) { throw "$($case.Name): resumed image differs" }
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
