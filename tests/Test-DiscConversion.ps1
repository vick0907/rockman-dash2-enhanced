[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$converter = Join-Path (Split-Path $PSScriptRoot -Parent) 'Convert-DiscImage.ps1'
$dropLauncher = Join-Path (Split-Path $PSScriptRoot -Parent) 'Convert-DiscImage.cmd'
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

function Invoke-DropLauncher {
    param([string]$Launcher, [string[]]$Images = @())
    $quotedPaths = @(@($Launcher) + $Images | ForEach-Object { '"' + $_ + '"' })
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $env:ComSpec
    $start.Arguments = '/d /v:off /s /c "' + ($quotedPaths -join ' ') + '"'
    $start.WorkingDirectory = $directory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errorOutput = $process.StandardError.ReadToEndAsync()
        $process.StandardInput.WriteLine()
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(30000)) {
            & taskkill.exe /PID $process.Id /T /F | Out-Null
            throw 'The drag-and-drop launcher did not finish within 30 seconds.'
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = $output.GetAwaiter().GetResult()
            ErrorOutput = $errorOutput.GetAwaiter().GetResult()
        }
    } finally { $process.Dispose() }
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
    $specialName = 'drop [fixture] & (copy)! 100% ' + [char]0x6E2C + [char]0x8A66
    $launcherDirectory = Join-Path $directory ($specialName + ' tools')
    [void][System.IO.Directory]::CreateDirectory($launcherDirectory)
    $stagedLauncher = Join-Path $launcherDirectory 'Convert-DiscImage.cmd'
    [System.IO.File]::Copy($dropLauncher, $stagedLauncher)
    [System.IO.File]::Copy($converter, (Join-Path $launcherDirectory 'Convert-DiscImage.ps1'))
    $droppedSource = Join-Path $directory ($specialName + '.iso')
    $droppedDestination = [System.IO.Path]::ChangeExtension($droppedSource, 'windows.iso')
    [System.IO.File]::WriteAllBytes($droppedSource, $raw)
    $droppedHash = (Get-FileHash -LiteralPath $droppedSource -Algorithm SHA256).Hash
    $drop = Invoke-DropLauncher -Launcher $stagedLauncher -Images @($droppedSource)
    Assert-Condition ($drop.ExitCode -eq 0 -and $drop.Output.Contains('Conversion complete.')) "Drag-and-drop conversion failed: $($drop.ErrorOutput)"
    Assert-Condition ([System.Collections.StructuralComparisons]::StructuralEqualityComparer.Equals(
        [System.IO.File]::ReadAllBytes($droppedDestination), $expected)) 'Drag-and-drop output differs from sector payloads.'
    Assert-Condition ((Get-FileHash -LiteralPath $droppedSource -Algorithm SHA256).Hash -eq $droppedHash) 'The dropped source image changed.'
    $drop = Invoke-DropLauncher -Launcher $stagedLauncher -Images @($droppedSource)
    Assert-Condition ($drop.ExitCode -eq 0) 'Dropping an already-converted source failed.'
    $drop = Invoke-DropLauncher -Launcher $stagedLauncher -Images @($droppedDestination)
    Assert-Condition ($drop.ExitCode -eq 0) 'Dropping a standard ISO failed.'
    Assert-Condition (-not [System.IO.File]::Exists([System.IO.Path]::ChangeExtension($droppedDestination, 'windows.iso'))) 'A standard ISO was converted again.'
    [System.IO.File]::WriteAllBytes($droppedDestination, [byte[]]@(1, 2, 3))
    $droppedConflictHash = (Get-FileHash -LiteralPath $droppedDestination -Algorithm SHA256).Hash
    $drop = Invoke-DropLauncher -Launcher $stagedLauncher -Images @($droppedSource)
    Assert-Condition ($drop.ExitCode -ne 0 -and $drop.Output.Contains('Conversion failed.')) 'A conflicting drag-and-drop destination was accepted.'
    Assert-Condition ((Get-FileHash -LiteralPath $droppedDestination -Algorithm SHA256).Hash -eq $droppedConflictHash) 'A conflicting dropped destination was overwritten.'
    $drop = Invoke-DropLauncher -Launcher $stagedLauncher
    Assert-Condition ($drop.ExitCode -eq 2 -and $drop.Output.Contains('Drag one ISO or BIN image')) 'Double-click usage instructions were not displayed.'
    $drop = Invoke-DropLauncher -Launcher $stagedLauncher -Images @($source, $droppedSource)
    Assert-Condition ($drop.ExitCode -eq 2) 'Multiple dropped files were silently accepted.'
    $drop = Invoke-DropLauncher -Launcher $stagedLauncher -Images @((Join-Path $directory 'missing.iso'))
    Assert-Condition ($drop.ExitCode -ne 0) 'A missing dropped file was accepted.'
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
    $drop = Invoke-DropLauncher -Launcher $stagedLauncher -Images @($invalid)
    Assert-Condition ($drop.ExitCode -ne 0) 'A malformed dropped image was accepted.'
    Assert-Condition (@(Get-ChildItem -LiteralPath $directory -Filter '*.tmp').Count -eq 0) 'Temporary output was left behind.'
    Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $directory 'invalid.windows.iso'))) 'Invalid output was published.'
    [System.IO.File]::Delete((Join-Path $launcherDirectory 'Convert-DiscImage.ps1'))
    $drop = Invoke-DropLauncher -Launcher $stagedLauncher -Images @($source)
    Assert-Condition ($drop.ExitCode -ne 0 -and $drop.Output.Contains('was not found next to this file')) 'A missing conversion script was not reported.'
    Write-Host 'Disc conversion: PASS (payloads, source preservation, idempotence, standard ISO, overwrite guards, malformed sectors, cleanup).'
    Write-Host 'Drag-and-drop launcher: PASS (Unicode/special paths, automatic output, repeated/standard images, usage, error propagation, missing script, overwrite guards).'
} finally {
    [System.IO.Directory]::Delete($directory, $true)
}