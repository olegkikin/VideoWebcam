# Temporarily registers this build for the current user, then always unregisters it.
# Refuses to overwrite an existing installation.
$ErrorActionPreference = 'Stop'
$testPath = $env:Path
[Environment]::SetEnvironmentVariable('PATH', $null, 'Process')
[Environment]::SetEnvironmentVariable('Path', $null, 'Process')
[Environment]::SetEnvironmentVariable('Path', $testPath, 'Process')
$root = Split-Path $PSScriptRoot -Parent
$dist = Join-Path $root 'dist/VideoWebcam'
$output = Join-Path $root 'test-output'
New-Item -ItemType Directory -Force $output | Out-Null
$classKey = 'Software\Classes\CLSID\{D9C360E1-24E1-4CD7-BAE5-385BA248EB72}'
foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)) {
    $hive = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,$view)
    $key = $hive.OpenSubKey($classKey)
    if ($key) { $key.Dispose(); $hive.Dispose(); throw 'An installation already exists. This test will not change it.' }
    $hive.Dispose()
}
function Run-Checked([string]$Exe,[string]$Arguments,[string]$LogName) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Exe
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardError = $true
    $info.RedirectStandardOutput = $true
    $process = [System.Diagnostics.Process]::Start($info)
    $errors = $process.StandardError.ReadToEndAsync()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    if (!$process.WaitForExit(15000)) { $process.Kill(); $process.WaitForExit(); throw "$LogName timed out." }
    $errors.Result | Set-Content "$output/$LogName.stderr.log"
    $stdout.Result | Set-Content "$output/$LogName.stdout.log"
    $code = $process.ExitCode
    $process.Dispose()
    if ($code -ne 0) { throw "$LogName failed with exit code $code. See test-output logs." }
}
try {
    Run-Checked "$dist/VideoWebcam.exe" '--register' 'register'
    # An independent FFmpeg process must discover the friendly name and negotiate each format.
    foreach ($format in @('bgr24','yuyv422')) {
        Run-Checked "$dist/ffmpeg.exe" "-hide_banner -loglevel info -f dshow -video_size 1280x720 -framerate 30 -pixel_format $format -i video=VideoWebcam -frames:v 5 -f null -" "capture-$format"
    }
    Write-Host 'Windows device discovery and independent FFmpeg capture passed in RGB24 and YUY2.'
} finally {
    Run-Checked "$dist/VideoWebcam.exe" '--unregister' 'unregister'
}
