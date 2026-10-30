# Gets the LLVM that builds the game's code module into third_party\llvm, where CMake (LLVM_ROOT) looks for it
# (ANALYSIS.md, Phase 5, The game's code module): clang.exe, lld-link.exe and clang's own headers from LLVM's own
# Windows x64 build, pinned and checked against its SHA-256, and the licenses of LLVM, clang and lld from its tag.
# Visual Studio's clang is the same version today and builds the same module; with this one, the module doesn't depend
# on the Visual Studio a PC has.
# The SHA-256 is GitHub's for the release's file, whose build attestation (gh attestation verify <file> --repo
# llvm/llvm-project) names LLVM's release workflow at the tag. The archive is about 820 MB; what's kept, about 200 MB.
# Usage: powershell -ExecutionPolicy Bypass -File tools\getllvm.ps1 [-Archive <the .tar.xz, already downloaded>]
param([string]$Archive = '')
$ErrorActionPreference = 'Stop'
$version = '22.1.3'
$name = "clang+llvm-$version-x86_64-pc-windows-msvc"
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$name.tar.xz"
$sha256 = 'F2D7D200F72C83880AA2C1C00F6B6A67D0EA607EA663DAFBD0CB2DB6E2819ED0'
# The archive has no license of its own: the projects' from the tag.
$licenses = [ordered]@{
    llvm  = '8D85C1057D742E597985C7D4E6320B015A9139385CFF4CBAE06FFC0EBE89AFEE'
    clang = 'EBCD9BBF783A73D05C53BA4D586B8D5813DCDF3BBEC50265860CCC885E606F47'
    lld   = 'F7891568956E34643EB6A0DB1462DB30820D40D7266E2A78063F2FE233ECE5A0'
}
$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root 'third_party\llvm'
$work = Join-Path ([IO.Path]::GetTempPath()) "doa4-getllvm-$PID"
New-Item -ItemType Directory -Force $work | Out-Null
try {
    if (-not $Archive) {
        $Archive = Join-Path $work "$name.tar.xz"
        "downloading $url (about 820 MB)"
        curl.exe --fail --location --silent --show-error --output $Archive $url
        if ($LASTEXITCODE -ne 0) { throw "download failed" }
    }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Archive).Hash
    if ($hash -ne $sha256) { throw "$Archive has SHA-256 $hash, not $sha256" }

    $extract = Join-Path $work 'extract'
    New-Item -ItemType Directory $extract | Out-Null
    $major = $version.Split('.')[0]
    # Windows' own tar unpacks .tar.xz; only what the module's build uses.
    & "$env:SystemRoot\System32\tar.exe" -xf $Archive -C $extract "$name/bin/clang.exe" "$name/bin/lld-link.exe" "$name/lib/clang/$major/include"
    if ($LASTEXITCODE -ne 0) { throw "can't unpack $Archive" }
    $unpacked = Join-Path $extract $name
    foreach ($project in $licenses.Keys) {
        $license = Join-Path $unpacked "LICENSE-$project.TXT"
        curl.exe --fail --location --silent --show-error --output $license "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$version/$project/LICENSE.TXT"
        if ($LASTEXITCODE -ne 0) { throw "download of $project's license failed" }
        $hash = (Get-FileHash -Algorithm SHA256 $license).Hash
        if ($hash -ne $licenses[$project]) { throw "$project's license has SHA-256 $hash, not $($licenses[$project])" }
    }
    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    Move-Item $unpacked $target
    "LLVM $version (clang, lld-link, clang's headers and the licenses) is in $target; rerun build.cmd to use it"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
