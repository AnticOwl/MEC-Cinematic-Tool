# Tests the production camera-session math, its IGCS data layout, and optionally
# the exports of a built x64 camera DLL. Requires Visual Studio 2022 C++ tools.
param(
  [ValidateSet('Release', 'Debug')]
  [string]$Configuration = 'Release',
  [string]$DllPath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswherePath)) { throw 'Visual Studio 2022 C++ Build Tools are required.' }
$vsInstallPath = & $vswherePath -latest -products '*' `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
  Select-Object -First 1
if (-not $vsInstallPath) { throw 'The Visual Studio C++ workload was not found.' }

# Microsoft's supported entry point configures cl.exe and dumpbin.exe in this
# PowerShell process and keeps the caller's working directory.
$devShellPath = Join-Path $vsInstallPath 'Common7\Tools\Launch-VsDevShell.ps1'
& $devShellPath -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Host

$testDirectory = Join-Path ([IO.Path]::GetTempPath()) ('mec-igcs-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDirectory | Out-Null
try {
  $testExecutable = Join-Path $testDirectory 'igcs_session_tests.exe'
  $compilerArguments = @(
    '/nologo', '/EHsc', '/W4', '/WX', '/std:c++14',
    ('/I' + (Join-Path $repoRoot 'CT_MirrorsEdge')),
    ('/Fo' + (Join-Path $testDirectory 'igcs_session_tests.obj')),
    ('/Fe' + $testExecutable),
    (Join-Path $repoRoot 'tests\igcs_session_tests.cpp')
  )
  if ($Configuration -eq 'Debug') {
    $compilerArguments += '/MTd', '/Od', '/Zi', ('/Fd' + (Join-Path $testDirectory 'igcs_session_tests.pdb'))
  } else {
    $compilerArguments += '/MT', '/O2'
  }
  & cl.exe @compilerArguments
  if ($LASTEXITCODE -ne 0) { throw 'IGCS session tests did not compile.' }
  & $testExecutable
  if ($LASTEXITCODE -ne 0) { throw 'IGCS session tests failed.' }

  if ($DllPath) {
    $cameraDllPath = (Resolve-Path -LiteralPath $DllPath).Path
    $headers = & dumpbin.exe /nologo /headers $cameraDllPath
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the camera DLL headers.' }
    if (-not ($headers | Select-String -Pattern '^\s+8664 machine' -Quiet)) {
      throw 'The camera DLL must be built for x64.'
    }
    $exports = & dumpbin.exe /nologo /exports $cameraDllPath
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the camera DLL exports.' }
    foreach ($exportName in @(
      'IGCS_StartScreenshotSession', 'IGCS_EndScreenshotSession',
      'IGCS_MoveCameraPanorama', 'IGCS_MoveCameraMultishot'
    )) {
      $exportPattern = '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+' + [regex]::Escape($exportName) + '(?:\s|$)'
      if (-not ($exports | Select-String -Pattern $exportPattern -Quiet)) {
        throw "Missing unmangled IGCS export: $exportName"
      }
    }
    Write-Host 'PASS: camera DLL is x64 and exports all four native IGCS entry points.'
  }
} finally {
  Remove-Item -LiteralPath $testDirectory -Recurse -Force
}
