#requires -Version 5.1
<#
.SYNOPSIS
  Collects reproducible, non-elevated client-OS startup evidence for LockPeek.
.DESCRIPTION
  Run separately in clean Windows 10 x64 and Windows 11 x64 desktop VMs.
  This is only an automated preflight: manual GUI/UAC/waiting QA remains required.
  Does not claim that the absence of VC++ Redistributable can be proved from registry data.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\qa-client.ps1 -Exe .\LockPeek.exe -ExpectedSha256 HEX_DIGEST
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$Exe,
  [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedSha256,
  [string]$ReportDirectory = '.qa-client'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path -LiteralPath $Exe).Path
$actualHash = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash.ToUpperInvariant()
if ($actualHash -ne $ExpectedSha256.ToUpperInvariant()) {
  throw "Binary SHA-256 mismatch: expected $ExpectedSha256, got $actualHash"
}
$os = Get-CimInstance -ClassName Win32_OperatingSystem
$cpu = @(Get-CimInstance -ClassName Win32_Processor | Select-Object -First 1)[0]
if ($os.ProductType -ne 1) { throw "Not a Windows client OS (ProductType=$($os.ProductType))" }
if ($cpu.Architecture -ne 9) { throw "Not x64 hardware (processor architecture=$($cpu.Architecture))" }
$build = [int]$os.BuildNumber
if ($build -lt 19041) { throw "Windows 10 2004+ or Windows 11 is required for this QA script" }
$family = if ($build -ge 22000) { 'Windows 11' } else { 'Windows 10' }

# Registry software inventories are informational only, not evidence of a clean image.
$uninstallKeys = @(
  'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
  'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
)
$redist = @(
  Get-ItemProperty -Path $uninstallKeys -ErrorAction SilentlyContinue |
    Where-Object { $_.DisplayName -match '^Microsoft Visual C\+\+.*Redistributable' } |
    Select-Object -ExpandProperty DisplayName -Unique
)
New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null

$proc = $null
$started = $false
$idleMs = $null
$workingSet = $null
$privateBytes = $null
$windowFound = $false
try {
  $proc = Start-Process -FilePath $exePath -PassThru
  Start-Sleep -Milliseconds 1600
  $proc.Refresh()
  if ($proc.HasExited) { throw "LockPeek exited early with code $($proc.ExitCode)" }
  $windowFound = $proc.MainWindowHandle -ne [IntPtr]::Zero
  if (!$windowFound) { throw 'LockPeek main window not found' }
  $before = $proc.TotalProcessorTime
  Start-Sleep -Milliseconds 2000
  $proc.Refresh()
  if ($proc.HasExited) { throw "LockPeek exited during idle sample: $($proc.ExitCode)" }
  $idleMs = ($proc.TotalProcessorTime - $before).TotalMilliseconds
  $workingSet = $proc.WorkingSet64
  $privateBytes = $proc.PrivateMemorySize64
  $started = $true
} finally {
  if ($null -ne $proc) {
    try {
      $proc.Refresh()
      if (!$proc.HasExited) {
        if (!$proc.CloseMainWindow() -or !$proc.WaitForExit(5000)) {
          Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
      }
    } finally {
      $proc.Dispose()
    }
  }
}
$report = [ordered]@{
  result = 'AUTOMATED_PREFLIGHT_PASS_MANUAL_QA_PENDING'
  timestampUtc = [DateTime]::UtcNow.ToString('o')
  operatingSystem = $os.Caption
  osVersion = $os.Version
  osBuild = $os.BuildNumber
  clientFamily = $family
  x64Hardware = $true
  machineName = $env:COMPUTERNAME
  powershellVersion = $PSVersionTable.PSVersion.ToString()
  executableSha256 = $actualHash
  executableBytes = (Get-Item -LiteralPath $exePath).Length
  guiStartup = $started
  mainWindowFound = $windowFound
  idleCpuMillisecondsOver2Seconds = $idleMs
  workingSetBytes = $workingSet
  privateBytes = $privateBytes
  detectedVcRedistributables = $redist
  limitation = 'Not proof of a clean OS or missing runtimes. Complete docs/TESTING.md manual QA separately.'
}
$reportPath = Join-Path $ReportDirectory "$($family.Replace(' ', '-'))-preflight.json"
$report | ConvertTo-Json -Depth 5 | Out-File -LiteralPath $reportPath -Encoding utf8
Write-Host "Automated preflight PASS, manual QA still required. Report: $reportPath"
