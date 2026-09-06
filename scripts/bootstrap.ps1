$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$upstream = Join-Path $root 'vendor/windows-classic-samples'
$base = Join-Path $root 'vendor/baseclasses'
$revision = '434f6002bdf9cf9829406c3ff2b33387982d6168'
if (!(Test-Path "$base/streams.h")) {
    if (!(Test-Path "$upstream/.git")) {
        & git clone --filter=blob:none --no-checkout https://github.com/microsoft/Windows-classic-samples.git $upstream
        if ($LASTEXITCODE) { throw 'Could not download DirectShow base classes.' }
    }
    & git -C $upstream sparse-checkout set Samples/Win7Samples/multimedia/directshow/baseclasses
    & git -C $upstream checkout $revision
    if ($LASTEXITCODE) { throw 'Could not checkout pinned Microsoft sample revision.' }
    New-Item -ItemType Directory -Force $base | Out-Null
    Copy-Item "$upstream/Samples/Win7Samples/multimedia/directshow/baseclasses/*" $base
    Copy-Item "$upstream/LICENSE" "$base/LICENSE.txt"
}
$archive = Join-Path $root 'vendor/ffmpeg-release-essentials.zip'
$destination = Join-Path $root 'vendor/ffmpeg'
if (!(Get-ChildItem $destination -Filter ffmpeg.exe -Recurse -ErrorAction SilentlyContinue)) {
    if (!(Test-Path $archive)) {
        & curl.exe -L --fail --output $archive https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip
        if ($LASTEXITCODE) { throw 'FFmpeg download failed.' }
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $destination -Force
}
Write-Host 'Dependencies are ready.'
