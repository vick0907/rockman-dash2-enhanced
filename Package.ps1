[CmdletBinding()]
param([switch]$CheckOnly)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build'
$distribution = Join-Path $PSScriptRoot 'dist'
$manifestPath = Join-Path $build 'payload-manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$executable = Join-Path $distribution 'RockmanDash2-Enhanced.exe'
if ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash -ine $manifest.executable_sha256) {
    throw 'The EXE differs from the last recorded build. Rebuild before packaging.'
}
foreach ($inputFile in $manifest.inputs) {
    if ([IO.Path]::IsPathRooted($inputFile.path) -or @($inputFile.path.Split('/')) -contains '..') {
        throw 'Invalid source path in build manifest.'
    }
    if ((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $inputFile.path) -Algorithm SHA256).Hash -ine $inputFile.sha256) {
        throw "Build input changed: $($inputFile.path). Rebuild before packaging."
    }
}
foreach ($asset in $manifest.files) {
    $assetPath = Join-Path (Join-Path $build 'runtime') $asset.path
    if ((Get-FileHash -LiteralPath $assetPath -Algorithm SHA256).Hash -ine $asset.sha256) {
        throw "Staged runtime differs from the EXE: $($asset.path). Rebuild before packaging."
    }
}
& $executable --self-test
if ($LASTEXITCODE -ne 0) { throw 'The packaged EXE failed its self-test.' }
if ($CheckOnly) {
    Write-Host 'Build inputs, staged assets, and EXE checksums: PASS. No archives created.'
    return
}

$status = @(& git -C $PSScriptRoot status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0 -or $status.Count -ne 0) { throw 'Commit the matching sources in a clean Git worktree before packaging.' }
$commit = & git -C $PSScriptRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'A source commit is required.' }
$tracked = @(& git -C $PSScriptRoot ls-files)
if ($LASTEXITCODE -ne 0 -or $tracked.Count -eq 0) { throw 'Cannot enumerate corresponding sources.' }
$rootFiles = @('.gitignore', '.gitattributes', 'Build.ps1', 'Package.ps1', 'Convert-DiscImage.ps1',
    'README.md', 'LICENSE.md', 'THIRD_PARTY_NOTICES.md')
$extensions = @('.cpp', '.c', '.h', '.hpp', '.ps1', '.json', '.ini', '.txt', '.md', '.mit')
$documentationImages = @('docs/screenshots/subtitles-opening.png', 'docs/screenshots/subtitles-1080p.png',
    'docs/screenshots/xinput-buttons.png', 'docs/screenshots/resolution-1080p.png')
foreach ($relative in $tracked) {
    if ($relative -in $documentationImages) { continue }
    $allowedRoot = $relative -in $rootFiles
    $allowedDirectory = $relative -match '^(src|scripts|tests|third_party|assets)/'
    $extension = [IO.Path]::GetExtension($relative).ToLowerInvariant()
    $licenseFile = [IO.Path]::GetFileName($relative) -eq 'LICENSE'
    if (-not $allowedRoot -and (-not $allowedDirectory -or (-not $licenseFile -and $extension -notin $extensions))) {
        throw "Unexpected tracked distribution file: $relative"
    }
    $text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot $relative))
    if ($text -match '(?i)[A-Z]:[\\/]+Users[\\/]+|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|\bgh[pousr]_[A-Za-z0-9]{30,}\b|\bgithub_pat_[A-Za-z0-9_]{30,}\b') {
        throw "Potential local path or credential in tracked file: $relative"
    }
}

$version = [string]$manifest.version
if ($version -notmatch '^\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$') { throw 'Invalid package version.' }
$playerArchive = Join-Path $distribution "rockman-dash2-enhanced-$version-windows.zip"
$sourceArchive = Join-Path $distribution "rockman-dash2-enhanced-$version-source.zip"
$checksumPath = Join-Path $distribution "rockman-dash2-enhanced-$version-SHA256SUMS.txt"
foreach ($path in @($playerArchive, $sourceArchive, $checksumPath)) {
    if (Test-Path -LiteralPath $path) { throw "Refusing to replace an existing release file: $path" }
}
$stage = Join-Path $build ('player-package-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($stage)
try {
    Copy-Item -LiteralPath $executable -Destination (Join-Path $stage 'RockmanDash2-Enhanced.exe')
    foreach ($name in @('Convert-DiscImage.ps1', 'README.md', 'LICENSE.md', 'THIRD_PARTY_NOTICES.md')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $stage $name)
    }
    foreach ($relative in $documentationImages) {
        $destination = Join-Path $stage $relative
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $relative) -Destination $destination
    }
    Copy-Item -LiteralPath (Join-Path $build 'runtime\licenses') -Destination (Join-Path $stage 'licenses') -Recurse
    [IO.File]::WriteAllText((Join-Path $stage 'build-info.json'),
        ([ordered]@{ version = $version; commit = $commit; executable_sha256 = $manifest.executable_sha256; runtime_folder = $manifest.runtime_folder } |
            ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $playerArchive)
    & git -C $PSScriptRoot archive --format=zip ('--output=' + $sourceArchive) HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the corresponding-source archive.' }
    $checksums = @($playerArchive, $sourceArchive, $executable, (Join-Path $PSScriptRoot 'Convert-DiscImage.ps1')) | ForEach-Object {
        (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($_)
    }
    [IO.File]::WriteAllText($checksumPath, (($checksums -join "`n") + "`n"), [Text.Encoding]::ASCII)
    Write-Host "Verified $($tracked.Count) tracked source files; packaged source commit $commit."
    Write-Host "Player archive: $playerArchive"
    Write-Host "Corresponding source: $sourceArchive"
    Write-Host "Checksums: $checksumPath"
} finally {
    [IO.Directory]::Delete($stage, $true)
}