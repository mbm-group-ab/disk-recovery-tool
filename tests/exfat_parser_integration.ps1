param([Parameter(Mandatory = $true)][string]$Parser)

$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) "disk_recovery_parser_test_$PID"
$image = Join-Path $testRoot 'gpt-exfat.img'
$plan = Join-Path $testRoot 'plan.csv'
$damagedPlan = Join-Path $testRoot 'damaged-plan.csv'
$backupPlan = Join-Path $testRoot 'backup-plan.csv'
$metadataPlan = Join-Path $testRoot 'metadata-plan.csv'
$nameHashPlan = Join-Path $testRoot 'name-hash-plan.csv'
$allocationPlan = Join-Path $testRoot 'allocation-plan.csv'
$datePlan = Join-Path $testRoot 'date-plan.csv'
$chunkIndex = Join-Path $testRoot 'split.rci'
$chunkPlan = Join-Path $testRoot 'chunk-plan.csv'
$map = "$image.map"
$output = Join-Path $testRoot 'recovered'
$plannedOutput = Join-Path $testRoot 'planned-recovery'
$emptyOutput = Join-Path $testRoot 'empty-plan-recovery'
$emptyPlan = Join-Path $testRoot 'empty-plan.csv'
$blockedParent = Join-Path $testRoot 'blocked-parent'

function Get-ExfatNameHash([string]$Name) {
    [uint64]$hash = 0
    foreach ($byte in [Text.Encoding]::Unicode.GetBytes($Name.ToUpperInvariant())) {
        $hash = ((($hash -shr 1) -bor (($hash -band 1) -shl 15)) + $byte) -band 0xFFFF
    }
    return [uint16]$hash
}

try {
    New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
    $stream = [IO.File]::Open($image, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite)
    $stream.SetLength(4MB)
    $writer = [IO.BinaryWriter]::new($stream)

    $mbr = [byte[]]::new(512)
    $mbr[450] = 0xEE
    [BitConverter]::GetBytes([uint32]1).CopyTo($mbr, 454)
    [BitConverter]::GetBytes([uint32]8191).CopyTo($mbr, 458)
    $mbr[510] = 0x55; $mbr[511] = 0xAA
    $writer.Write($mbr)

    $stream.Position = 512
    $gpt = [byte[]]::new(512)
    [Text.Encoding]::ASCII.GetBytes('EFI PART').CopyTo($gpt, 0)
    [BitConverter]::GetBytes([uint64]2).CopyTo($gpt, 72)
    [BitConverter]::GetBytes([uint32]128).CopyTo($gpt, 80)
    [BitConverter]::GetBytes([uint32]128).CopyTo($gpt, 84)
    $writer.Write($gpt)

    $stream.Position = 1024
    $partition = [byte[]]::new(128)
    $partition[0] = 1
    [BitConverter]::GetBytes([uint64]2048).CopyTo($partition, 32)
    [BitConverter]::GetBytes([uint64]4095).CopyTo($partition, 40)
    $writer.Write($partition)

    $volumeOffset = [uint64](2048 * 512)
    $bootRegion = [byte[]]::new(12 * 512)
    $boot = [byte[]]::new(512)
    $boot[0] = 0xEB; $boot[1] = 0x76; $boot[2] = 0x90
    [Text.Encoding]::ASCII.GetBytes('EXFAT   ').CopyTo($boot, 3)
    [BitConverter]::GetBytes([uint64]2048).CopyTo($boot, 64)
    [BitConverter]::GetBytes([uint64]2048).CopyTo($boot, 72)
    [BitConverter]::GetBytes([uint32]24).CopyTo($boot, 80)
    [BitConverter]::GetBytes([uint32]1).CopyTo($boot, 84)
    [BitConverter]::GetBytes([uint32]25).CopyTo($boot, 88)
    [BitConverter]::GetBytes([uint32]100).CopyTo($boot, 92)
    [BitConverter]::GetBytes([uint32]2).CopyTo($boot, 96)
    $boot[108] = 9; $boot[109] = 0; $boot[110] = 1
    $boot[510] = 0x55; $boot[511] = 0xAA
    $boot.CopyTo($bootRegion, 0)
    for ($sector = 1; $sector -le 10; ++$sector) {
        $bootRegion[$sector * 512 + 510] = 0x55
        $bootRegion[$sector * 512 + 511] = 0xAA
    }
    [uint64]$checksum = 0
    for ($j = 0; $j -lt 11 * 512; ++$j) {
        if ($j -eq 106 -or $j -eq 107 -or $j -eq 112) { continue }
        $rotated = (($checksum -shr 1) -bor (($checksum -band 1) -shl 31))
        $checksum = ($rotated + $bootRegion[$j]) -band 0xFFFFFFFFL
    }
    for ($j = 11 * 512; $j -lt 12 * 512; $j += 4) {
        [BitConverter]::GetBytes([uint32]$checksum).CopyTo($bootRegion, $j)
    }
    $stream.Position = [int64]$volumeOffset
    $writer.Write($bootRegion)
    $writer.Write($bootRegion)

    $heapOffset = $volumeOffset + 25 * 512
    $stream.Position = [int64]$heapOffset
    $root = [byte[]]::new(512)
    $root[0] = 0x81
    [BitConverter]::GetBytes([uint32]4).CopyTo($root, 20)
    [BitConverter]::GetBytes([uint64]13).CopyTo($root, 24)
    $root[32] = 0x85; $root[33] = 2
    $modifiedTimestamp = [uint32]((44 -shl 25) -bor (5 -shl 21) -bor (6 -shl 16))
    [BitConverter]::GetBytes($modifiedTimestamp).CopyTo($root, 44)
    $root[64] = 0xC0; $root[65] = 3; $root[67] = 8
    [BitConverter]::GetBytes((Get-ExfatNameHash 'test.txt')).CopyTo($root, 68)
    [BitConverter]::GetBytes([uint64]5).CopyTo($root, 72)
    [BitConverter]::GetBytes([uint32]3).CopyTo($root, 84)
    [BitConverter]::GetBytes([uint64]8).CopyTo($root, 88)
    $root[96] = 0xC1
    [Text.Encoding]::Unicode.GetBytes('test.txt').CopyTo($root, 98)
    [uint64]$setChecksum = 0
    for ($j = 0; $j -lt 96; ++$j) {
        if ($j -eq 2 -or $j -eq 3) { continue }
        $rotated = (($setChecksum -shr 1) -bor (($setChecksum -band 1) -shl 15))
        $setChecksum = ($rotated + $root[32 + $j]) -band 0xFFFFL
    }
    [BitConverter]::GetBytes([uint16]$setChecksum).CopyTo($root, 34)

    $root[128] = 0x85; $root[129] = 2
    [BitConverter]::GetBytes($modifiedTimestamp).CopyTo($root, 140)
    $root[160] = 0xC0; $root[161] = 3; $root[163] = 9
    [BitConverter]::GetBytes((Get-ExfatNameHash 'clone.txt')).CopyTo($root, 164)
    [BitConverter]::GetBytes([uint64]5).CopyTo($root, 168)
    [BitConverter]::GetBytes([uint32]3).CopyTo($root, 180)
    [BitConverter]::GetBytes([uint64]8).CopyTo($root, 184)
    $root[192] = 0xC1
    [Text.Encoding]::Unicode.GetBytes('clone.txt').CopyTo($root, 194)
    [uint64]$cloneChecksum = 0
    for ($j = 0; $j -lt 96; ++$j) {
        if ($j -eq 2 -or $j -eq 3) { continue }
        $rotated = (($cloneChecksum -shr 1) -bor (($cloneChecksum -band 1) -shl 15))
        $cloneChecksum = ($rotated + $root[128 + $j]) -band 0xFFFFL
    }
    [BitConverter]::GetBytes([uint16]$cloneChecksum).CopyTo($root, 130)

    [uint16[]]$upcaseWords = @([uint16]0xFFFF, [uint16]97)
    $upcaseWords += [uint16[]](65..90)
    $upcaseWords += @([uint16]0xFFFF, [uint16]65413)
    $upcaseBytes = [byte[]]::new($upcaseWords.Count * 2)
    for ($word = 0; $word -lt $upcaseWords.Count; ++$word) {
        [BitConverter]::GetBytes($upcaseWords[$word]).CopyTo($upcaseBytes, $word * 2)
    }
    [uint64]$upcaseChecksum = 0
    foreach ($byte in $upcaseBytes) {
        $upcaseChecksum = ((($upcaseChecksum -shr 1) -bor (($upcaseChecksum -band 1) -shl 31)) + $byte) -band 0xFFFFFFFFL
    }
    $root[224] = 0x82
    [BitConverter]::GetBytes([uint32]$upcaseChecksum).CopyTo($root, 228)
    [BitConverter]::GetBytes([uint32]5).CopyTo($root, 244)
    [BitConverter]::GetBytes([uint64]$upcaseBytes.Length).CopyTo($root, 248)
    $writer.Write($root)

    $dataOffset = $heapOffset + 512
    $stream.Position = [int64]$dataOffset
    $writer.Write([Text.Encoding]::ASCII.GetBytes('hello'))
    $stream.Position = [int64]($heapOffset + 2 * 512)
    $writer.Write([byte]0x0F)
    $stream.Position = [int64]($heapOffset + 3 * 512)
    $writer.Write($upcaseBytes)
    $writer.Dispose(); $stream.Dispose()

    $splitOffset = [int64]1049000
    $part1 = Join-Path $testRoot 'part-000.bin'
    $part2 = Join-Path $testRoot 'part-001.bin'
    $sourceStream = [IO.File]::OpenRead($image)
    try {
        $copyBuffer = [byte[]]::new(64KB)
        $partStream = [IO.File]::Create($part1)
        try {
            [int64]$remainingPart = $splitOffset
            while ($remainingPart -gt 0) {
                $amount = [int][Math]::Min($copyBuffer.Length, $remainingPart)
                $read = $sourceStream.Read($copyBuffer, 0, $amount)
                if ($read -le 0) { throw 'unexpected EOF while creating first chunk fixture' }
                $partStream.Write($copyBuffer, 0, $read)
                $remainingPart -= $read
            }
        }
        finally { $partStream.Dispose() }
        $partStream = [IO.File]::Create($part2)
        try {
            [int64]$remainingPart = $sourceStream.Length - $splitOffset
            while ($remainingPart -gt 0) {
                $amount = [int][Math]::Min($copyBuffer.Length, $remainingPart)
                $read = $sourceStream.Read($copyBuffer, 0, $amount)
                if ($read -le 0) { throw 'unexpected EOF while creating second chunk fixture' }
                $partStream.Write($copyBuffer, 0, $read)
                $remainingPart -= $read
            }
        }
        finally { $partStream.Dispose() }
    }
    finally { $sourceStream.Dispose() }
    $part1Index = $part1.Replace('\', '/')
    $part2Index = $part2.Replace('\', '/')
    @(
        '#recovery-chunks 1'
        "total $((Get-Item -LiteralPath $image).Length)"
        "part 0 $splitOffset `"$part1Index`""
        "part $splitOffset $((Get-Item -LiteralPath $part2).Length) `"$part2Index`""
    ) | Set-Content -LiteralPath $chunkIndex -Encoding ascii

    & $Parser $chunkIndex --scan $chunkPlan | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "chunk-index scan exited $LASTEXITCODE" }
    if ((Get-Content -LiteralPath $chunkPlan -Raw) -notmatch 'test\.txt.*,planned-complete') {
        throw 'multi-part chunk index did not reconstruct cross-part source reads'
    }

    & $Parser $image --scan $plan | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "scan exited $LASTEXITCODE" }
    if ((Get-Content -LiteralPath $plan -Raw) -notmatch 'test\.txt.*,8,0,planned-complete.*1061888:5') {
        throw 'scan did not inventory the live file as complete'
    }
    if ((Get-Content -LiteralPath $plan -Raw) -notmatch 'test\.txt.*planned-complete.*,high') {
        throw 'healthy primary metadata was not ranked high confidence'
    }
    if ((Get-Content -LiteralPath $plan -Raw) -notmatch '2024-05-06') {
        throw 'exFAT modified date was not exported'
    }
    & $Parser $image --scan $datePlan --modified-after 2025-01-01 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "date-filter scan exited $LASTEXITCODE" }
    if ((Get-Content -LiteralPath $datePlan -Raw) -notmatch 'test\.txt.*,skipped-filtered') {
        throw 'modified-date filter did not exclude the older file'
    }

    & $Parser $image $plannedOutput --from-plan $plan | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "plan recovery exited $LASTEXITCODE" }
    $plannedBytes = [IO.File]::ReadAllBytes((Join-Path $plannedOutput 'test.txt'))
    if ($plannedBytes.Length -ne 8 -or [Text.Encoding]::ASCII.GetString($plannedBytes, 0, 5) -ne 'hello' -or
        $plannedBytes[5] -ne 0 -or $plannedBytes[6] -ne 0 -or $plannedBytes[7] -ne 0) {
        throw 'selected plan row was not recovered'
    }
    $cloneBytes = [IO.File]::ReadAllBytes((Join-Path $plannedOutput 'clone.txt'))
    if ($cloneBytes.Length -ne 8 -or [Text.Encoding]::ASCII.GetString($cloneBytes, 0, 5) -ne 'hello') {
        throw 'second file sharing a source extent was not reconstructed'
    }
    $reportText = Get-Content -LiteralPath (Join-Path $plannedOutput 'recovery_report.json') -Raw
    if ($reportText -notmatch '"unique_extent_reads": 1' -or
        $reportText -notmatch '"planned_extent_tasks": 2' -or
        $reportText -notmatch '"deduplicated_extent_tasks": 1' -or
        $reportText -notmatch '"source_data_bytes_read": 5') {
        throw 'machine-readable report did not record the extent read plan'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $plannedOutput 'test.txt.recovery.sha256')) -or
        -not (Test-Path -LiteralPath (Join-Path $plannedOutput 'clone.txt.recovery.sha256'))) {
        throw 'completed files did not receive verification sidecars'
    }
    & $Parser $image $plannedOutput --from-plan $plan | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "verified resume exited $LASTEXITCODE" }
    $resumeManifest = Get-Content -LiteralPath (Join-Path $plannedOutput 'manifest.csv') -Raw
    $resumeReport = Get-Content -LiteralPath (Join-Path $plannedOutput 'recovery_report.json') -Raw
    if (($resumeManifest -split 'verified-existing').Count -ne 3 -or
        $resumeReport -notmatch '"unique_extent_reads": 0' -or
        $resumeReport -notmatch '"source_data_bytes_read": 0') {
        throw 'verified resume reread source data or failed to report existing files'
    }
    ((Get-Content -LiteralPath $plan -Raw) -replace '(?m)^1,100,', '0,100,') |
        Set-Content -LiteralPath $emptyPlan -Encoding ascii
    & $Parser $image $emptyOutput --from-plan $emptyPlan | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "empty plan recovery exited $LASTEXITCODE" }
    if (Test-Path -LiteralPath (Join-Path $emptyOutput 'test.txt')) {
        throw 'a plan with every row deselected recovered a file'
    }
    [IO.File]::WriteAllText($blockedParent, 'not a directory')
    & $Parser $image (Join-Path $blockedParent 'child') | Out-Host
    if ($LASTEXITCODE -eq 0) { throw 'uncreatable output directory was not rejected' }

    & $Parser $image $output | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "recovery exited $LASTEXITCODE" }
    $recoveredBytes = [IO.File]::ReadAllBytes((Join-Path $output 'test.txt'))
    if ($recoveredBytes.Length -ne 8 -or [Text.Encoding]::ASCII.GetString($recoveredBytes, 0, 5) -ne 'hello' -or
        $recoveredBytes[5] -ne 0 -or $recoveredBytes[6] -ne 0 -or $recoveredBytes[7] -ne 0) {
        throw 'recovered file content differs'
    }

    $stream = [IO.File]::Open($image, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
    $stream.Position = [int64]($volumeOffset + 120)
    $stream.WriteByte(0xA5)
    $stream.Dispose()
    & $Parser $image --scan $backupPlan | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "backup-boot scan exited $LASTEXITCODE" }
    if ((Get-Content -LiteralPath $backupPlan -Raw) -notmatch 'test\.txt.*,8,0,planned-complete') {
        throw 'validated backup boot region was not used after primary corruption'
    }
    if ((Get-Content -LiteralPath $backupPlan -Raw) -notmatch 'test\.txt.*planned-complete.*,medium') {
        throw 'backup-boot metadata was not conservatively downgraded'
    }

    $imageSize = (Get-Item -LiteralPath $image).Length
    @(
        "#total $imageSize"
        "0 $dataOffset +"
        "$dataOffset 5 ?"
        "$($dataOffset + 5) $($imageSize - $dataOffset - 5) +"
    ) | Set-Content -LiteralPath $map -Encoding ascii
    & $Parser $image --scan $damagedPlan --map $map | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "damaged scan exited $LASTEXITCODE" }
    if ((Get-Content -LiteralPath $damagedPlan -Raw) -notmatch 'test\.txt.*,8,0,planned-unreadable') {
        throw 'map damage was not propagated to the planned file status'
    }

    $stream = [IO.File]::Open($image, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
    $stream.Position = [int64]($heapOffset + 2 * 512)
    $stream.WriteByte(0x05)
    $stream.Dispose()
    & $Parser $image --scan $allocationPlan --map $map | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "allocation-damage scan exited $LASTEXITCODE" }
    if ((Get-Content -LiteralPath $allocationPlan -Raw) -notmatch 'planned-metadata-damaged') {
        throw 'live file referencing a free allocation-bitmap cluster was not rejected'
    }

    $stream = [IO.File]::Open($image, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
    $rootForHash = [byte[]]::new(512)
    $stream.Position = [int64]$heapOffset
    if ($stream.Read($rootForHash, 0, $rootForHash.Length) -ne $rootForHash.Length) {
        throw 'could not reload root directory fixture'
    }
    $rootForHash[68] = $rootForHash[68] -bxor 1
    [uint64]$recomputedSetChecksum = 0
    for ($j = 0; $j -lt 96; ++$j) {
        if ($j -eq 2 -or $j -eq 3) { continue }
        $rotated = (($recomputedSetChecksum -shr 1) -bor (($recomputedSetChecksum -band 1) -shl 15))
        $recomputedSetChecksum = ($rotated + $rootForHash[32 + $j]) -band 0xFFFFL
    }
    [BitConverter]::GetBytes([uint16]$recomputedSetChecksum).CopyTo($rootForHash, 34)
    $stream.Position = [int64]$heapOffset
    $stream.Write($rootForHash, 0, $rootForHash.Length)
    $stream.Position = [int64]($heapOffset + 2 * 512)
    $stream.WriteByte(0x0F)
    $stream.Dispose()
    & $Parser $image --scan $nameHashPlan --map $map | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "name-hash scan exited $LASTEXITCODE" }
    if ((Get-Content -LiteralPath $nameHashPlan -Raw) -notmatch 'test\.txt.*,planned-metadata-damaged') {
        throw 'invalid exFAT NameHash with a valid entry checksum was not rejected'
    }
    if ((Get-Content -LiteralPath $nameHashPlan -Raw) -notmatch 'test\.txt.*planned-metadata-damaged.*,low') {
        throw 'damaged metadata was not ranked low confidence'
    }

    $stream = [IO.File]::Open($image, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
    $stream.Position = [int64]($heapOffset + 2 * 512)
    $stream.WriteByte(0x07)
    $stream.Position = [int64]($heapOffset + 98)
    $stream.WriteByte([byte][char]'X')
    $stream.Dispose()
    & $Parser $image --scan $metadataPlan --map $map | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "metadata-damage scan exited $LASTEXITCODE" }
    if ((Get-Content -LiteralPath $metadataPlan -Raw) -notmatch 'planned-metadata-damaged') {
        throw 'invalid directory-entry checksum was not reported as metadata damage'
    }

    # Lost directory: copy the intact clone.txt entry set into an orphan cluster
    # that nothing points at, then find it by signature and walk it.
    $lostList = Join-Path $testRoot 'lost-dirs.txt'
    $lostPlan = Join-Path $testRoot 'lost-plan.csv'
    $stream = [IO.File]::Open($image, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
    $orphan = [byte[]]::new(96)
    $stream.Position = [int64]($heapOffset + 128)
    if ($stream.Read($orphan, 0, 96) -ne 96) { throw 'could not read clone entry set' }
    $stream.Position = [int64]($heapOffset + (50 - 2) * 512)
    $stream.Write($orphan, 0, 96)
    $stream.Dispose()
    & $Parser $image --find-lost-dirs $lostList --all-clusters --map $map | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "find-lost-dirs exited $LASTEXITCODE" }
    $lostLines = @(Get-Content -LiteralPath $lostList | Where-Object { $_ -notmatch '^#' })
    if ($lostLines.Count -ne 1 -or $lostLines[0] -ne '50') {
        throw "lost-directory search returned '$($lostLines -join ',')' instead of cluster 50"
    }
    & $Parser $image --scan $lostPlan --lost-dirs $lostList --map $map | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "lost-dirs scan exited $LASTEXITCODE" }
    if ((Get-Content -LiteralPath $lostPlan -Raw) -notmatch 'LOST\.DIR/cluster_50/clone\.txt",8,') {
        throw 'file inside the lost directory was not planned'
    }

    # Fake-capacity device: data at/after --real-capacity was never stored.
    # Every file's data sits above 1 MiB, so a 1M limit must classify them all.
    $capacityPlan = Join-Path $testRoot 'capacity-plan.csv'
    $capacityOutput = Join-Path $testRoot 'capacity-recovery'
    & $Parser $image --scan $capacityPlan --real-capacity 1M --map $map | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "real-capacity scan exited $LASTEXITCODE" }
    $capacityRows = Import-Csv -LiteralPath $capacityPlan | Where-Object { $_.selected -eq '1' }
    if (@($capacityRows | Where-Object { $_.status -notmatch 'beyond-device-capacity|metadata-damaged' }).Count -ne 0) {
        throw 'files stored beyond the real capacity were not reported as beyond-device-capacity'
    }
    & $Parser $image $capacityOutput --real-capacity 1M --map $map | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "real-capacity recovery exited $LASTEXITCODE" }
    if (Test-Path -LiteralPath (Join-Path $capacityOutput 'clone.txt')) {
        throw 'a zero-filled file was written for data beyond the real capacity'
    }
    if ((Get-Content -LiteralPath (Join-Path $capacityOutput 'manifest.csv') -Raw) -notmatch 'clone\.txt",8,0,beyond-device-capacity') {
        throw 'recovery manifest does not report beyond-device-capacity'
    }
}
finally {
    if ($null -ne $writer) { try { $writer.Dispose() } catch {} }
    if ($null -ne $stream) { try { $stream.Dispose() } catch {} }
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
        $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if (-not $resolvedTestRoot.StartsWith($resolvedTemp, [StringComparison]::OrdinalIgnoreCase)) {
            throw "refusing to remove test path outside temp: $resolvedTestRoot"
        }
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
