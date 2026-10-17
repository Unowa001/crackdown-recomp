[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,39}$')][string]$Version,
    [switch]$SkipBuild,
    # Visual Studio / Build Tools install whose vcvars64 builds the SDK. Its precompiled header
    # must come from the same MSVC version, so pass the install that built sdk/out/build/win-amd64
    # when vswhere's newest install differs (docs/launcher.md).
    [string]$VisualStudio
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$sdk = Join-Path $repo 'sdk'
$output = Join-Path $repo 'out/packages'
$packageName = "CrackdownRecomp-beta$Version"
$stage = Join-Path $output $packageName
$zip = Join-Path $output "$packageName.zip"

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE)" }
}
function Git-Text([string]$Directory, [string[]]$Arguments) {
    $result = & git -c core.excludesFile= -C $Directory @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'Git inspection failed' }
    return ($result -join "`n")
}

# SDK tracked changes are allowed only when they exactly equal the checked-in patch stack.
# A separate index verifies this without touching the real SDK index or worktree.
function Assert-ReleaseTree {
    $dirty = Git-Text $repo @('status','--porcelain','--ignore-submodules=dirty')
    if ($dirty) { throw "Refusing beta package from a dirty repository:`n$dirty" }
    $expected = Git-Text $repo @('rev-parse','HEAD:sdk')
    $actual = Git-Text $sdk @('rev-parse','HEAD')
    if ($expected -ne $actual) { throw 'SDK checkout differs from the pinned submodule commit' }
    Invoke-Checked git @('-c','core.excludesFile=','-C',$sdk,'diff','--cached','--exit-code')
    $scratch = Join-Path $output ("sdk-index-" + [guid]::NewGuid().ToString('N'))
    $oldIndex = $env:GIT_INDEX_FILE
    try {
        $env:GIT_INDEX_FILE = $scratch
        Invoke-Checked git @('-c','core.excludesFile=','-C',$sdk,'read-tree','HEAD')
        foreach ($patch in Get-ChildItem (Join-Path $repo 'patches/sdk') -Filter '*.patch' | Sort-Object Name) {
            Invoke-Checked git @('-c','core.excludesFile=','-C',$sdk,'apply','--unidiff-zero','--cached',$patch.FullName)
            Invoke-Checked git @('-c','core.excludesFile=','-C',$sdk,'apply','--unidiff-zero','--reverse','--check',$patch.FullName)
        }
        Invoke-Checked git @('-c','core.excludesFile=','-C',$sdk,'diff','--exit-code','--ignore-submodules=dirty')
        $submodules = Git-Text $sdk @('submodule','status','--recursive')
        if ($submodules -match '(?m)^[+U-]') { throw 'An SDK dependency differs from its pinned commit' }
        Invoke-Checked git @('-c','core.excludesFile=','-C',$sdk,'submodule','foreach','--recursive','git -c core.excludesFile= diff --quiet && git -c core.excludesFile= diff --cached --quiet')
    } finally {
        $env:GIT_INDEX_FILE = $oldIndex
        Remove-Item -LiteralPath $scratch -Force -ErrorAction SilentlyContinue
    }
}

New-Item -ItemType Directory -Path $output -Force | Out-Null
Assert-ReleaseTree
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zip)) { throw 'This package version already exists; choose a new version' }

if (-not $SkipBuild) {
    # Capture vcvars once, then use its x64 environment for both SDK and game.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = if ($VisualStudio) { $VisualStudio } else { & $vswhere -latest -products * -property installationPath }
    if (-not $vs -or -not (Test-Path -LiteralPath (Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'))) { throw 'Visual Studio C++ Build Tools not found; pass -VisualStudio <install path>' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $vars = & $env:ComSpec /d /s /c "`"`"$vcvars`" >nul && set`""
    if ($LASTEXITCODE -ne 0) { throw 'vcvars64 failed' }
    foreach ($line in $vars) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') }
    }
    $env:PATH = "$env:ProgramFiles\LLVM\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:PATH"
    Push-Location $sdk
    try {
        Invoke-Checked cmake @('--preset','win-amd64')
        Invoke-Checked cmake @('--build','out/build/win-amd64','--config','Release','--target','install')
    } finally { Pop-Location }
    Push-Location (Join-Path $repo 'crackdown')
    try {
        Invoke-Checked cmake @('--preset','win-amd64-release',"-DCMAKE_PREFIX_PATH=$sdk/out/install/win-amd64")
        Invoke-Checked cmake @('--build','--preset','win-amd64-release')
    } finally { Pop-Location }
    $oldLauncherVS = $env:LAUNCHER_VS
    try {
        $env:LAUNCHER_VS = $vs
        Invoke-Checked (Join-Path $repo 'tools/build-launcher.bat') @()
    } finally { $env:LAUNCHER_VS = $oldLauncherVS }
}

$gameBuild = Join-Path $repo 'crackdown/out/build/win-amd64-release'
$launcherBuild = Join-Path $repo 'crackdown/launcher/out'
$bin = Join-Path $sdk 'out/install/win-amd64/bin'
$llvm = Join-Path $env:ProgramFiles 'LLVM/bin/llvm-readobj.exe'
if (-not (Test-Path -LiteralPath $llvm)) { throw 'llvm-readobj is required to inspect binary dependencies' }
$cache = Get-Content (Join-Path $gameBuild 'CMakeCache.txt') -Raw
if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$') { throw 'Game build is not Release' }
$sdkCache = Get-Content (Join-Path $sdk 'out/build/win-amd64/CMakeCache.txt') -Raw
if ($sdkCache -match '(?m)^REXGLUE_ENABLE_FIDELITYFX:BOOL=ON\r?$') { throw 'FidelityFX packaging requires a matching launcher build and a separate runtime dependency audit' }

New-Item -ItemType Directory -Path $stage | Out-Null
# Fixed allowlist: never copy a build directory, game dump, generated sources or config.
foreach ($file in @(
    (Join-Path $launcherBuild 'CrackdownLauncher.exe'),
    (Join-Path $gameBuild 'crackdown.exe'),
    (Join-Path $bin 'rexruntime.dll'),
    (Join-Path $bin 'rexgpu-xenos.dll'),
    (Join-Path $repo 'tools/game-manifest.txt'),
    (Join-Path $repo 'tools/beta-README.txt')
)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing package input: $file" }
    $name = Split-Path $file -Leaf
    if ($name -eq 'beta-README.txt') { $name = 'README.txt' }
    Copy-Item -LiteralPath $file -Destination (Join-Path $stage $name)
}

# Reviewed asset manifest is the fixed allowlist; never recursively copy assets or art caches.
# Native PC Settings loads these audited recipes beside the game executable.
New-Item -ItemType Directory -Path (Join-Path $stage 'xui') | Out-Null
foreach ($patchName in @('Scene.xpatch', 'menuHUD_3.xpatch')) {
    Copy-Item -LiteralPath (Join-Path $repo "patches/xui/$patchName") -Destination (Join-Path $stage "xui/$patchName")
}
$assetRoot = Join-Path $repo 'crackdown/launcher/assets'
foreach ($relative in Get-Content (Join-Path $assetRoot 'files.txt')) {
    if ($relative -notmatch '^(fonts/[A-Za-z0-9.-]+\.(ttf|txt)|glyphs/(LICENSE\.txt|(ps5|xbox_series|keyboard)/[A-Za-z0-9_-]+\.png))$') { throw "Invalid asset allowlist entry: $relative" }
    $target = Join-Path $stage "assets/$relative"
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $assetRoot $relative) -Destination $target
}

$licenses = Join-Path $stage 'LICENSES'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'crackdown/launcher/licenses/stb.txt') -Destination (Join-Path $licenses 'stb.txt')
Copy-Item -LiteralPath (Join-Path $assetRoot 'fonts/OFL.txt') -Destination (Join-Path $licenses 'Barlow-OFL.txt')
Copy-Item -LiteralPath (Join-Path $assetRoot 'glyphs/LICENSE.txt') -Destination (Join-Path $licenses 'Controller-prompts-CC0.txt')
Copy-Item -LiteralPath (Join-Path $sdk 'LICENSE') -Destination (Join-Path $licenses 'ReXGlue-and-Xenia.txt')
Copy-Item -LiteralPath (Join-Path $repo 'tools/launcher-NOTICES.txt') -Destination $licenses
foreach ($component in Get-ChildItem (Join-Path $sdk 'thirdparty') -Directory) {
    foreach ($notice in Get-ChildItem $component.FullName -File | Where-Object { $_.Name -match '^(LICENSE|COPYING|COPYRIGHT|UNLICENSE)([.-].*)?$' }) {
        Copy-Item -LiteralPath $notice.FullName -Destination (Join-Path $licenses "$($component.Name)-$($notice.Name).txt")
    }
}
foreach ($relative in @('libmspack/libmspack/COPYING.LIB','disruptorplus/LICENSE.txt','tiny-aes-c/unlicense.txt','stb/LICENSE')) {
    $notice = Join-Path $sdk "thirdparty/$relative"
    if (Test-Path -LiteralPath $notice) { Copy-Item -LiteralPath $notice -Destination (Join-Path $licenses (($relative -replace '/','-') + '.txt')) }
}
$sdkVersion = Git-Text $sdk @('describe','--tags','--always')
$repoVersion = Git-Text $repo @('describe','--tags','--always')
$patchList = Get-ChildItem (Join-Path $repo 'patches/sdk') -Filter '*.patch' | Sort-Object Name | ForEach-Object { "$($_.Name) SHA256=$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)" }
# VERSION.txt stays one line: third-party launchers (e.g. "GitHub Launcher") show its whole
# text as the installed version (issue #3). Build details go at the end of README.txt, a file
# the 0.4.0 updater already accepts (a new top-level file would fail its allowlist).
"Beta $Version" | Set-Content (Join-Path $stage 'VERSION.txt') -Encoding UTF8
@('', 'Build details', "Repo: $repoVersion", "SDK: $sdkVersion", 'Applied SDK patches:', $patchList) | Add-Content (Join-Path $stage 'README.txt') -Encoding UTF8

# Scan every executable and DLL, then fail on any non-system import not in the package.
# Windows CRT is a prerequisite (documented in README); Tracy is added only if imported.
$system = '^(?:(?:api-ms-win-|ext-ms-win-)[A-Za-z0-9_-]+|MSVCP140(?:_[A-Z0-9_]+)?|VCRUNTIME140(?:_[0-9]+)?|ucrtbase|KERNEL32|USER32|ole32|OLEAUT32|SHELL32|WS2_32|dxgi|d3d12|bcrypt|ADVAPI32|IMM32|WINMM|SETUPAPI|GDI32|VERSION|HID|SHLWAPI|COMDLG32|CFGMGR32|DWMAPI|NTDLL|SECUR32|CRYPT32|PROPSYS|WINHTTP|XINPUT1_4|DINPUT8|DSOUND|AVRT|MF|MFPlat|MFReadWrite|dcomp|PowrProf|DEVOBJ|MSIMG32|RPCRT4|NORMALIZ|USP10|WINTRUST|PSAPI)\.dll$'
$seen = @{}
do {
    $added = $false
    foreach ($binary in Get-ChildItem $stage -File | Where-Object Extension -In '.exe','.dll') {
        if ($seen.ContainsKey($binary.Name)) { continue }
        $imports = & $llvm --coff-imports $binary.FullName
        if ($LASTEXITCODE -ne 0) { throw 'Binary dependency inspection failed' }
        foreach ($line in $imports) {
            if ($line -notmatch '^\s+Name: (.+\.dll)\s*$') { continue }
            $dependency = $Matches[1]
            if (Test-Path -LiteralPath (Join-Path $stage $dependency)) { continue }
            if ($dependency -eq 'TracyClient.dll') {
                Copy-Item -LiteralPath (Join-Path $bin $dependency) -Destination $stage
                $added = $true
            } elseif ($dependency -notmatch $system) { throw "Unpackaged dependency $dependency in $($binary.Name)" }
        }
        $seen[$binary.Name] = $true
    }
} while ($added)

Assert-ReleaseTree
foreach ($file in Get-ChildItem $stage -Recurse -File) {
    $relative = $file.FullName.Substring($stage.Length + 1)
    if ($relative -match '(^|[\\/])(game|generated)([\\/]|$)|\.xex$|\.iso$|_recomp\..*\.cpp$') { throw "Forbidden package input: $relative" }
}
# Not Compress-Archive: under Windows PowerShell 5.1 it writes '\' separators, which the
# launcher's updater rejects as unsafe ZIP paths. Entry names here always use '/'.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem $stage -Recurse -File | Sort-Object FullName) {
        $entry = $packageName + '/' + $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $archive.Dispose() }
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    if (@($archive.Entries | Where-Object { $_.FullName.Contains('\') }).Count) { throw 'ZIP entry names must use forward slashes' }
} finally { $archive.Dispose() }
# Companion digest is required by the launcher's update flow. Upload both assets together.
$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$zipHash  $packageName.zip" | Set-Content -LiteralPath "$zip.sha256" -Encoding ASCII
Write-Output "Created local beta package: $zip"
Write-Output 'Public upload remains a separate owner decision; this script does not publish.'
