[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $WindowsDriverSamplesRoot,

    [Parameter(Mandatory = $true)]
    [string] $WdkPackageRoot,

    [Parameter(Mandatory = $true)]
    [string] $OutputPath,

    [Parameter(Mandatory = $true)]
    [string] $IntermediatePath,

    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$ExpectedSamplesCommit = '197ba2156a60e2b76fcd4820bae594223e91a1e9'
$ExpectedWdkVersion = '10.0.26100.6584'
$ExpectedFiles = [ordered]@{
    'Source\Main\adapter.cpp' =
        '6a6385cf0eb44a97ac5f5f4f29b982f1674a79a9ff97dd0ee50e78f852759a15'
    'Source\Main\minwavertstream.cpp' =
        'eb14edf4f23b163ec1c968f6553ff6bd6c45e5dccb6c2045a23528d7cc785714'
    'Source\Main\minwavertstream.h' =
        '69351135ddd73ea25e82bb94e0507ac120213f51da35ceab7fc39d13d65c48ab'
    'Source\Main\common.cpp' =
        'bbf64be7bec8413c68377302a7bcf9d25bf50dfd09435bf794b9c39241172e34'
    'Source\Inc\definitions.h' =
        '4d60233d179b18aef6e9bbd160f54e3ebb7859635f8a0f6bf4f7ce3f00d0c3d2'
    'Source\Utilities\Utilities.vcxproj' =
        '8add0221b9cf9cce6e990008e66f295d6168add7c784699eedc115308140aa63'
    'Source\Main\Main.vcxproj' =
        '43f9d4fbaed73f37de27158c1c1b3ef5f798745868b51a6e5c017e8658f12a1a'
    'Source\Main\SimpleAudioSample.inx' =
        'b17fd758d24302e6e3787ff9879cc25bd0cf1be37553b31299b5927a4906202b'
    'Source\Filters\speakerwavtable.h' =
        'e275671a5256866a1ca6aeaa045eb2e65e764b9f4617cf16a46b65f5b85abdd1'
    'Source\Filters\micarraywavtable.h' =
        '42e96087f9fb7fe3177df49817cdb62f74e7febf73e4c69c536efe128742e70c'
}

$DriverRoot = $PSScriptRoot
$PatchPaths = @(
    Join-Path $DriverRoot 'patches\0001-desklink-virtual-microphone.patch'
    Join-Path $DriverRoot 'patches\0002-feed-format.patch'
    Join-Path $DriverRoot 'patches\0003-strip-sample-utilities.patch'
)
$InfTransform = Join-Path $DriverRoot 'Transform-Inf.ps1'
$BridgeHeader = Join-Path $DriverRoot 'src\DeskLinkPcmBridge.h'
$BridgeSource = Join-Path $DriverRoot 'src\DeskLinkPcmBridge.cpp'
$WindowsDriverSamplesRoot =
    (Resolve-Path -LiteralPath $WindowsDriverSamplesRoot).Path
$WdkPackageRoot = (Resolve-Path -LiteralPath $WdkPackageRoot).Path
$SourceSample = Join-Path $WindowsDriverSamplesRoot 'audio\simpleaudiosample'

foreach ($RequiredPath in @(
        $SourceSample, $BridgeHeader, $BridgeSource, $InfTransform) +
        $PatchPaths) {
    if (-not (Test-Path -LiteralPath $RequiredPath)) {
        throw "Required virtual microphone input is missing: $RequiredPath"
    }
}

$ActualCommit = (& git -C $WindowsDriverSamplesRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $ActualCommit -ne $ExpectedSamplesCommit) {
    throw "windows-driver-samples must be pinned to $ExpectedSamplesCommit"
}
if ((Split-Path -Leaf $WdkPackageRoot) -notlike "*$ExpectedWdkVersion*") {
    throw "The virtual microphone requires pinned WDK $ExpectedWdkVersion"
}

foreach ($Entry in $ExpectedFiles.GetEnumerator()) {
    $SourcePath = Join-Path $SourceSample $Entry.Key
    $ActualHash = (Get-FileHash -Algorithm SHA256 `
        -LiteralPath $SourcePath).Hash.ToLowerInvariant()
    if ($ActualHash -ne $Entry.Value) {
        throw "Pinned SYSVAD-derived source changed: $($Entry.Key)"
    }
}

$PatchHashes = @((@($PatchPaths) + $InfTransform + $BridgeHeader +
        $BridgeSource) | ForEach-Object {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash.ToLowerInvariant()
})
$PatchFingerprint = $PatchHashes -join ':'
$StageId = ($PatchHashes | ForEach-Object { $_.Substring(0, 8) }) -join '-'
$IntermediatePath = [IO.Path]::GetFullPath($IntermediatePath)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$StageRoot = Join-Path $IntermediatePath "source-$StageId"
$StageSample = Join-Path $StageRoot 'audio\simpleaudiosample'
$StageMarker = Join-Path $StageRoot '.desklink-virtual-microphone-stage'

New-Item -ItemType Directory -Path $IntermediatePath, $OutputPath -Force |
    Out-Null
if (-not (Test-Path -LiteralPath $StageRoot)) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $StageSample) -Force |
        Out-Null
    Copy-Item -LiteralPath $SourceSample -Destination $StageSample -Recurse
    # Git otherwise treats an in-worktree staging directory as a subdirectory of
    # the parent checkout and silently ignores patch paths outside that prefix.
    & git -c 'init.templateDir=' init --quiet `
        --initial-branch=desklink-stage $StageRoot
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not isolate the staged driver source before patching.'
    }
    Push-Location $StageRoot
    try {
        $BasePatchArguments = @(
            'apply', '--check', '--no-index', '--recount', '--whitespace=error-all',
            '--exclude=audio/simpleaudiosample/Source/Filters/speakerwavtable.h',
            '--exclude=audio/simpleaudiosample/Source/Main/SimpleAudioSample.inx',
            $PatchPaths[0])
        & git @BasePatchArguments
        if ($LASTEXITCODE -ne 0) {
            throw 'DeskLink virtual microphone patch no longer applies cleanly.'
        }
        $BasePatchArguments = @(
            'apply', '--no-index', '--recount', '--whitespace=error-all',
            '--exclude=audio/simpleaudiosample/Source/Filters/speakerwavtable.h',
            '--exclude=audio/simpleaudiosample/Source/Main/SimpleAudioSample.inx',
            $PatchPaths[0])
        & git @BasePatchArguments
        if ($LASTEXITCODE -ne 0) {
            throw 'DeskLink virtual microphone patch could not be applied.'
        }
        foreach ($PatchPath in $PatchPaths[1..($PatchPaths.Count - 1)]) {
            & git apply --check --no-index --unidiff-zero --whitespace=error-all $PatchPath
            if ($LASTEXITCODE -ne 0) {
                throw "DeskLink patch no longer applies: $PatchPath"
            }
            & git apply --no-index --unidiff-zero --whitespace=error-all $PatchPath
            if ($LASTEXITCODE -ne 0) {
                throw "DeskLink patch could not be applied: $PatchPath"
            }
        }
        & $InfTransform -Path (Join-Path $StageSample `
            'Source\Main\SimpleAudioSample.inx')
    }
    finally {
        Pop-Location
    }
    $MainSource = Join-Path $StageSample 'Source\Main'
    Copy-Item -LiteralPath $BridgeHeader -Destination $MainSource
    Copy-Item -LiteralPath $BridgeSource -Destination $MainSource

    $ProductionSourceFiles = @(
        Join-Path $StageSample 'Source\Main\adapter.cpp'
        Join-Path $StageSample 'Source\Main\common.cpp'
        Join-Path $StageSample 'Source\Main\minwavertstream.h'
        Join-Path $StageSample 'Source\Main\minwavertstream.cpp'
        Join-Path $StageSample 'Source\Utilities\Utilities.vcxproj'
    )
    $ProhibitedSampleMechanisms = @(
        'm_ToneGenerator', 'CSaveData', 'g_DoNotCreateDataFiles',
        'savedata.h', 'ToneGenerator.h', 'savedata.cpp', 'tonegenerator.cpp'
    )
    foreach ($SourceFile in $ProductionSourceFiles) {
        $SourceText = Get-Content -LiteralPath $SourceFile -Raw
        foreach ($Mechanism in $ProhibitedSampleMechanisms) {
            if ($SourceText.Contains($Mechanism)) {
                throw "Sample-only driver mechanism remains in $SourceFile`: $Mechanism"
            }
        }
    }
    Set-Content -LiteralPath $StageMarker -Encoding ascii -NoNewline -Value (
        "$ExpectedSamplesCommit`n$PatchFingerprint")
}

$ExpectedMarker = "$ExpectedSamplesCommit`n$PatchFingerprint"
if (-not (Test-Path -LiteralPath $StageMarker -PathType Leaf) -or
    (Get-Content -LiteralPath $StageMarker -Raw) -ne $ExpectedMarker) {
    throw "Refusing unrecognized intermediate driver tree: $StageRoot"
}

$WdkBuildRoot = Join-Path ${env:ProgramFiles(x86)} `
    'Windows Kits\10\build\10.0.26100.0'
$InfVerifier = Join-Path $WdkBuildRoot 'bin\x64\InfVerif.dll'
$PackageVerifier = Join-Path $WdkBuildRoot `
    'bin\Microsoft.DriverKit.Build.Tasks.PackageVerifier.17.0.dll'
$Inf2Cat = Join-Path ${env:ProgramFiles(x86)} `
    'Windows Kits\10\bin\10.0.26100.0\x86\Inf2Cat.exe'
foreach ($RequiredTool in @($InfVerifier, $PackageVerifier, $Inf2Cat)) {
    if (-not (Test-Path -LiteralPath $RequiredTool -PathType Leaf)) {
        throw "Installed WDK $ExpectedWdkVersion tool is missing: $RequiredTool"
    }
}
foreach ($VersionedTool in @($InfVerifier, $PackageVerifier)) {
    $Version = (Get-Item -LiteralPath $VersionedTool).VersionInfo.ProductVersion
    if ($Version -ne $ExpectedWdkVersion) {
        throw "Installed WDK tool is $Version, expected $ExpectedWdkVersion"
    }
}
$ExpectedWdkToolHashes = [ordered]@{
    $InfVerifier =
        '2237cf9008787d37a0deeccc42c63ea486504b8a766a99d9754d98fbf1294942'
    $PackageVerifier =
        'd27c46bc454c8af6f7e1a91793cd2b74a8d85da81d28045d618debe0c0a7b77d'
    $Inf2Cat =
        'b594728d38b271979367abc8060a971b8e42422738009be126710b1f5dd0fcbc'
}
foreach ($Entry in $ExpectedWdkToolHashes.GetEnumerator()) {
    $ActualHash = (Get-FileHash -Algorithm SHA256 `
        -LiteralPath $Entry.Key).Hash.ToLowerInvariant()
    if ($ActualHash -ne $Entry.Value) {
        throw "Installed WDK tool does not match the pinned release: $($Entry.Key)"
    }
}

$VsWhere = Join-Path ${env:ProgramFiles(x86)} `
    'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $VsWhere -PathType Leaf)) {
    throw 'Visual Studio vswhere.exe was not found.'
}
$VisualStudioRoot = (& $VsWhere -latest -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath).Trim()
$MsBuild = Join-Path $VisualStudioRoot `
    'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path -LiteralPath $MsBuild -PathType Leaf)) {
    throw 'MSBuild.exe was not found.'
}

$CommonArguments = @(
    '/m:2',
    '/nologo',
    '/verbosity:minimal',
    '/t:Build',
    "/p:Configuration=$Configuration",
    '/p:Platform=x64',
    '/p:WindowsTargetPlatformVersion=10.0.26100.0',
    '/p:SignMode=Off',
    '/p:EnableInf2cat=false'
)
$Projects = @(
    Join-Path $StageSample 'Source\Filters\Filters.vcxproj'
    Join-Path $StageSample 'Source\Utilities\Utilities.vcxproj'
    Join-Path $StageSample 'Source\Main\Main.vcxproj'
)
foreach ($Project in $Projects) {
    & $MsBuild $Project @CommonArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Virtual microphone driver build failed with exit code $LASTEXITCODE"
    }
}

$DriverBinaries = @(Get-ChildItem -LiteralPath $StageSample -Recurse -File `
    -Filter 'DeskLinkVirtualMicrophone.sys')
$GeneratedInfs = @(Get-ChildItem -LiteralPath $StageSample -Recurse -File `
    -Filter 'SimpleAudioSample.inf')
if ($DriverBinaries.Count -ne 1 -or $GeneratedInfs.Count -ne 1) {
    throw 'The driver build did not produce exactly one SYS and one INF.'
}

$PackageRoot = Join-Path $OutputPath 'DeskLinkVirtualMicrophone'
New-Item -ItemType Directory -Path $PackageRoot -Force | Out-Null
$PackagedSys = Join-Path $PackageRoot 'DeskLinkVirtualMicrophone.sys'
$PackagedInf = Join-Path $PackageRoot 'DeskLinkVirtualMicrophone.inf'
Copy-Item -LiteralPath $DriverBinaries[0].FullName -Destination $PackagedSys
Copy-Item -LiteralPath $GeneratedInfs[0].FullName -Destination $PackagedInf

& $Inf2Cat "/driver:$PackageRoot" '/os:10_CO_X64,ServerFE_X64' `
    '/uselocaltime'
if ($LASTEXITCODE -ne 0) {
    throw "Inf2Cat failed with exit code $LASTEXITCODE"
}

$Catalog = Join-Path $PackageRoot 'DeskLinkVirtualMicrophone.cat'
if (-not (Test-Path -LiteralPath $Catalog -PathType Leaf)) {
    throw 'Inf2Cat did not produce the expected driver catalog.'
}
$Manifest = [ordered]@{
    schema = 1
    architecture = 'x64'
    format = '48000 Hz mono PCM16'
    maximum_bridge_ms = 60
    target_bridge_ms = 40
    upstream_repository =
        'https://github.com/microsoft/Windows-driver-samples'
    upstream_commit = $ExpectedSamplesCommit
    wdk = $ExpectedWdkVersion
    signature_state = 'unsigned-development-output'
    files = @(
        foreach ($File in @($PackagedInf, $PackagedSys, $Catalog)) {
            [ordered]@{
                name = Split-Path -Leaf $File
                sha256 = (Get-FileHash -Algorithm SHA256 `
                    -LiteralPath $File).Hash.ToLowerInvariant()
            }
        }
    )
}
$Manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (
    Join-Path $PackageRoot 'manifest.json') -Encoding utf8

Write-Host "[VirtualMicrophone:Build] created unsigned package $PackageRoot"
Write-Host '[VirtualMicrophone:Security] Microsoft production signing is required before distribution or installation.'
