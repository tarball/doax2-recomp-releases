# Gets the FFmpeg build the runtime decodes XMA audio with into third_party\ffmpeg, where CMake looks for it
# (FFMPEG_ROOT): BtbN's LGPL Windows x64 build with shared libraries, pinned and checked against its SHA-256.
# Usage: powershell -ExecutionPolicy Bypass -File tools\getffmpeg.ps1 [-Zip <the zip, already downloaded>]
param([string]$Zip = '')
$ErrorActionPreference = 'Stop'
$name = 'ffmpeg-n8.1.2-50-g1a748fe2cd-win64-lgpl-shared-8.1'
$url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-08-31-13-27/$name.zip"
$sha256 = 'E9712FFBDB03EF71BBAB660C75B835BFE698EF6FAD0247C76D8D394A39A3DB63'
$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root 'third_party\ffmpeg'
$work = Join-Path ([IO.Path]::GetTempPath()) "doa4-getffmpeg-$PID"
New-Item -ItemType Directory -Force $work | Out-Null
try {
    if (-not $Zip) {
        $Zip = Join-Path $work "$name.zip"
        "downloading $url (about 90 MB)"
        curl.exe --fail --location --silent --show-error --output $Zip $url
        if ($LASTEXITCODE -ne 0) { throw "download failed" }
    }
    $hash = (Get-FileHash -Algorithm SHA256 $Zip).Hash
    if ($hash -ne $sha256) { throw "$Zip has SHA-256 $hash, not $sha256" }
    Expand-Archive -Path $Zip -DestinationPath $work -Force
    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    Move-Item (Join-Path $work $name) $target
    "FFmpeg $name is in $target; rerun build.cmd to use it"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
