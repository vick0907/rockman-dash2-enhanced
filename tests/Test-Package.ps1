[CmdletBinding()]
param([switch]$DiscOnly, [switch]$IconOnly, [string]$GameExecutable)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$executable = Join-Path $root 'dist\RockmanDash2-Enhanced.exe'

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

if ($DiscOnly -and $IconOnly) { throw 'Choose DiscOnly or IconOnly, not both.' }
if (-not $DiscOnly) {
    if (-not ('Dash2EnhancedTests.IconResources' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace Dash2EnhancedTests {
    public static class IconResources {
        [DllImport("kernel32.dll", EntryPoint = "LoadLibraryExW", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr Load(string path, IntPtr file, uint flags);
        [DllImport("kernel32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool FreeLibrary(IntPtr module);
        [DllImport("kernel32.dll", EntryPoint = "FindResourceW", SetLastError = true)]
        private static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint SizeofResource(IntPtr module, IntPtr resource);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr LockResource(IntPtr resource);
        public static byte[] Read(IntPtr module, int type, int identifier) {
            IntPtr resource = FindResource(module, new IntPtr(identifier), new IntPtr(type));
            if (resource == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Missing EXE icon resource");
            uint size = SizeofResource(module, resource);
            IntPtr data = LockResource(LoadResource(module, resource));
            if (size == 0 || data == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            byte[] bytes = new byte[checked((int)size)];
            Marshal.Copy(data, bytes, 0, bytes.Length);
            return bytes;
        }
    }
}
'@
    }
    $module = [Dash2EnhancedTests.IconResources]::Load($executable, [IntPtr]::Zero, 2)
    Assert-Condition ($module -ne [IntPtr]::Zero) 'Cannot open the launcher as a read-only resource file.'
    try {
        $group = [Dash2EnhancedTests.IconResources]::Read($module, 14, 1)
        $icon = [IO.File]::ReadAllBytes((Join-Path $root 'assets\RockmanDash2-Enhanced.ico'))
        $sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
        Assert-Condition ([BitConverter]::ToUInt16($icon, 0) -eq 0 -and [BitConverter]::ToUInt16($icon, 2) -eq 1) 'Invalid ICO header.'
        Assert-Condition ([BitConverter]::ToUInt16($icon, 4) -eq $sizes.Count) 'Unexpected ICO frame count.'
        Assert-Condition ($group.Length -eq 6 + 14 * $sizes.Count) 'Unexpected EXE icon group size.'
        Assert-Condition ([Convert]::ToBase64String($group, 0, 6) -ceq [Convert]::ToBase64String($icon, 0, 6)) 'EXE icon group header differs.'
        for ($index = 0; $index -lt $sizes.Count; $index++) {
            $entry = 6 + 16 * $index
            $resourceEntry = 6 + 14 * $index
            $dimension = if ($sizes[$index] -eq 256) { 0 } else { $sizes[$index] }
            Assert-Condition ($icon[$entry] -eq $dimension -and $icon[$entry + 1] -eq $dimension) 'Unexpected ICO dimensions.'
            Assert-Condition ([Convert]::ToBase64String($group, $resourceEntry, 12) -ceq [Convert]::ToBase64String($icon, $entry, 12)) 'EXE icon frame metadata differs.'
            $identifier = [BitConverter]::ToUInt16($group, $resourceEntry + 12)
            $bytes = [Dash2EnhancedTests.IconResources]::Read($module, 3, $identifier)
            $length = [BitConverter]::ToUInt32($icon, $entry + 8)
            $offset = [BitConverter]::ToUInt32($icon, $entry + 12)
            Assert-Condition ($offset + $length -le $icon.Length -and $bytes.Length -eq $length) 'Invalid ICO frame bounds.'
            Assert-Condition ([Convert]::ToBase64String($bytes) -ceq [Convert]::ToBase64String($icon, $offset, $length)) 'Embedded icon pixels differ from the source ICO.'
        }
        Write-Host 'EXE icon resources: PASS (all 10 frames match the source ICO, 16-256 pixels).'
    } finally { [void][Dash2EnhancedTests.IconResources]::FreeLibrary($module) }
}
if ($IconOnly) { return }

& {
    $WarningPreference = 'SilentlyContinue'
    $tokens = $null
    $parseErrors = $null
    $syntax = [Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'scripts\Start-Game.ps1'), [ref]$tokens, [ref]$parseErrors)
    Assert-Condition (@($parseErrors).Count -eq 0) 'The disc launcher script does not parse.'
    $definitions = @($syntax.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -in @('Get-GameDiscIdentity', 'Test-GameDiscVolume', 'Find-MountedGameDisc')
    }, $false))
    Assert-Condition ($definitions.Count -eq 3) 'The mounted-disc helpers were not found.'
    foreach ($definition in $definitions) {
        Set-Item -Path ('Function:\' + $definition.Name) -Value $definition.Body.GetScriptBlock()
    }
    $identityPath = [IO.Path]::GetTempFileName()
    try {
        $bytes = New-Object byte[] 426504
        [IO.File]::WriteAllBytes($identityPath, $bytes)
        $hasher = [Security.Cryptography.SHA256]::Create()
        try { $expectedHash = [BitConverter]::ToString($hasher.ComputeHash($bytes)).Replace('-', '') }
        finally { $hasher.Dispose() }
        $identity = Get-GameDiscIdentity -Path $identityPath
        Assert-Condition ($identity.Length -eq $bytes.Length -and $identity.Hash -ceq $expectedHash) 'Disc stream hashing differs from the source bytes.'
        [IO.File]::WriteAllBytes($identityPath, [byte[]]@(1, 2, 3))
        $identity = Get-GameDiscIdentity -Path $identityPath
        Assert-Condition ($identity.Length -eq 3 -and $null -eq $identity.Hash) 'An unexpected-size disc file was hashed.'
    } finally { [IO.File]::Delete($identityPath) }
    $volume = [pscustomobject]@{ DeviceID = 'T:'; DriveType = 5; VolumeName = 'ROCKMANDASH2' }
    $fixture = [pscustomobject]@{
        Volumes = @($volume)
        CabinetDrive = 'T:'
        Length = 426504
        Hash = 'E50A937B846FE88C6A3D4FC35B7FCE794E47A1B314DF0412A31C5B2DF6E9B2F0'
        Readable = $true
        EnumerationAvailable = $true
    }
    function Get-CimInstance {
        [CmdletBinding()]
        param([string]$ClassName, [string]$Filter)
        if (-not $fixture.EnumerationAvailable) { throw 'Fixture enumeration failure.' }
        Assert-Condition ($ClassName -eq 'Win32_LogicalDisk' -and $Filter -eq 'DriveType = 5') 'Unexpected drive query.'
        return $fixture.Volumes
    }
    function Get-GameDiscIdentity {
        [CmdletBinding()]
        param([string]$Path)
        if (-not $fixture.Readable) { throw 'Fixture disc is not readable.' }
        Assert-Condition ($Path -eq ($fixture.CabinetDrive + '\DATA1.CAB')) 'Unexpected disc file access.'
        return [pscustomobject]@{ Length = $fixture.Length; Hash = $fixture.Hash }
    }
    Assert-Condition ((Find-MountedGameDisc) -ceq 'T:\') 'A verified mounted disc was not recognized without a local ISO.'
    foreach ($identifier in @('D:', 'E:', 'F:', 'Z:')) {
        $volume.DeviceID = $identifier
        $fixture.CabinetDrive = $identifier
        Assert-Condition ((Find-MountedGameDisc) -ceq ($identifier + '\')) "A verified disc on $identifier was not recognized."
    }
    $volume.DeviceID = 'T:'
    $fixture.CabinetDrive = 'T:'
    $fixture.Volumes = @([pscustomobject]@{ DeviceID = 'D:'; DriveType = 5; VolumeName = 'OTHER_DISC' }, $volume)
    Assert-Condition ((Find-MountedGameDisc) -ceq 'T:\') 'An unrelated first optical drive prevented finding the game disc.'
    $fixture.Volumes = @($volume)
    $volume.DriveType = 3
    Assert-Condition ($null -eq (Find-MountedGameDisc)) 'A fixed disk was accepted as a CD-ROM.'
    $volume.DriveType = 5
    $volume.VolumeName = 'OTHER_DISC'
    Assert-Condition ($null -eq (Find-MountedGameDisc)) 'A different disc label was accepted.'
    $volume.VolumeName = 'ROCKMANDASH2'
    $fixture.Hash = '0' * 64
    Assert-Condition ($null -eq (Find-MountedGameDisc)) 'A same-label disc with different content was accepted.'
    $fixture.Hash = 'E50A937B846FE88C6A3D4FC35B7FCE794E47A1B314DF0412A31C5B2DF6E9B2F0'
    $fixture.Length++
    Assert-Condition ($null -eq (Find-MountedGameDisc)) 'An unexpected disc file size was accepted.'
    $fixture.Length--
    $fixture.Readable = $false
    Assert-Condition ($null -eq (Find-MountedGameDisc)) 'An unreadable disc was accepted.'
    $fixture.Readable = $true
    $volume.DeviceID = 'T'
    Assert-Condition ($null -eq (Find-MountedGameDisc)) 'An invalid drive identifier was accepted.'
    $volume.DeviceID = 'T:'
    $fixture.Volumes = @()
    Assert-Condition ($null -eq (Find-MountedGameDisc)) 'An absent disc was reported as mounted.'
    $fixture.EnumerationAvailable = $false
    Assert-Condition ($null -eq (Find-MountedGameDisc)) 'Drive enumeration failure did not allow ISO fallback.'
    Write-Host 'Mounted-disc detection: PASS (D/E/F/T/Z slots, multiple drives, verified content, drive type, label, hash, size, unreadable media, empty drives, enumeration failure).'
}
if ($DiscOnly) { return }

$manifest = Get-Content -LiteralPath (Join-Path $root 'build\payload-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$directory = Join-Path ([IO.Path]::GetTempPath()) ('dash2-package [test] ' + [char]0x6E2C + [char]0x8A66 + '-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($directory)

if (-not ('Dash2EnhancedTests.IniProfile' -as [type])) {
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
namespace Dash2EnhancedTests {
    public static class IniProfile {
        [DllImport("kernel32.dll", EntryPoint = "WritePrivateProfileStringW", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool Write(string section, string key, string value, string filePath);
    }
}
'@
}

try {
    & $executable --self-test
    Assert-Condition ($LASTEXITCODE -eq 0) 'Embedded payload verification failed.'
    & $executable --extract-only --game-directory $directory
    Assert-Condition ($LASTEXITCODE -eq 0) 'Extraction into a Unicode/bracket path failed.'
    $runtime = Join-Path $directory $manifest.runtime_folder
    foreach ($asset in $manifest.files) {
        $path = Join-Path $runtime $asset.path
        Assert-Condition ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ieq $asset.sha256) "Extracted bytes differ: $($asset.path)"
    }
    $configuration = Join-Path $runtime 'xinput\Xidi.ini'
    $originalConfigurationHash = (Get-FileHash -LiteralPath $configuration -Algorithm SHA256).Hash
    Assert-Condition ([Dash2EnhancedTests.IniProfile]::Write('Properties', 'DeadzonePercentStickLeft', '11', $configuration)) 'Could not update a supported Xidi setting.'
    $configurationHash = (Get-FileHash -LiteralPath $configuration -Algorithm SHA256).Hash
    Assert-Condition ($configurationHash -ne $originalConfigurationHash) 'The controller preservation fixture was not changed.'
    & $executable --extract-only --game-directory $directory
    Assert-Condition ($LASTEXITCODE -eq 0) 'Idempotent extraction failed.'
    Assert-Condition ((Get-FileHash -LiteralPath $configuration -Algorithm SHA256).Hash -eq $configurationHash) 'Player controller settings were overwritten.'
    & $executable --diagnose --game-directory $directory
    Assert-Condition ($LASTEXITCODE -eq 0) 'Packaged three-feature diagnostics failed without game files.'
    foreach ($log in (Get-ChildItem -LiteralPath ([Environment]::GetFolderPath('Desktop')) -File -Filter 'Xidi_GameLaunch.exe_*.log')) {
        $content = [IO.File]::ReadAllText($log.FullName)
        if ($content.IndexOf($runtime, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            Assert-Condition (-not $content.Contains('[E]')) "Xidi reported a runtime/configuration error: $($log.FullName)"
        }
    }
    if ($GameExecutable) {
        $mountedBefore = @(Get-CimInstance Win32_LogicalDisk -Filter 'DriveType = 5' |
            Select-Object DeviceID, VolumeName)
        Copy-Item -LiteralPath $GameExecutable -Destination (Join-Path $directory 'dash2.exe')
        Assert-Condition (@(Get-ChildItem -LiteralPath $directory -Filter '*.iso').Count -eq 0) 'The mounted-disc fixture unexpectedly contains an ISO.'
        & $executable --check-only --game-directory $directory
        Assert-Condition ($LASTEXITCODE -eq 0) 'A verified mounted game disc was not reused without a local ISO.'
        & $executable --check-only --game-directory $directory --iso (Join-Path $directory 'missing-explicit.iso')
        Assert-Condition ($LASTEXITCODE -ne 0) 'An explicit missing ISO silently fell back to another mounted disc.'
        $mountedAfter = @(Get-CimInstance Win32_LogicalDisk -Filter 'DriveType = 5' |
            Select-Object DeviceID, VolumeName)
        Assert-Condition (@(Compare-Object $mountedBefore $mountedAfter -Property DeviceID, VolumeName).Count -eq 0) 'An existing disc mount changed.'
        Assert-Condition (@(Get-ChildItem -LiteralPath $directory -Filter '*.iso').Count -eq 0) 'The launcher copied or converted an ISO without being asked.'
        Write-Host 'Mounted-disc integration: PASS (no local ISO, existing mount preserved, explicit ISO respected, no game started).'
    }
    $module = Join-Path $runtime 'GameFeatures.dll'
    [IO.File]::AppendAllText($module, 'tamper', [Text.Encoding]::ASCII)
    $tamperedHash = (Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash
    & $executable --extract-only --game-directory $directory
    Assert-Condition ($LASTEXITCODE -ne 0) 'A tampered module was accepted.'
    Assert-Condition ((Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash -eq $tamperedHash) 'A different existing module was overwritten.'
    [IO.File]::WriteAllBytes((Join-Path $directory 'dash2.exe'), [byte[]]@(77, 90, 0, 0))
    & $executable --check-only --game-directory $directory
    Assert-Condition ($LASTEXITCODE -ne 0) 'An unsupported game executable was accepted.'
    & $executable --self-test --extract-only
    Assert-Condition ($LASTEXITCODE -ne 0) 'Conflicting diagnostic flags were accepted.'
    Write-Host 'Single-file package: PASS (embedded hashes, Unicode paths, extraction, valid editable settings, native diagnostics, no Xidi errors, tamper rejection, version guard).'
} finally {
    [IO.Directory]::Delete($directory, $true)
}