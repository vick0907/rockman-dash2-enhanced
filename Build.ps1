[CmdletBinding()]
param(
    [string]$ZigPath,
    [string]$XidiArchivePath,
    [switch]$ModulesOnly,
    [switch]$PackageOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$buildDirectory = Join-Path $PSScriptRoot 'build'
$runtimeDirectory = Join-Path $buildDirectory 'runtime'
$dependencies = Join-Path $buildDirectory 'dependencies'
$sourceDirectory = Join-Path $PSScriptRoot 'src'
$thirdParty = Join-Path $PSScriptRoot 'third_party'
if ($ModulesOnly -and $PackageOnly) { throw 'Choose ModulesOnly or PackageOnly, not both.' }
foreach ($directory in @($buildDirectory, $runtimeDirectory, $dependencies)) {
    [void][IO.Directory]::CreateDirectory($directory)
}

function Get-VerifiedDownload {
    param([string]$Url, [string]$Path, [string]$SHA256)
    if (-not [IO.File]::Exists($Path)) {
        $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
        try {
            Invoke-WebRequest -Uri $Url -OutFile $temporary -UseBasicParsing
            if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ine $SHA256) {
                throw "Download checksum mismatch: $Url"
            }
            [IO.File]::Move($temporary, $Path)
        } finally {
            if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
        }
    }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $SHA256) {
        throw "Cached dependency checksum mismatch: $Path"
    }
}

function Get-PeMachine {
    param([string]$Path)
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($Path))
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE file: $Path" }
        $reader.BaseStream.Position = 0x3C
        $reader.BaseStream.Position = $reader.ReadUInt32()
        if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE signature: $Path" }
        return $reader.ReadUInt16()
    } finally { $reader.Dispose() }
}

if (-not $ZigPath) {
    $zigArchive = Join-Path $dependencies 'zig-0.14.1.zip'
    Get-VerifiedDownload 'https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip' $zigArchive `
        '554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c'
    $zigDirectory = Join-Path $dependencies 'zig-x86_64-windows-0.14.1'
    if (-not [IO.Directory]::Exists($zigDirectory)) {
        Expand-Archive -LiteralPath $zigArchive -DestinationPath $dependencies
    }
    $ZigPath = Join-Path $zigDirectory 'zig.exe'
}
$compiler = [IO.Path]::GetFullPath($ZigPath)
if (-not [IO.File]::Exists($compiler)) { throw "Compiler not found: $compiler" }
$compilerVersion = & $compiler version
if ($LASTEXITCODE -ne 0 -or $compilerVersion -ne '0.14.1') { throw 'Zig 0.14.1 is required.' }
if ((Get-FileHash -LiteralPath (Join-Path $thirdParty 'nlohmann\json.hpp') -Algorithm SHA256).Hash -ne
    '9BEA4C8066EF4A1C206B2BE5A36302F8926F7FDC6087AF5D20B417D0CF103EA6') {
    throw 'The vendored nlohmann/json 3.11.3 header has changed.'
}

$xidiHash = '41B6D23692D7E8043DEEF032AE5E619EE96AAEF1586F58E4A541D4C15429B8CD'
if (-not $XidiArchivePath) {
    $XidiArchivePath = Join-Path $dependencies 'Xidi-v5.0.0.zip'
    Get-VerifiedDownload 'https://github.com/samuelgr/Xidi/releases/download/v5.0.0/Xidi-v5.0.0.zip' $XidiArchivePath $xidiHash
}
if ((Get-FileHash -LiteralPath $XidiArchivePath -Algorithm SHA256).Hash -ne $xidiHash) {
    throw 'The Xidi archive does not match the pinned official release.'
}
$xidiDirectory = Join-Path $dependencies 'Xidi-v5.0.0'
if (-not [IO.Directory]::Exists($xidiDirectory)) {
    Expand-Archive -LiteralPath $XidiArchivePath -DestinationPath $xidiDirectory
}
$xinputDirectory = Join-Path $runtimeDirectory 'xinput'
[void][IO.Directory]::CreateDirectory($xinputDirectory)
foreach ($name in @('Xidi.32.dll', 'dinput.dll')) {
    $candidates = @(Get-ChildItem -LiteralPath $xidiDirectory -Recurse -File -Filter $name |
        Where-Object { (Get-PeMachine $_.FullName) -eq 0x14C })
    if ($candidates.Count -ne 1) { throw "Expected one official x86 $name." }
    Copy-Item -LiteralPath $candidates[0].FullName -Destination (Join-Path $xinputDirectory $name) -Force
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'assets\Xidi.ini') -Destination (Join-Path $xinputDirectory 'Xidi.ini') -Force
$subtitleDirectory = Join-Path $runtimeDirectory 'subtitles'
[void][IO.Directory]::CreateDirectory($subtitleDirectory)
foreach ($name in @('opening.zh-Hant.json', 'story.zh-Hant.json')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('assets\subtitles\' + $name)) -Destination (Join-Path $subtitleDirectory $name) -Force
}

$previousGlobalCache = $env:ZIG_GLOBAL_CACHE_DIR
$previousLocalCache = $env:ZIG_LOCAL_CACHE_DIR
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
try {
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $buildDirectory 'zig-global-cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $buildDirectory 'zig-local-cache'
    $env:TEMP = Join-Path $buildDirectory 'temp'
    $env:TMP = $env:TEMP
    [void][IO.Directory]::CreateDirectory($env:TEMP)
    $minhook = Join-Path $thirdParty 'MinHook'
    $objects = @()
    $common = @('c++', '-target', 'x86-windows-gnu', '-std=c++20', '-O2', '-static', '-s')
    if (-not $PackageOnly) {
    foreach ($relative in @('src\buffer.c', 'src\hook.c', 'src\trampoline.c', 'src\hde\hde32.c')) {
        $object = Join-Path $buildDirectory ([IO.Path]::GetFileNameWithoutExtension($relative) + '.o')
        & $compiler cc -target x86-windows-gnu -O2 -c (Join-Path $minhook $relative) -o $object
        if ($LASTEXITCODE -ne 0) { throw "MinHook compilation failed: $relative" }
        $objects += $object
    }
    $hookInclude = '-I' + (Join-Path $minhook 'include')
    $shimDirectory = Join-Path $thirdParty 'SafeDiscShim\src'
    & $compiler @common -shared '-DSPDLOG_ACTIVE_LEVEL=0' ('-I' + $sourceDirectory) ('-I' + $shimDirectory) $hookInclude `
        (Join-Path $sourceDirectory 'LocalCompat.cpp') (Join-Path $shimDirectory 'secdrv_ioctl.cpp') @objects -lntdll `
        -o (Join-Path $runtimeDirectory 'LocalCompat.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Compatibility module compilation failed.' }
    & $compiler @common -shared $hookInclude ('-I' + $thirdParty) (Join-Path $sourceDirectory 'SubtitleTest.cpp') `
        @objects -lgdi32 -luser32 -o (Join-Path $runtimeDirectory 'SubtitleTest.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Subtitle module compilation failed.' }
    & $compiler @common -shared $hookInclude (Join-Path $sourceDirectory 'XInputTest.cpp') `
        @objects -ldinput -ldxguid -luser32 -o (Join-Path $runtimeDirectory 'XInputTest.dll')
    if ($LASTEXITCODE -ne 0) { throw 'XInput module compilation failed.' }
    & $compiler @common -shared $hookInclude (Join-Path $sourceDirectory 'ResolutionTest.cpp') `
        @objects -luser32 -lgdi32 -o (Join-Path $runtimeDirectory 'ResolutionTest.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Resolution module compilation failed.' }
    & $compiler @common -shared (Join-Path $sourceDirectory 'GameFeatures.cpp') -o (Join-Path $runtimeDirectory 'GameFeatures.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Feature coordinator compilation failed.' }
    $worker = Join-Path $runtimeDirectory 'GameLaunch.exe'
    & $compiler @common -municode (Join-Path $sourceDirectory 'LocalLaunch.cpp') -o $worker
    if ($LASTEXITCODE -ne 0) { throw 'Internal launcher compilation failed.' }
    }
    $worker = Join-Path $runtimeDirectory 'GameLaunch.exe'
    & $worker --enhanced --self-test
    if ($LASTEXITCODE -ne 0) { throw 'Native loader self-test failed.' }
    & $worker --enhanced --diagnose
    if ($LASTEXITCODE -ne 0) { throw 'Native feature diagnostics failed. See build/runtime/*.log.' }
    Write-Host 'Native feature modules built and diagnosed without the original game.'
    if ($ModulesOnly) { return }

    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'scripts\Start-Game.ps1') -Destination (Join-Path $runtimeDirectory 'Start-Game.ps1') -Force
    $licenseDirectory = Join-Path $runtimeDirectory 'licenses'
    [void][IO.Directory]::CreateDirectory($licenseDirectory)
    $zigDirectory = Split-Path $compiler -Parent
    $licenses = [ordered]@{
        'LICENSE.md' = (Join-Path $PSScriptRoot 'LICENSE.md')
        'THIRD_PARTY_NOTICES.md' = (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.md')
        'MinHook.txt' = (Join-Path $thirdParty 'MinHook\LICENSE.txt')
        'Xidi.txt' = (Join-Path $thirdParty 'Xidi\LICENSE')
        'nlohmann-json.txt' = (Join-Path $thirdParty 'nlohmann\LICENSE.MIT')
        'libcxx.txt' = (Join-Path $zigDirectory 'lib\libcxx\LICENSE.TXT')
        'libcxxabi.txt' = (Join-Path $zigDirectory 'lib\libcxxabi\LICENSE.TXT')
        'mingw.txt' = (Join-Path $zigDirectory 'lib\libc\mingw\COPYING')
    }
    foreach ($name in $licenses.Keys) {
        Copy-Item -LiteralPath $licenses[$name] -Destination (Join-Path $licenseDirectory $name) -Force
    }
    $paths = @('GameLaunch.exe', 'LocalCompat.dll', 'GameFeatures.dll', 'SubtitleTest.dll', 'XInputTest.dll',
        'ResolutionTest.dll', 'xinput\Xidi.32.dll', 'xinput\dinput.dll', 'xinput\Xidi.ini',
        'subtitles\opening.zh-Hant.json', 'subtitles\story.zh-Hant.json', 'Start-Game.ps1')
    $paths += @($licenses.Keys | ForEach-Object { 'licenses\' + $_ })
    $payload = @()
    $identifier = 400
    foreach ($relative in $paths) {
        $payload += [ordered]@{
            id = $identifier++
            path = $relative
            sha256 = (Get-FileHash -LiteralPath (Join-Path $runtimeDirectory $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
            editable = ($relative -eq 'xinput\Xidi.ini')
        }
    }
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = $hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes(($payload | ConvertTo-Json -Depth 4 -Compress)))
        $packageId = [BitConverter]::ToString($digest).Replace('-', '').ToLowerInvariant().Substring(0, 16)
    } finally { $hasher.Dispose() }
    $runtimeFolder = 'dash2-enhanced-' + $packageId
    $gameHash = '48baddc9250dc6b99da7ac15b3ae68b0c088489b7351f79ffb990e3384dd0ebc'
    $header = @('static constexpr const char* packageVersion = "0.1.2";',
        ('static constexpr const wchar_t* runtimeFolder = L"' + $runtimeFolder + '";'),
        ('static constexpr const char* supportedGameSha256 = "' + $gameHash + '";'),
        'static constexpr PayloadAsset payloadAssets[] = {')
    foreach ($asset in $payload) {
        $header += '    {' + $asset.id + ', L"' + $asset.path.Replace('\', '\\') + '", "' + $asset.sha256 + '", ' +
            $asset.editable.ToString().ToLowerInvariant() + '},'
    }
    $header += '};'
    [IO.File]::WriteAllText((Join-Path $buildDirectory 'Payload.generated.h'), ($header -join "`n"), [Text.Encoding]::ASCII)
    $manifestPath = Join-Path $buildDirectory 'application.manifest'
    [IO.File]::WriteAllText($manifestPath,
        '<?xml version="1.0" encoding="UTF-8"?><assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"><trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security></trustInfo></assembly>',
        [Text.Encoding]::ASCII)
    $resources = @(('1 24 "' + $manifestPath.Replace('\', '/') + '"'))
    foreach ($asset in $payload) {
        $resources += [string]$asset.id + ' 10 "' + (Join-Path $runtimeDirectory $asset.path).Replace('\', '/') + '"'
    }
    $resourceFile = Join-Path $buildDirectory 'Payload.rc'
    $resourceObject = Join-Path $buildDirectory 'Payload.res'
    [IO.File]::WriteAllText($resourceFile, ($resources -join "`n"), [Text.UTF8Encoding]::new($false))
    & $compiler rc /c 65001 /fo $resourceObject $resourceFile
    if ($LASTEXITCODE -ne 0) { throw 'Embedded resource compilation failed.' }
    $distribution = Join-Path $PSScriptRoot 'dist'
    [void][IO.Directory]::CreateDirectory($distribution)
    $executable = Join-Path $distribution 'RockmanDash2-Enhanced.exe'
    & $compiler @common -municode ('-I' + $buildDirectory) (Join-Path $sourceDirectory 'PackageLaunch.cpp') `
        $resourceObject -lbcrypt -luser32 -o $executable
    if ($LASTEXITCODE -ne 0) { throw 'Single-file launcher compilation failed.' }
    & $executable --self-test
    if ($LASTEXITCODE -ne 0) { throw 'Embedded payload self-test failed.' }
    $inputFiles = @('Build.ps1', 'LICENSE.md', 'THIRD_PARTY_NOTICES.md')
    foreach ($folder in @('src', 'assets', 'scripts', 'third_party')) {
        $inputFiles += @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot $folder) -File -Recurse |
            ForEach-Object { $_.FullName.Substring($PSScriptRoot.Length + 1).Replace('\', '/') })
    }
    $sourceHashes = @($inputFiles | Sort-Object -Unique | ForEach-Object {
        [ordered]@{ path = $_; sha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $_) -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    [IO.File]::WriteAllText((Join-Path $buildDirectory 'payload-manifest.json'),
        ([ordered]@{
            version = '0.1.2'
            runtime_folder = $runtimeFolder
            game_sha256 = $gameHash
            executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
            inputs = $sourceHashes
            files = $payload
        } |
            ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
    Write-Host "Single-file launcher built: $executable"
} finally {
    $env:ZIG_GLOBAL_CACHE_DIR = $previousGlobalCache
    $env:ZIG_LOCAL_CACHE_DIR = $previousLocalCache
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
}