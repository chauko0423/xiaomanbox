param(
  [Parameter(Mandatory=$true)][string]$Source,
  [Parameter(Mandatory=$true)][string]$Build,
  [Parameter(Mandatory=$true)][string]$CudaRoot,
  [string]$CmakeExe = 'cmake',
  [string]$NinjaExe = 'ninja',
  [string]$VsRoot,
  [switch]$Vision,
  [string]$FfmpegRoot,
  [int]$Jobs = 0
)
$ErrorActionPreference = 'Stop'
if (-not $VsRoot) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2022 C++ tools or supply -VsRoot.' }
  $VsRoot = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$toolRoot = Join-Path $VsRoot 'VC\Tools\MSVC'
$msvc = Get-ChildItem -LiteralPath $toolRoot -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe') } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $msvc) { throw 'MSVC x64 compiler missing.' }
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkVersion = Get-ChildItem -LiteralPath (Join-Path $sdk 'Include') -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdkVersion) { throw 'Windows SDK missing.' }
$sdkV = $sdkVersion.Name
$compiler = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$env:VSLANG = '1033'
$env:PATH = "$(Split-Path $compiler);$sdk\bin\$sdkV\x64;$CudaRoot\bin;$env:PATH"
$env:INCLUDE = "$($msvc.FullName)\include;$sdk\Include\$sdkV\ucrt;$sdk\Include\$sdkV\um;$sdk\Include\$sdkV\shared;$sdk\Include\$sdkV\winrt"
$env:LIB = "$($msvc.FullName)\lib\x64;$sdk\Lib\$sdkV\ucrt\x64;$sdk\Lib\$sdkV\um\x64"
$cFlags = '/DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUTF8PROC_STATIC /D_CRT_SECURE_NO_WARNINGS /D_SCL_SECURE_NO_WARNINGS /utf-8'
$hostFlags = "$cFlags /Zc:preprocessor /Zc:__cplusplus /bigobj /EHsc"
$cudaFlags = ($hostFlags.Split(' ') | ForEach-Object { "-Xcompiler=$_" }) -join ' '
$mediaOptions = @('-DNINFER_ENABLE_VISION=OFF')
if ($Vision) {
  if (-not $FfmpegRoot) { throw '-Vision requires -FfmpegRoot containing the shared FFmpeg headers and import libraries.' }
  $mediaOptions = @('-DNINFER_ENABLE_VISION=ON',"-DNINFER_FFMPEG_ROOT=$([System.IO.Path]::GetFullPath($FfmpegRoot))")
}
& $CmakeExe -S $Source -B $Build -G Ninja "-DCMAKE_MAKE_PROGRAM=$NinjaExe" "-DCMAKE_C_COMPILER=$compiler" "-DCMAKE_CXX_COMPILER=$compiler" "-DCMAKE_CUDA_COMPILER=$CudaRoot\bin\nvcc.exe" "-DCMAKE_CUDA_HOST_COMPILER=$compiler" "-DCMAKE_C_FLAGS=$cFlags" "-DCMAKE_CXX_FLAGS=$hostFlags" "-DCMAKE_CUDA_FLAGS=$cudaFlags" '-DCMAKE_CUDA_ARCHITECTURES=120a' '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_CXX_SCAN_FOR_MODULES=OFF' '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded' @mediaOptions '-DBUILD_TESTING=OFF' '-DNINFER_BUILD_BENCHMARKS=OFF'
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
if ($Jobs -gt 0) { & $CmakeExe --build $Build --target ninfer-serve -j $Jobs }
else { & $CmakeExe --build $Build --target ninfer-serve -j }
if ($LASTEXITCODE -ne 0) { throw 'NInfer build failed.' }
Write-Output "Runtime built: $Build\apps\ninfer-serve.exe"
