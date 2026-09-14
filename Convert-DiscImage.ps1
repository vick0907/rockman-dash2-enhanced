[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath,
    [string]$DestinationPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-Exact {
    param([System.IO.Stream]$Stream, [byte[]]$Buffer)
    $offset = 0
    while ($offset -lt $Buffer.Length) {
        $received = $Stream.Read($Buffer, $offset, $Buffer.Length - $offset)
        if ($received -eq 0) { throw 'Unexpected end of disc image.' }
        $offset += $received
    }
}

function Test-SectorSync {
    param([byte[]]$Buffer)
    if ($Buffer[0] -ne 0 -or $Buffer[11] -ne 0) { return $false }
    for ($index = 1; $index -le 10; $index++) {
        if ($Buffer[$index] -ne 255) { return $false }
    }
    return $true
}

function Test-StandardIso {
    param([System.IO.Stream]$Stream)
    if ($Stream.Length -lt 17 * 2048 -or $Stream.Length % 2048 -ne 0) { return $false }
    [void]$Stream.Seek(32769, [System.IO.SeekOrigin]::Begin)
    $descriptor = New-Object byte[] 6
    Read-Exact $Stream $descriptor
    return [System.Text.Encoding]::ASCII.GetString($descriptor, 0, 5) -eq 'CD001' -and $descriptor[5] -eq 1
}

$source = [System.IO.Path]::GetFullPath($SourcePath)
if (-not [System.IO.File]::Exists($source)) { throw "Source image not found: $source" }
$sourceStream = [System.IO.File]::Open($source, [System.IO.FileMode]::Open,
    [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
$temporaryPath = $null
try {
    if ($sourceStream.Length -lt 16) { throw 'The source is too short to be a disc image.' }
    $header = New-Object byte[] 16
    Read-Exact $sourceStream $header
    if (-not (Test-SectorSync $header)) {
        if (-not (Test-StandardIso $sourceStream)) {
            throw 'Expected a standard ISO or a MODE1/2352 image. Other disc layouts are not supported.'
        }
        if ($DestinationPath -and [System.IO.Path]::GetFullPath($DestinationPath) -ine $source) {
            throw 'The source is already a standard ISO. Omit DestinationPath and use the source directly.'
        }
        [pscustomobject]@{ Path = $source; Status = 'AlreadyStandard'; SHA256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash }
        return
    }
    if ($header[15] -ne 1 -or $sourceStream.Length % 2352 -ne 0) {
        throw 'Expected complete MODE1/2352 sectors. MODE2 and mixed-mode images are not supported.'
    }
    $destination = if ($DestinationPath) {
        [System.IO.Path]::GetFullPath($DestinationPath)
    } else {
        [System.IO.Path]::ChangeExtension($source, 'windows.iso')
    }
    if ($destination -ieq $source) { throw 'The destination must not be the source image.' }
    $destinationDirectory = [System.IO.Path]::GetDirectoryName($destination)
    if (-not [System.IO.Directory]::Exists($destinationDirectory)) {
        throw "Destination directory does not exist: $destinationDirectory"
    }
    $temporaryPath = $destination + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $outputStream = [System.IO.File]::Open($temporaryPath, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        [void]$sourceStream.Seek(0, [System.IO.SeekOrigin]::Begin)
        $sector = New-Object byte[] 2352
        $sectorCount = $sourceStream.Length / 2352
        for ($sectorIndex = 0; $sectorIndex -lt $sectorCount; $sectorIndex++) {
            Read-Exact $sourceStream $sector
            if (-not (Test-SectorSync $sector) -or $sector[15] -ne 1) {
                throw "Sector $sectorIndex is not MODE1/2352. No output image was published."
            }
            $outputStream.Write($sector, 16, 2048)
        }
        $outputStream.Flush()
        if (-not (Test-StandardIso $outputStream)) {
            throw 'The converted data does not contain a supported ISO 9660 volume descriptor.'
        }
    } finally {
        $outputStream.Dispose()
    }
    $convertedHash = (Get-FileHash -LiteralPath $temporaryPath -Algorithm SHA256).Hash
    $status = 'Converted'
    if (Test-Path -LiteralPath $destination) {
        if (-not [System.IO.File]::Exists($destination) -or
            (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $convertedHash) {
            throw "Destination already exists with different content; it was not changed: $destination"
        }
        $status = 'AlreadyConverted'
    } else {
        [System.IO.File]::Move($temporaryPath, $destination)
    }
    [pscustomobject]@{ Path = $destination; Status = $status; SHA256 = $convertedHash }
} finally {
    $sourceStream.Dispose()
    if ($temporaryPath -and [System.IO.File]::Exists($temporaryPath)) {
        [System.IO.File]::Delete($temporaryPath)
    }
}