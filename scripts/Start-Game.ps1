[CmdletBinding()]
param(
    [switch]$Diagnose,
    [switch]$CheckOnly,
    [string]$ImagePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$gameDirectory = Split-Path $PSScriptRoot -Parent
$worker = Join-Path $PSScriptRoot 'GameLaunch.exe'
$mountedHere = $false
$result = 0

function Get-GameDiscIdentity {
    param([string]$Path)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($stream.Length -ne 426504) {
            return [pscustomobject]@{ Length = $stream.Length; Hash = $null }
        }
        $hasher = [Security.Cryptography.SHA256]::Create()
        try {
            return [pscustomobject]@{
                Length = $stream.Length
                Hash = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '')
            }
        } finally { $hasher.Dispose() }
    } finally { $stream.Dispose() }
}

function Test-GameDiscVolume {
    param([object]$Volume)
    if ($null -eq $Volume -or $Volume.DriveType -ne 5 -or
        $Volume.DeviceID -notmatch '^[A-Z]:$' -or $Volume.VolumeName -ine 'ROCKMANDASH2') {
        return $false
    }
    try {
        $cabinet = [IO.Path]::Combine(($Volume.DeviceID + '\'), 'DATA1.CAB')
        $identity = Get-GameDiscIdentity -Path $cabinet
        if ($identity.Length -ne 426504) { return $false }
        return $identity.Hash -ieq
            'E50A937B846FE88C6A3D4FC35B7FCE794E47A1B314DF0412A31C5B2DF6E9B2F0'
    } catch {
        Write-Warning ("Cannot read candidate game disc $($Volume.DeviceID): " + $_.Exception.Message)
        return $false
    }
}

function Find-MountedGameDisc {
    try {
        $volumes = @(Get-CimInstance -ClassName Win32_LogicalDisk -Filter 'DriveType = 5' -ErrorAction Stop)
    } catch {
        Write-Warning ('Cannot enumerate mounted optical drives: ' + $_.Exception.Message)
        return $null
    }
    foreach ($volume in ($volumes | Sort-Object DeviceID)) {
        if (Test-GameDiscVolume $volume) { return ($volume.DeviceID + '\') }
    }
    return $null
}

try {
    & $worker --enhanced --self-test
    if ($LASTEXITCODE -ne 0) { throw 'The embedded native launcher failed its self-test.' }
    & $worker --enhanced --diagnose
    if ($LASTEXITCODE -ne 0) {
        throw 'Feature preflight failed. Check the runtime logs and install the official x86 Visual C++ runtime if needed.'
    }
    if (-not $Diagnose) {
        $discRoot = $null
        if (-not $ImagePath) { $discRoot = Find-MountedGameDisc }
        if ($discRoot) {
            Write-Host 'Reusing a verified mounted game disc. Its existing mount will be preserved.'
        } else {
            if (-not $ImagePath) { $ImagePath = Join-Path $gameDirectory 'gamez88_d2.windows.iso' }
            $ImagePath = [IO.Path]::GetFullPath($ImagePath)
            if (-not [IO.File]::Exists($ImagePath)) {
                throw 'No verified mounted game disc or selected standard ISO was found. Mount the supported game disc, run Convert-DiscImage.ps1, or supply --iso PATH to the EXE.'
            }
            $stream = [IO.File]::OpenRead($ImagePath)
            try {
                if ($stream.Length -lt 17 * 2048 -or $stream.Length % 2048 -ne 0) {
                    throw 'A standard 2048-byte-sector ISO is required. Use the separate converter for MODE1/2352 images.'
                }
                $stream.Position = 32769
                $descriptor = New-Object byte[] 6
                if ($stream.Read($descriptor, 0, 6) -ne 6 -or
                    [Text.Encoding]::ASCII.GetString($descriptor, 0, 5) -ne 'CD001' -or $descriptor[5] -ne 1) {
                    throw 'This is not a supported standard ISO. The original image was not changed.'
                }
            } finally { $stream.Dispose() }
            $image = Get-DiskImage -ImagePath $ImagePath -ErrorAction Stop
            if (-not $image.Attached) {
                $image = Mount-DiskImage -ImagePath $ImagePath -StorageType ISO -Access ReadOnly -PassThru -ErrorAction Stop
                $mountedHere = $true
            }
            $volumes = @($image | Get-Volume -ErrorAction Stop | Where-Object { $_.DriveLetter })
            if ($volumes.Count -ne 1) { throw 'Expected one readable mounted game-disc volume.' }
            $discRoot = '{0}:\' -f $volumes[0].DriveLetter
            if (-not [IO.File]::Exists((Join-Path $discRoot 'DATA1.CAB'))) {
                throw 'The mounted ISO does not contain the expected game-disc files.'
            }
        }
        Write-Host "Game disc ready: $discRoot"
        if ($CheckOnly) {
            Write-Host 'All three features and disc mounting passed; the game was not started.'
        } else {
            Write-Host 'Starting the original game with subtitles, XInput and high resolution. Keep this window open.'
            $process = Start-Process -FilePath $worker -ArgumentList '--enhanced' -WorkingDirectory $gameDirectory -PassThru -Wait
            $result = $process.ExitCode
        }
    }
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    $result = 2
} finally {
    if ($mountedHere) {
        try { Dismount-DiskImage -ImagePath $ImagePath -ErrorAction Stop | Out-Null }
        catch {
            [Console]::Error.WriteLine('Could not eject the image mounted by this launch; eject it in File Explorer.')
            if ($result -eq 0) { $result = 2 }
        }
    }
}
exit $result