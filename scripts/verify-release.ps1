param(
  [Parameter(Mandatory=$true)][string]$Exe,
  [Parameter(Mandatory=$true)][string]$ReportDirectory
)
$ErrorActionPreference='Stop'
New-Item -Force -ItemType Directory $ReportDirectory | Out-Null
if (!(Test-Path -LiteralPath $Exe)) { throw "Missing EXE: $Exe" }
$File=Get-Item -LiteralPath $Exe
$Bytes=$File.Length
$Hash=(Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
$VsWhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $VsWhere)) { throw 'vswhere is missing' }
$Root=(& $VsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (!$Root) { throw 'MSVC Build Tools not installed' }
$VcVars=Join-Path $Root 'VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path $VcVars)) { throw 'Missing vcvars64.bat' }
$Toolchain=cmd /d /s /c "call `"$VcVars`" >nul && cl 2>&1" 2>&1 | Out-String
if ($Toolchain -notmatch 'Version 19\.(\d+)') { throw "Unable to identify cl.exe: $Toolchain" }
$Minor=[int]$Matches[1]
if ($Minor -lt 52) { throw "MSVC 14.52+ with stable /std:c++23 is required, got 19.$Minor" }
$Dependencies=(cmd /d /s /c "call `"$VcVars`" >nul && dumpbin /dependents `"$Exe`"") -join "`n"
$Headers=(cmd /d /s /c "call `"$VcVars`" >nul && dumpbin /headers `"$Exe`"") -join "`n"
$LoadConfig=(cmd /d /s /c "call `"$VcVars`" >nul && dumpbin /loadconfig `"$Exe`"") -join "`n"
$Dependencies | Out-File -Encoding utf8 (Join-Path $ReportDirectory 'dependents.txt')
$Headers | Out-File -Encoding utf8 (Join-Path $ReportDirectory 'headers.txt')
$LoadConfig | Out-File -Encoding utf8 (Join-Path $ReportDirectory 'loadconfig.txt')
foreach ($name in @('vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll','ucrtbase.dll','hostfxr.dll','hostpolicy.dll','coreclr.dll')) {
  if ($Dependencies -match [regex]::Escape($name)) { throw "Dynamic runtime dependency: $name" }
}
$Allowed=@('KERNEL32.DLL','USER32.DLL','GDI32.DLL','COMCTL32.DLL','COMDLG32.DLL','SHELL32.DLL','ADVAPI32.DLL','OLE32.DLL','OLEAUT32.DLL','SHLWAPI.DLL','NTDLL.DLL','IMM32.DLL')
$ImportLines=[regex]::Matches($Dependencies, '(?im)^\s*([A-Za-z0-9_.-]+\.dll)\s*$')
foreach ($entry in $ImportLines) {
  $name=$entry.Groups[1].Value.ToUpperInvariant()
  if ($Allowed -notcontains $name) { throw "Unexpected imported DLL: $name" }
}
if ($ImportLines.Count -eq 0) { throw 'No PE import entries parsed' }
foreach ($flag in @('Dynamic base','NX compatible','High Entropy Virtual Addresses')) {
  if ($Headers -notmatch [regex]::Escape($flag)) { throw "Missing PE hardening flag: $flag" }
}
if ($Headers -notmatch 'machine \(x64\)') { throw 'PE architecture is not x64' }
if ($Headers -notmatch 'Guard') { throw 'CFG metadata not seen in PE headers' }
$CompilerLine=([regex]::Match($Toolchain,'Version 19\.\d+[^\r\n]*')).Value
$Metadata=@(
  'LockPeek.exe',
  "Size: $Bytes bytes",
  'Architecture: x64',
  "Compiler: $CompilerLine",
  'Language: C++23 (/std:c++23)',
  'CRT: static (/MT; dynamically imported VC++ CRT blocked)',
  "SHA256: $Hash"
)
$Metadata | Out-File -Encoding utf8 (Join-Path $ReportDirectory 'build-info.txt')
Write-Host ($Metadata -join "`n")
if ($Bytes -gt 512000) { Write-Warning 'EXE exceeds the preliminary 500 KiB goal; investigate sections and imports.' }
