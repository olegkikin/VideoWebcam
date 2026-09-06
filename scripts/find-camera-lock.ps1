$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$target = [IO.Path]::GetFullPath((Join-Path $root 'dist/VideoWebcam/VideoWebcamFilter64.dll'))
Get-Process | ForEach-Object {
    $candidate = $_
    try {
        foreach ($module in $candidate.Modules) {
            if ($module.FileName -eq $target) {
                [PSCustomObject]@{Process=$candidate.ProcessName; Id=$candidate.Id; Window=$candidate.MainWindowTitle}
                break
            }
        }
    } catch { }
}
