# Builds CT_MirrorsEdge.dll (Release x64).
# Needs Visual Studio 2022 (or Build Tools) with the C++ workload and a Windows 10/11 SDK.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-Configuration Debug]

param(
  [ValidateSet('Release', 'Debug')]
  [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found, install Visual Studio 2022 or the Build Tools' }

$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild.exe not found' }
Write-Host "MSBuild: $msbuild"

# 1. MinHook (https://github.com/TsudaKageyu/minhook, v1.3.3) as a static library.
#    Both MinHook and the tools link the CRT statically (/MT), so the injected
#    DLL doesn't depend on whichever VC++ redistributable is installed.
$minhookDir = Join-Path $root 'minhook'
if (-not (Test-Path (Join-Path $minhookDir 'include\MinHook.h'))) {
  throw "MinHook not found in $minhookDir. Clone it with: git clone --branch v1.3.3 https://github.com/TsudaKageyu/minhook.git `"$minhookDir`""
}

$minhookLib = Join-Path $minhookDir "lib\libMinHook.x64.lib"
if ($Configuration -eq 'Debug') { $minhookLib = Join-Path $minhookDir "lib\libMinHook.x64.debug.lib" }

# MinHook only looks for trampoline memory within 1GB of a hooked function,
# and Mirror's Edge fills that space with its heap. Widen it (see buffer.c).
$bufferC = Join-Path $minhookDir 'src\buffer.c'
$bufferSource = [IO.File]::ReadAllText($bufferC)
if ($bufferSource -match '#define MAX_MEMORY_RANGE 0x40000000') {
  $bufferSource = $bufferSource.Replace('#define MAX_MEMORY_RANGE 0x40000000',
    "// CT_MirrorsEdge: raised from 0x40000000, see build.ps1`r`n#define MAX_MEMORY_RANGE 0x70000000")
  [IO.File]::WriteAllText($bufferC, $bufferSource)
  Write-Host 'Patched MinHook MAX_MEMORY_RANGE'
}
$minhookPatchMarker = Join-Path $minhookDir 'lib\range_0x70000000.txt'
if (-not (Test-Path $minhookPatchMarker)) { Remove-Item $minhookLib -ErrorAction SilentlyContinue }

if (-not (Test-Path $minhookLib)) {
  # MinHookNoLTCG.props turns off /GL, see the comment in that file
  $noLtcgProps = Join-Path $root 'tools\MinHookNoLTCG.props'
  & $msbuild (Join-Path $minhookDir 'build\VC15\libMinHook.vcxproj') /nologo /v:minimal `
    /p:Configuration=$Configuration /p:Platform=x64 `
    /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 `
    "/p:ForceImportBeforeCppTargets=$noLtcgProps" | Out-Host
  if ($LASTEXITCODE -ne 0) { throw 'MinHook build failed, see the MSBuild output above' }

  New-Item -ItemType Directory -Force (Split-Path $minhookLib) | Out-Null
  Copy-Item (Join-Path $minhookDir "build\VC15\lib\$Configuration\libMinHook.x64.lib") $minhookLib
  Set-Content $minhookPatchMarker 'libMinHook built with MAX_MEMORY_RANGE 0x70000000'
}

# 2. The tools
& $msbuild (Join-Path $root 'CT_MirrorsEdge\Cinematic Tools.vcxproj') /nologo /v:minimal `
  /p:Configuration=$Configuration /p:Platform=x64 `
  "/p:MinHookDir=$minhookDir" "/p:MinHookLib=$(Split-Path -Leaf $minhookLib)" | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'CT_MirrorsEdge build failed, see the MSBuild output above' }

# 3. The usp10.dll loader, which makes the game load the tools by itself
& $msbuild (Join-Path $root 'Loader\Loader.vcxproj') /nologo /v:minimal `
  /p:Configuration=$Configuration /p:Platform=x64 | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Loader build failed, see the MSBuild output above' }

# 4. Everything that goes into the game folder
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item (Join-Path $root "CT_MirrorsEdge\x64\$Configuration\CT_MirrorsEdge.dll") $dist -Force
Copy-Item (Join-Path $root "Loader\x64\$Configuration\usp10.dll") $dist -Force
# The old version.dll loader, which the game never picks up
Remove-Item (Join-Path $dist 'version.dll') -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Built $dist\CT_MirrorsEdge.dll and $dist\usp10.dll"
Write-Host "Copy both next to MirrorsEdgeCatalyst.exe."
