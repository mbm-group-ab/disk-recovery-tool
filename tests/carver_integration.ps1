param([Parameter(Mandatory = $true)][string]$Carver)

$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) "disk_recovery_carver_test_$PID"
$image = Join-Path $testRoot 'source.bin'
$output = Join-Path $testRoot 'carved'
$partialOutput = Join-Path $testRoot 'partial-carved'
$skippedOutput = Join-Path $testRoot 'skipped-carved'
$chunkOutput = Join-Path $testRoot 'chunk-carved'
$rangeOutput = Join-Path $testRoot 'range-carved'
$implicitOutput = Join-Path $testRoot 'implicit-carved'
$map = "$image.map"

function Get-Sha256([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $inputStream = [IO.File]::OpenRead($Path)
        try { return [BitConverter]::ToString($sha.ComputeHash($inputStream)).Replace('-', '') }
        finally { $inputStream.Dispose() }
    }
    finally { $sha.Dispose() }
}

function Get-Be32([uint32]$Value) {
    return ,([byte[]]@(
        [byte](($Value -shr 24) -band 0xFF),
        [byte](($Value -shr 16) -band 0xFF),
        [byte](($Value -shr 8) -band 0xFF),
        [byte]($Value -band 0xFF)
    ))
}

try {
    New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
    $stream = [IO.File]::Open($image, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite)
    $stream.SetLength(18MB)
    $jpegOffset = 16MB - 2
    $stream.Position = $jpegOffset
    $stream.Write([byte[]](0xFF, 0xD8, 0xFF), 0, 3)
    $stream.Write([byte[]]::new(100), 0, 100)
    $stream.Write([byte[]](0xFF, 0xD9), 0, 2)
    $stream.Position = 2MB
    $stream.Write([byte[]](0xFF, 0xD8, 0xFF), 0, 3)
    $stream.Write([byte[]]::new(100), 0, 100)
    $stream.Write([byte[]](0xFF, 0xD9), 0, 2)
    $stream.Position = 1MB
    $stream.Write([byte[]](0x50, 0x4B, 0x03, 0x04), 0, 4)

    $zip = [byte[]]::new(98)
    ([byte[]](0x50,0x4B,0x03,0x04)).CopyTo($zip, 0)
    $zip[4] = 20
    ([byte[]](0x50,0x4B,0x01,0x02)).CopyTo($zip, 30)
    $zip[34] = 20; $zip[36] = 20
    ([byte[]](0x50,0x4B,0x05,0x06)).CopyTo($zip, 76)
    [BitConverter]::GetBytes([uint16]1).CopyTo($zip, 84)
    [BitConverter]::GetBytes([uint16]1).CopyTo($zip, 86)
    [BitConverter]::GetBytes([uint32]46).CopyTo($zip, 88)
    [BitConverter]::GetBytes([uint32]30).CopyTo($zip, 92)
    $stream.Position = 3MB
    $stream.Write($zip, 0, $zip.Length)

    $mp4 = [byte[]]::new(36)
    (Get-Be32 16).CopyTo($mp4, 0)
    [Text.Encoding]::ASCII.GetBytes('ftyp').CopyTo($mp4, 4)
    [Text.Encoding]::ASCII.GetBytes('isom').CopyTo($mp4, 8)
    (Get-Be32 12).CopyTo($mp4, 16)
    [Text.Encoding]::ASCII.GetBytes('mdat').CopyTo($mp4, 20)
    (Get-Be32 8).CopyTo($mp4, 28)
    [Text.Encoding]::ASCII.GetBytes('moov').CopyTo($mp4, 32)
    $stream.Position = 4MB
    $stream.Write($mp4, 0, $mp4.Length)
    $stream.Dispose()

    & $Carver $image $implicitOutput --max-output 1000 | Out-Host
    if ($LASTEXITCODE -eq 0) { throw 'carver accepted an implicit whole-image scan' }

    & $Carver $image $rangeOutput --range "$([int64](3MB)):98" --max-output 1000 | Out-Host
    if ($LASTEXITCODE -ne 0 -or
        @(Get-ChildItem -LiteralPath $rangeOutput -File -Filter '*.zip').Count -ne 1 -or
        @(Get-ChildItem -LiteralPath $rangeOutput -File -Filter '*.jpg').Count -ne 0) {
        throw 'explicit carve range was not enforced'
    }

    & $Carver $image $output --whole-image --max-output 1000 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "carver exited $LASTEXITCODE" }
    $files = @(Get-ChildItem -LiteralPath $output -File -Filter '*.jpg')
    if ($files.Count -ne 1 -or $files[0].Extension -ne '.jpg' -or $files[0].Length -ne 105) {
        throw 'identical JPEGs were not content-deduplicated to one exact file'
    }
    if (@(Get-ChildItem -LiteralPath $output -File -Filter '*.zip').Count -ne 1 -or
        @(Get-ChildItem -LiteralPath $output -File -Filter '*.zip')[0].Length -ne 98 -or
        @(Get-ChildItem -LiteralPath $output -File -Filter '*.mp4').Count -ne 1 -or
        @(Get-ChildItem -LiteralPath $output -File -Filter '*.mp4')[0].Length -ne 36) {
        throw 'ZIP/MP4 structural validators did not carve exact container lengths'
    }
    $firstManifest = Get-Content -LiteralPath (Join-Path $output 'carve_manifest.csv') -Raw
    if ($firstManifest -notmatch "$jpegOffset,105,jpg,duplicate-content" -or
        $firstManifest -notmatch '[0-9a-f]{64}') {
        throw 'boundary-spanning duplicate lacks hash/provenance'
    }

    & $Carver $image $output --whole-image --max-output 1000 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "carver resume exited $LASTEXITCODE" }
    if (@(Get-ChildItem -LiteralPath $output -File -Filter '*.jpg').Count -ne 1) {
        throw 'rerunning carver duplicated an existing deterministic result'
    }
    if ((Get-Content -LiteralPath (Join-Path $output 'carve_manifest.csv') -Raw) -notmatch 'verified-existing') {
        throw 'rerunning carver did not verify the existing result'
    }
    $carvedPath = @(Get-ChildItem -LiteralPath $output -File -Filter '*.jpg')[0].FullName
    $corruptBytes = [IO.File]::ReadAllBytes($carvedPath)
    $corruptBytes[10] = $corruptBytes[10] -bxor 0xFF
    [IO.File]::WriteAllBytes($carvedPath, $corruptBytes)
    $corruptHash = Get-Sha256 $carvedPath
    & $Carver $image $output --whole-image --max-output 1000 | Out-Host
    if ($LASTEXITCODE -eq 0) { throw 'carver trusted a corrupted existing output' }
    if ((Get-Sha256 $carvedPath) -ne $corruptHash) {
        throw 'carver overwrote a corrupted existing output'
    }

    $tail = Join-Path $testRoot 'tail.bin'
    $sourceStream = [IO.File]::OpenRead($image)
    $tailStream = [IO.File]::Create($tail)
    try {
        $sourceStream.Position = 16MB
        $sourceStream.CopyTo($tailStream)
    }
    finally {
        $tailStream.Dispose()
        $sourceStream.Dispose()
    }
    $chunkIndex = Join-Path $testRoot 'source.rci'
    $imageIndexPath = ([IO.Path]::GetFullPath($image) -replace '\\', '/')
    $tailIndexPath = ([IO.Path]::GetFullPath($tail) -replace '\\', '/')
    @(
        '#recovery-chunks 1'
        "total $((Get-Item -LiteralPath $image).Length)"
        "part 0 $([int64](16MB)) `"$imageIndexPath`""
        "part $([int64](16MB)) $((Get-Item -LiteralPath $tail).Length) `"$tailIndexPath`""
    ) | Set-Content -LiteralPath $chunkIndex -Encoding ascii
    & $Carver $chunkIndex $chunkOutput --whole-image --max-output 1000 | Out-Host
    if ($LASTEXITCODE -ne 0 -or
        @(Get-ChildItem -LiteralPath $chunkOutput -File -Filter '*.jpg').Count -ne 1) {
        throw 'carver could not scan a chunk index across a part boundary'
    }

    $imageSize = (Get-Item -LiteralPath $image).Length
    $badOffset1 = 2MB + 50
    $badOffset2 = $jpegOffset + 50
    @(
        "#total $imageSize"
        "0 $badOffset1 +"
        "$badOffset1 1 -"
        "$($badOffset1 + 1) $($badOffset2 - $badOffset1 - 1) +"
        "$badOffset2 1 -"
        "$($badOffset2 + 1) $($imageSize - $badOffset2 - 1) +"
    ) | Set-Content -LiteralPath $map -Encoding ascii
    & $Carver $image $skippedOutput --whole-image --map $map --max-output 1000 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "mapped carver exited $LASTEXITCODE" }
    if (@(Get-ChildItem -LiteralPath $skippedOutput -File -Filter '*.jpg').Count -ne 0 -or
        (Get-Content -LiteralPath (Join-Path $skippedOutput 'carve_manifest.csv') -Raw) -notmatch 'skipped-partial,1') {
        throw 'carver did not skip an artifact intersecting a bad map range'
    }

    & $Carver $image $partialOutput --whole-image --map $map --include-partial --max-output 1000 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "partial carver exited $LASTEXITCODE" }
    if (@(Get-ChildItem -LiteralPath $partialOutput -File -Filter '*.jpg').Count -ne 1 -or
        (Get-Content -LiteralPath (Join-Path $partialOutput 'carve_manifest.csv') -Raw) -notmatch 'partial,1') {
        throw 'explicit partial carving did not record map provenance'
    }
}
finally {
    if ($null -ne $stream) { try { $stream.Dispose() } catch {} }
    if ($null -ne $sourceStream) { try { $sourceStream.Dispose() } catch {} }
    if ($null -ne $tailStream) { try { $tailStream.Dispose() } catch {} }
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
        $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if (-not $resolvedTestRoot.StartsWith($resolvedTemp, [StringComparison]::OrdinalIgnoreCase)) {
            throw "refusing to remove test path outside temp: $resolvedTestRoot"
        }
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
