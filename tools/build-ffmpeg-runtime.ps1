param(
  [Parameter(Mandatory=$true)][string]$Source,
  [Parameter(Mandatory=$true)][string]$ZlibSource,
  [Parameter(Mandatory=$true)][string]$Build,
  [Parameter(Mandatory=$true)][string]$Prefix,
  [Parameter(Mandatory=$true)][string]$MsysRoot,
  [string]$VsRoot,
  [string]$CmakeExe = 'cmake',
  [string]$NinjaExe = 'ninja',
  [int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
if (-not $VsRoot) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  $VsRoot = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$msvc = Get-ChildItem -LiteralPath (Join-Path $VsRoot 'VC\Tools\MSVC') -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkV = (Get-ChildItem -LiteralPath (Join-Path $sdk 'Include') -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1).Name
$compiler = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$env:VSLANG = '1033'
$env:PATH = "$(Split-Path $compiler);$sdk\bin\$sdkV\x64;$env:PATH"
$env:INCLUDE = "$($msvc.FullName)\include;$sdk\Include\$sdkV\ucrt;$sdk\Include\$sdkV\um;$sdk\Include\$sdkV\shared;$sdk\Include\$sdkV\winrt"
$env:LIB = "$($msvc.FullName)\lib\x64;$sdk\Lib\$sdkV\ucrt\x64;$sdk\Lib\$sdkV\um\x64"
$Source = (Resolve-Path -LiteralPath $Source).Path
$ZlibSource = (Resolve-Path -LiteralPath $ZlibSource).Path
$Build = [System.IO.Path]::GetFullPath($Build)
$Prefix = [System.IO.Path]::GetFullPath($Prefix)
New-Item -ItemType Directory -Path $Build,$Prefix -Force | Out-Null
$zBuild = Join-Path $Build 'zlib'
$zPrefix = Join-Path $Build 'zlib-install'
& $CmakeExe -S $ZlibSource -B $zBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=$NinjaExe" "-DCMAKE_C_COMPILER=$compiler" '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW' '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded'
if ($LASTEXITCODE -ne 0) { throw 'zlib configure failed.' }
& $CmakeExe --build $zBuild --target zlibstatic -j $Jobs
if ($LASTEXITCODE -ne 0) { throw 'zlib build failed.' }
New-Item -ItemType Directory -Path "$zPrefix\include","$zPrefix\lib" -Force | Out-Null
Copy-Item -LiteralPath "$ZlibSource\zlib.h","$zBuild\zconf.h" -Destination "$zPrefix\include"
Copy-Item -LiteralPath "$zBuild\zlibstatic.lib" -Destination "$zPrefix\lib\zlib.lib"
$env:XIAOMAN_MSVC_BIN = Split-Path $compiler
$env:XIAOMAN_FFMPEG_SOURCE = $Source
$env:XIAOMAN_FFMPEG_BUILD = Join-Path $Build 'ffmpeg'
$env:XIAOMAN_FFMPEG_PREFIX = $Prefix
$env:XIAOMAN_ZLIB_PREFIX = $zPrefix
$env:XIAOMAN_BUILD_JOBS = [string]$Jobs
New-Item -ItemType Directory -Path $env:XIAOMAN_FFMPEG_BUILD -Force | Out-Null
$script = @'
set -eu
export PATH="$(/usr/bin/cygpath -u "$XIAOMAN_MSVC_BIN"):/usr/bin:$PATH"
source_dir=$(/usr/bin/cygpath -u "$XIAOMAN_FFMPEG_SOURCE")
build_dir=$(/usr/bin/cygpath -u "$XIAOMAN_FFMPEG_BUILD")
prefix_dir=$(/usr/bin/cygpath -u "$XIAOMAN_FFMPEG_PREFIX")
zlib_dir=$(/usr/bin/cygpath -m "$XIAOMAN_ZLIB_PREFIX")
cd "$build_dir"
"$source_dir/configure" --prefix="$prefix_dir" --toolchain=msvc --arch=x86_64 --target-os=win64 \
 --enable-shared --disable-static --disable-programs --disable-doc --disable-debug --disable-x86asm \
 --disable-autodetect --disable-network --disable-everything --disable-avdevice --disable-avfilter --disable-swresample \
 --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-zlib \
 --enable-decoder=png,mjpeg,bmp,gif,webp,h264,hevc,vp8,vp9 \
 --enable-demuxer=image2,image2pipe,image_png_pipe,image_jpeg_pipe,image_bmp_pipe,image_webp_pipe,image_gif_pipe,gif,h264,hevc,mov,matroska,avi \
 --enable-parser=h264,hevc,vp8,vp9,png,mjpeg,webp --enable-protocol=file \
 --extra-cflags="-MT -I$zlib_dir/include" --extra-ldflags="-libpath:$zlib_dir/lib"
make -j "$XIAOMAN_BUILD_JOBS"
make install
'@
$scriptPath = Join-Path $Build 'build-ffmpeg.sh'
[System.IO.File]::WriteAllText($scriptPath,$script.Replace("`r`n","`n"),[System.Text.UTF8Encoding]::new($false))
& (Join-Path $MsysRoot 'usr\bin\bash.exe') $scriptPath
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg build failed; inspect ffbuild/config.log.' }
Write-Output "FFmpeg LGPL shared libraries built: $Prefix"
