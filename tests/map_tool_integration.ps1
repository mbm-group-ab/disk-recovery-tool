param([Parameter(Mandatory = $true)][string]$MapTool)

$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) "disk_recovery_map_tool_test_$PID"
$map = Join-Path $testRoot 'image.map'
try {
    New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
    @('#total 4096', '0 1024 +', '1024 512 -', '1536 2560 ?') |
        Set-Content -LiteralPath "$map.bak" -Encoding ascii
    @('#total 4096', '0 4096 X') | Set-Content -LiteralPath $map -Encoding ascii

    & $MapTool validate $map 4096 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'validation did not fall back to the valid backup' }
    & $MapTool repair $map 4096 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'map repair failed' }
    if (-not (Test-Path -LiteralPath "$map.invalid")) { throw 'invalid primary was not quarantined' }
    & $MapTool validate $map 4096 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'repaired primary did not validate' }
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        $resolved = [IO.Path]::GetFullPath($testRoot)
        $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if (-not $resolved.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase)) {
            throw "refusing to remove test path outside temp: $resolved"
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
