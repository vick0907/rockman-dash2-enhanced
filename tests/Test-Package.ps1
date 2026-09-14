[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$executable = Join-Path $root 'dist\RockmanDash2-Enhanced.exe'
$manifest = Get-Content -LiteralPath (Join-Path $root 'build\payload-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$directory = Join-Path ([IO.Path]::GetTempPath()) ('dash2-package [test] ' + [char]0x6E2C + [char]0x8A66 + '-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($directory)

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

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