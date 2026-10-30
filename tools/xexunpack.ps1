# Decrypts (retail key) and decompresses (basic compression) a XEX2 into its raw PE image.
# Usage: powershell -File xexunpack.ps1 <in.xex> <out.pe>
param([string]$InPath, [string]$OutPath)

$data = [IO.File]::ReadAllBytes((Resolve-Path $InPath))

function BE32([byte[]]$b, [int]$o) {
    return [uint32](([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o+1] -shl 16) -bor ([uint32]$b[$o+2] -shl 8) -bor [uint32]$b[$o+3])
}
function BE16([byte[]]$b, [int]$o) { return [int](([int]$b[$o] -shl 8) -bor [int]$b[$o+1]) }

$headerSize = BE32 $data 8
$secOff = BE32 $data 16
$count = BE32 $data 20

$ffi = $null
for ($i = 0; $i -lt $count; $i++) {
    if ((BE32 $data (24 + $i * 8)) -eq 0x3FF) { $ffi = [int](BE32 $data (28 + $i * 8)) }
}
$infoSize = BE32 $data $ffi
$enc = BE16 $data ($ffi + 4)
$comp = BE16 $data ($ffi + 6)
if ($comp -ne 1) { throw "Only basic compression is handled here (got $comp)" }

$payload = New-Object byte[] ($data.Length - $headerSize)
[Array]::Copy($data, $headerSize, $payload, 0, $payload.Length)

if ($enc -eq 1) {
    $retail = [byte[]](0x20,0xB1,0x85,0xA5,0x9D,0x28,0xFD,0xC3,0x40,0x58,0x3F,0xBB,0x08,0x96,0xBF,0x91)
    $aes = [Security.Cryptography.Aes]::Create()
    $aes.Mode = 'CBC'; $aes.Padding = 'None'; $aes.IV = New-Object byte[] 16
    $aes.Key = $retail
    $encKey = New-Object byte[] 16
    [Array]::Copy($data, $secOff + 0x150, $encKey, 0, 16)
    $sessionKey = $aes.CreateDecryptor().TransformFinalBlock($encKey, 0, 16)
    $aes.Key = $sessionKey
    $len = $payload.Length - ($payload.Length % 16)
    $dec = $aes.CreateDecryptor().TransformFinalBlock($payload, 0, $len)
    [Array]::Copy($dec, 0, $payload, 0, $len)
}

$numBlocks = [int]($infoSize / 8) - 1
$total = 0
for ($i = 0; $i -lt $numBlocks; $i++) {
    $total += (BE32 $data ($ffi + 8 + $i * 8)) + (BE32 $data ($ffi + 12 + $i * 8))
}
# The image is the size the security info gives, which can be more than the blocks cover (trailing zeros).
$imageSize = BE32 $data ($secOff + 4)
if ($total -gt $imageSize) { throw "The blocks ($total bytes) don't fit the image ($imageSize bytes)" }
$out = New-Object byte[] $imageSize
$src = 0; $dst = 0
for ($i = 0; $i -lt $numBlocks; $i++) {
    $ds = [int](BE32 $data ($ffi + 8 + $i * 8))
    $zs = [int](BE32 $data ($ffi + 12 + $i * 8))
    [Array]::Copy($payload, $src, $out, $dst, $ds)
    $src += $ds; $dst += $ds + $zs
}
[IO.File]::WriteAllBytes($OutPath, $out)
"wrote $imageSize bytes; MZ=$([char]$out[0])$([char]$out[1])"
