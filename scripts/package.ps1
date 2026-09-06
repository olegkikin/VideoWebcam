param([ValidateSet('Release','Debug')][string]$Configuration = 'Release',
      [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$PackageName = 'VideoWebcam')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dist = Join-Path $root "dist/$PackageName"
New-Item -ItemType Directory -Force $dist | Out-Null
# Check all binaries before replacing any of them: the app and DLLs share an IPC version.
foreach ($name in @('VideoWebcam.exe','VideoWebcamFilter64.dll','VideoWebcamFilter32.dll','ffmpeg.exe')) {
    $destination = Join-Path $dist $name
    if (Test-Path $destination) {
        try { $handle = [IO.File]::Open($destination,'Open','ReadWrite','None'); $handle.Dispose() }
        catch { throw "Close apps using $name before updating $dist, or choose a different PackageName." }
    }
}
Copy-Item "$root/build-x64/$Configuration/VideoWebcam.exe" $dist
Copy-Item "$root/build-x64/$Configuration/VideoWebcamFilter64.dll" $dist
Copy-Item "$root/build-Win32/$Configuration/VideoWebcamFilter.dll" "$dist/VideoWebcamFilter32.dll"
$ffmpeg = Get-ChildItem "$root/vendor/ffmpeg" -Filter ffmpeg.exe -Recurse | Select-Object -First 1
if (!$ffmpeg) { throw 'Run scripts/bootstrap.ps1 to download FFmpeg.' }
Copy-Item $ffmpeg.FullName $dist
Copy-Item "$root/README.md","$root/THIRD_PARTY_NOTICES.md","$root/LICENSE" $dist
Copy-Item "$root/scripts/install.cmd","$root/scripts/uninstall.cmd" $dist
New-Item -ItemType Directory -Force "$dist/licenses" | Out-Null
Copy-Item "$root/vendor/baseclasses/LICENSE.txt" "$dist/licenses/Microsoft-DirectShow.txt"
$ffmpegRoot = Split-Path (Split-Path $ffmpeg.FullName -Parent) -Parent
Copy-Item "$ffmpegRoot/LICENSE" "$dist/licenses/FFmpeg.txt"
Copy-Item "$ffmpegRoot/README.txt" "$dist/licenses/FFmpeg-build.txt"
Write-Host "Ready: $dist/VideoWebcam.exe"
