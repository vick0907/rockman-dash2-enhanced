[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$converter = Join-Path (Split-Path $PSScriptRoot -Parent) 'Convert-DiscImage.ps1'
$directory = Join-Path ([System.IO.Path]::GetTempPath()) ('dash2-iso-test-' + [Guid]::NewGuid().ToString('N'))
[void][System.IO.Directory]::CreateDirectory($directory)

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Rejected {
    param([scriptblock]$Action)
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Assert-Condition $rejected 'An invalid conversion unexpectedly succeeded.'
}

try {
    $raw = New-Object byte[] (18 * 2352)
    $expected = New-Object byte[] (18 * 2048)
    for ($sectorIndex = 0; $sectorIndex -lt 18; $sectorIndex++) {
        $offset = $sectorIndex * 2352
        for ($index = 1; $index -le 10; $index++) { $raw[$offset + $index] = 255 }
        $raw[$offset + 15] = 1
        for ($index = 0; $index -lt 2048; $index++) {
            $value = [byte](($sectorIndex + $index) % 251)
            $raw[$offset + 16 + $index] = $value
            $expected[$sectorIndex * 2048 + $index] = $value
        }
    }
    $descriptor = [byte[]]@(1, 67, 68, 48, 48, 49, 1)
    [System.Buffer]::BlockCopy($descriptor, 0, $raw, 16 * 2352 + 16, $descriptor.Length)
    [System.Buffer]::BlockCopy($descriptor, 0, $expected, 16 * 2048, $descriptor.Length)
    $source = Join-Path $directory 'raw [fixture].iso'
    $destination = Join-Path $directory 'converted [fixture].iso'
    [System.IO.File]::WriteAllBytes($source, $raw)
    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $result = & $converter -SourcePath $source -DestinationPath $destination
    Assert-Condition ($result.Status -eq 'Converted') 'The raw fixture was not converted.'
    $actual = [System.IO.File]::ReadAllBytes($destination)
    Assert-Condition ([System.Collections.StructuralComparisons]::StructuralEqualityComparer.Equals($actual, $expected)) 'Sector payloads changed.'
    Assert-Condition ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -eq $sourceHash) 'The source image changed.'
    $result = & $converter -SourcePath $source -DestinationPath $destination
    Assert-Condition ($result.Status -eq 'AlreadyConverted') 'Identical existing output was not reused.'
    $result = & $converter -SourcePath $destination
    Assert-Condition ($result.Status -eq 'AlreadyStandard') 'A standard ISO was not recognized.'
    Assert-Rejected { & $converter -SourcePath $source -DestinationPath $source }
    Assert-Rejected { & $converter -SourcePath $destination -DestinationPath $source }
    $conflict = Join-Path $directory 'conflict.iso'
    [System.IO.File]::WriteAllBytes($conflict, [byte[]]@(1, 2, 3))
    $conflictHash = (Get-FileHash -LiteralPath $conflict -Algorithm SHA256).Hash
    Assert-Rejected { & $converter -SourcePath $source -DestinationPath $conflict }
    Assert-Condition ((Get-FileHash -LiteralPath $conflict -Algorithm SHA256).Hash -eq $conflictHash) 'Existing different output was overwritten.'
    $invalid = Join-Path $directory 'invalid.iso'
    $raw[2352 + 15] = 2
    [System.IO.File]::WriteAllBytes($invalid, $raw)
    Assert-Rejected { & $converter -SourcePath $invalid }
    $raw[2352 + 15] = 1
    $raw[2352 + 1] = 0
    [System.IO.File]::WriteAllBytes($invalid, $raw)
    Assert-Rejected { & $converter -SourcePath $invalid }
    [System.IO.File]::WriteAllBytes($invalid, [byte[]]@($raw[0..($raw.Length - 2)]))
    Assert-Rejected { & $converter -SourcePath $invalid }
    Assert-Condition (@(Get-ChildItem -LiteralPath $directory -Filter '*.tmp').Count -eq 0) 'Temporary output was left behind.'
    Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $directory 'invalid.windows.iso'))) 'Invalid output was published.'
    Write-Host 'Disc conversion: PASS (payloads, source preservation, idempotence, standard ISO, overwrite guards, malformed sectors, cleanup).'
} finally {
    [System.IO.Directory]::Delete($directory, $true)
}