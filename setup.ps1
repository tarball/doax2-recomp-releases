# Builds Dead or Alive Xtreme 2 from source for your own disc image: checks the tools, extracts and unpacks the game's
# executable, recompiles it, builds the game, and puts it in a folder of its own with a shortcut. A release does
# without all this: its doax2.exe runs from the disc image as it is (see INSTALL.md). The game made here works the same
# way, reading the game from the disc image that settings.ini points at.
#
# Usage: powershell -ExecutionPolicy Bypass -File setup.ps1 [-Iso <disc image>] [-Destination <folder>]
#   -Iso defaults to the first .iso in private\ or this folder, else asks.
#   -Destination defaults to game\ in this folder. Running setup again updates the game there, keeping its saves and
#   settings.
param(
    [string]$Iso = '',
    [string]$Destination = ''
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
# The game: its name, executable and XenonRecomp config, and the default.xex this project supports, Dead or Alive
# Xtreme 2 (USA), title 544307D2: the version the config describes.
$title = 'Dead or Alive Xtreme 2'
$exe = 'doax2.exe'
$config = 'config\DOAX2.toml'
$xexHash = 'C4A14853E1F7AF951BE34F380604113703638780B36D5E74E674FE7431992870'

function Step([string]$text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Fail([string]$text) { Write-Host "`n$text" -ForegroundColor Red; exit 1 }

Step 'Checking the tools'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = $null
if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath }
if (-not $vs) { Fail 'Needs Visual Studio 2026 (Build Tools or any edition) with "Desktop development with C++" and "C++ Clang tools for Windows" (see INSTALL.md).' }
Write-Host "Visual Studio: $vs"
if (-not (Get-Command python -ErrorAction SilentlyContinue)) { Fail 'Needs Python 3 (see INSTALL.md).' }
$xenonRecomp = $env:XENONRECOMP_ROOT
if (-not $xenonRecomp) { $xenonRecomp = Join-Path (Split-Path $root -Parent) 'XenonRecomp' }
if (-not (Test-Path (Join-Path $xenonRecomp 'thirdparty\simde\x86'))) {
    Fail "Needs the XenonRecomp checkout (doa4 branch, with its submodules) at $xenonRecomp, or XENONRECOMP_ROOT set to it (see INSTALL.md)."
}
Write-Host "XenonRecomp: $xenonRecomp"

Step 'Finding the disc image'
if (-not $Iso) {
    $found = @(Get-ChildItem (Join-Path $root 'private\*.iso'), (Join-Path $root '*.iso') -ErrorAction SilentlyContinue)
    if ($found.Count) { $Iso = $found[0].FullName }
}
if (-not $Iso) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Your $title disc image"
    $dialog.Filter = 'Disc images (*.iso)|*.iso|All files (*.*)|*.*'
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail 'No disc image chosen.' }
    $Iso = $dialog.FileName
}
if (-not (Test-Path $Iso)) { Fail "There's no $Iso." }
$Iso = (Resolve-Path $Iso).Path
Write-Host $Iso

Step "Extracting and checking the game's executable"
$private = Join-Path $root 'private'
New-Item -ItemType Directory -Force $private | Out-Null
& python -I (Join-Path $root 'tools\xdvdfs.py') $Iso extract default.xex (Join-Path $private 'default.xex')
if ($LASTEXITCODE) { Fail "Can't read default.xex from $Iso. Is it an Xbox 360 disc image?" }
$hash = (Get-FileHash (Join-Path $private 'default.xex') -Algorithm SHA256).Hash
if ($hash -ne $xexHash) {
    Fail "This disc's default.xex (SHA-256 $hash) isn't the one this project supports: $title (USA, title 544307D2)."
}
& powershell -ExecutionPolicy Bypass -File (Join-Path $root 'tools\xexunpack.ps1') (Join-Path $private 'default.xex') (Join-Path $private 'default.pe')
if ($LASTEXITCODE) { Fail "Can't unpack default.xex." }

Step 'Recompiling the game code'
$recompilerBuild = Join-Path $xenonRecomp 'out\build\x64-Clang-Release'
$recompiler = Join-Path $recompilerBuild 'XenonRecomp\XenonRecomp.exe'
if (-not (Test-Path $recompiler)) {
    Write-Host 'Building XenonRecomp'
    & cmd.exe /c call (Join-Path $root 'tools\env.cmd') '&&' cmake -S $xenonRecomp -B $recompilerBuild -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
        -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl '&&' cmake --build $recompilerBuild --target XenonRecomp
    if ($LASTEXITCODE) { Fail "Can't build XenonRecomp." }
}
Push-Location $root
try {
    & $recompiler $config (Join-Path $xenonRecomp 'XenonUtils\ppc_context.h') | Select-Object -Last 1
    $code = $LASTEXITCODE
}
finally { Pop-Location }
if ($code) { Fail 'XenonRecomp failed.' }

if (-not (Test-Path (Join-Path $root 'third_party\ffmpeg\bin'))) {
    Step 'FFmpeg'
    $answer = Read-Host 'The game decodes its music, voices and effects with FFmpeg. Download its pinned build (about 90 MB)? [Y/n]'
    if ($answer -notmatch '^[nN]') {
        & powershell -ExecutionPolicy Bypass -File (Join-Path $root 'tools\getffmpeg.ps1')
        if ($LASTEXITCODE) { Fail "Can't get FFmpeg." }
    }
    else { Write-Host 'Without FFmpeg the game is silent; run setup again to add it.' -ForegroundColor Yellow }
}

Step 'Building the game'
& cmd.exe /c call (Join-Path $root 'build.cmd') Release
if ($LASTEXITCODE) { Fail 'The build failed.' }

if (-not $Destination) { $Destination = Join-Path $root 'game' }
New-Item -ItemType Directory -Force $Destination | Out-Null
$Destination = (Resolve-Path $Destination).Path
Step "Putting the game in $Destination"
$data = Join-Path $Destination 'data'
New-Item -ItemType Directory -Force $data | Out-Null
$build = Join-Path $root 'build\Release'
Copy-Item (Join-Path $build $exe) $Destination -Force
Copy-Item (Join-Path $build '*.dll') $Destination -Force
$ffmpegLicense = Join-Path $root 'third_party\ffmpeg\LICENSE.txt'
if (Test-Path $ffmpegLicense) { Copy-Item $ffmpegLicense (Join-Path $Destination 'FFmpeg LICENSE.txt') -Force }
# The game reads its executable from the disc image, as a release does: copies an earlier setup put here go.
foreach ($name in 'default.xex', 'default.pe') {
    if (Test-Path (Join-Path $data $name)) { Remove-Item -Force (Join-Path $data $name) }
}
# settings.ini: the disc image, with any other settings already there.
$settings = Join-Path $data 'settings.ini'
$kept = @()
if (Test-Path $settings) {
    $kept = @(Get-Content $settings | Where-Object {
        $_ -notmatch '^\s*disc_image\s*=' -and $_ -ne '[game]' -and $_ -ne '; The disc image the game runs from.' -and
        $_ -notmatch "^; $title recomp settings" })
}
$lines = @("; $title recomp settings", '[game]', '; The disc image the game runs from.', "disc_image = $Iso") + $kept
Set-Content $settings $lines -Encoding Default
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut((Join-Path $Destination "$title.lnk"))
$shortcut.TargetPath = Join-Path $Destination $exe
$shortcut.WorkingDirectory = $Destination
$shortcut.Save()

Step 'Done'
Write-Host "Play with `"$Destination\$title.lnk`" (or $exe there)."
Write-Host "Saves, settings and the log ($([IO.Path]::ChangeExtension($exe, 'log'))) are in $data."
Write-Host 'The first run copies 1.7 GB of the game to the hard disk folder in data\hdd (for about 7 minutes, in the background),'
Write-Host 'and compiles the shaders in the background for a minute or two: until then parts of a scene can appear late.'
