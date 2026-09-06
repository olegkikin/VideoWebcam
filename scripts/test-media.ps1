$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ffmpeg = Join-Path $root 'dist/VideoWebcam/ffmpeg.exe'
$fixtures = Join-Path $root 'test-output/media with spaces'
New-Item -ItemType Directory -Force $fixtures | Out-Null
$files = @()
foreach ($extension in @('jpg','png','webp','mp4','mkv','webm')) {
    $file = Join-Path $fixtures "red fixture.$extension"
    $arguments = @('-hide_banner','-loglevel','error','-y','-f','lavfi','-i','color=c=red:s=640x480:r=30')
    if ($extension -in @('jpg','png','webp')) { $arguments += @('-frames:v','1') }
    else { $arguments += @('-t','1') }
    & $ffmpeg @arguments $file
    if ($LASTEXITCODE) { throw "Fixture generation failed: $extension" }
    $files += $file
}
Push-Location "$root/test-output"
try {
    & "$root/build-x64/Release/decoder_tests.exe" $ffmpeg @files
    if ($LASTEXITCODE) { throw 'Decoder tests failed.' }
} finally { Pop-Location }
$version = & $ffmpeg -version 2>&1 | Select-Object -First 3
$hash = (Get-FileHash -LiteralPath $ffmpeg -Algorithm SHA256).Hash
@("FFmpeg SHA-256: $hash",$version) | Set-Content "$root/dist/VideoWebcam/build-info.txt"
Write-Host 'All media tests passed.'
