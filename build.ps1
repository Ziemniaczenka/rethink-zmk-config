<#
.SYNOPSIS
    Build (and optionally flash) ReThink firmware in the same container CI uses.

.DESCRIPTION
    Runs scripts/build.sh inside the image from docker/Dockerfile. The west
    workspace (ZMK, Zephyr, modules, build directories) lives in the Docker
    volume "rethink-zmk-ws"; this repository is bind-mounted at /repo.
    The signed image is written to out\<artifact>.signed.bin.

.EXAMPLE
    .\build.ps1                                   # rethink_bare_tester
    .\build.ps1 -Snippet rethink-usb-log -Pristine
    .\build.ps1 -Flash                            # build, then flash (see flash.ps1)
    .\build.ps1 -Shell                            # interactive shell in the container
#>
[CmdletBinding()]
param(
    [string]$Shield = 'rethink_bare_tester',
    [string]$Board = 'framework_wireless_tp_kb/nrf54lm20a/cpuapp/zmk',
    [string]$Snippet = '',
    [string]$CmakeArgs = '',
    [switch]$Pristine,
    [switch]$Update,
    [switch]$Menuconfig,
    [switch]$Shell,
    [switch]$RebuildImage,
    # Flash after building (hold the pairing button while plugging in USB)
    [switch]$Flash,
    # Serial port of MCUboot recovery; implies -Flash. Auto-detected if omitted.
    [string]$Port = ''
)

$ErrorActionPreference = 'Stop'
$Repo = $PSScriptRoot
$Volume = 'rethink-zmk-ws'

# Tag the image by the hash of its inputs, so edits to docker/ trigger a rebuild
$hashInput = (Get-ChildItem "$Repo\docker" -File | Sort-Object Name | ForEach-Object { Get-Content $_.FullName -Raw }) -join "`n"
$sha = [System.Security.Cryptography.SHA256]::Create()
$tag = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($hashInput))[0..5] | ForEach-Object { $_.ToString('x2') })
$Image = "rethink-zmk-build:$tag"

docker image inspect $Image *> $null
if ($LASTEXITCODE -ne 0 -or $RebuildImage) {
    Write-Host "Building image $Image"
    docker build -t $Image "$Repo\docker"
    if ($LASTEXITCODE -ne 0) { throw 'docker build failed' }
}

$runArgs = @(
    'run', '--rm',
    '-v', "${Volume}:/zmk-ws",
    '-v', "${Repo}:/repo",
    '-e', "SHIELD=$Shield",
    '-e', "BOARD=$Board",
    '-e', "SNIPPET=$Snippet",
    '-e', "CMAKE_ARGS=$CmakeArgs",
    '-w', '/repo'
)

if ($Shell -or $Menuconfig) {
    $runArgs += '-it'
}

if ($Shell) {
    docker @runArgs $Image bash
    exit $LASTEXITCODE
}

$mode = if ($Update) { '--update' } elseif ($Menuconfig) { '--menuconfig' } elseif ($Pristine) { '--pristine' } else { '' }
$cmd = "bash scripts/build.sh $mode".Trim()
docker @runArgs $Image bash -c $cmd
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
if ($Update -or $Menuconfig) { exit 0 }

if ($Flash -or $Port) {
    & "$Repo\flash.ps1" -Port $Port -Shield $Shield -Snippet $Snippet
    exit $LASTEXITCODE
}
