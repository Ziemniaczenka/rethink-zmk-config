<#
.SYNOPSIS
    Flash a signed image from out\ over MCUboot USB serial recovery.

.DESCRIPTION
    Hold the pairing button while plugging in USB; the bootloader shows up as
    a USB serial port (green LED). Needs mcumgr on PATH:
        go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest

    Framework's MCUboot waits for a CDC ACM line-state request (DTR/RTS)
    before it reads from the port, and mcumgr does not assert DTR on Windows,
    so the port is opened once with DTR/RTS set before uploading.

.EXAMPLE
    .\flash.ps1                                         # usage and the images in out\
    .\flash.ps1 -Shield rethink_bare_tester             # out\rethink_bare_tester.signed.bin, port auto-detected
    .\flash.ps1 -Shield rethink_bare_tester -Snippet rethink-usb-log
    .\flash.ps1 -File out\something.signed.bin
    .\flash.ps1 -Shield rethink_bare_tester -Port COM7  # explicit port
#>
[CmdletBinding()]
param(
    [string]$Port = '',
    [string]$Shield = '',
    [string]$Snippet = '',
    [string]$File = ''
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    Write-Host $Message -ForegroundColor Red
    exit 1
}

# Never pick an image implicitly
if (-not $File -and -not $Shield) {
    Write-Host @'
Usage:
  .\flash.ps1 -Shield <shield> [-Snippet <snippet>] [-Port COMx]
  .\flash.ps1 -File <image.signed.bin> [-Port COMx]

Hold the pairing button while plugging in USB first. The port is auto-detected.
'@
    Write-Host 'Images in out\:'
    $images = @(Get-ChildItem (Join-Path $PSScriptRoot 'out') -Filter '*.signed.bin' -ErrorAction SilentlyContinue)
    if ($images.Count -eq 0) {
        Write-Host '  (none, run .\build.ps1 first)'
    }
    $images | ForEach-Object { Write-Host "  $($_.Name)  ($($_.LastWriteTime.ToString('yyyy-MM-dd HH:mm')))" }
    exit 1
}

if (-not $File) {
    $artifact = if ($Snippet) { "$Shield-$Snippet" } else { $Shield }
    $File = Join-Path $PSScriptRoot "out\$artifact.signed.bin"
}
if (-not (Test-Path $File)) { Fail "Image not found: $File (run .\flash.ps1 to list images)" }

if (-not (Get-Command mcumgr -ErrorAction SilentlyContinue)) {
    Fail 'mcumgr not found on PATH. Install it with: go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest'
}

# Framework's MCUboot in serial recovery
$RecoveryId = 'USB\VID_32AC&PID_0034'

if (-not $Port) {
    $ports = @(Get-CimInstance Win32_PnPEntity -Filter "Name LIKE '%(COM%'")
    $recovery = @($ports | Where-Object { $_.PNPDeviceID -like "$RecoveryId*" })

    if ($recovery.Count -eq 1) {
        $Port = [regex]::Match($recovery[0].Name, '\((COM\d+)\)').Groups[1].Value
        Write-Host "Found MCUboot recovery port: $Port"
    } else {
        $reason = if ($recovery.Count -eq 0) { 'No board in recovery mode found' } else { 'More than one board in recovery mode' }
        Write-Host "${reason}. Serial ports:"
        $ports | ForEach-Object { Write-Host "  $($_.Name)  [$($_.PNPDeviceID)]" }
        Fail 'Hold the pairing button while plugging in USB, or pass -Port COMx.'
    }
}

# Wake MCUboot's recovery loop: it starts reading after a line-state change
try {
    $sp = New-Object System.IO.Ports.SerialPort $Port, 115200
    $sp.DtrEnable = $true
    $sp.RtsEnable = $true
    $sp.Open()
    Start-Sleep -Milliseconds 200
    $sp.Close()
    $sp.Dispose()
} catch {
    Fail "Cannot open ${Port}: $($_.Exception.Message)"
}

$conn = "dev=$Port,baud=115200"
Write-Host "Uploading $File to $Port"
& mcumgr --conntype serial --connstring $conn image upload $File
if ($LASTEXITCODE -ne 0) { Fail 'mcumgr upload failed' }
& mcumgr --conntype serial --connstring $conn reset
if ($LASTEXITCODE -ne 0) { Fail 'mcumgr reset failed' }
