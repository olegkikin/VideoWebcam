param([ValidateSet('Release','Debug')][string]$Configuration = 'Release',
      [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$PackageName = 'VideoWebcam')
$ErrorActionPreference = 'Stop'
# Some terminal wrappers supply both Path and PATH. MSBuild rejects that environment.
$buildPath = $env:Path
[Environment]::SetEnvironmentVariable('PATH', $null, 'Process')
[Environment]::SetEnvironmentVariable('Path', $null, 'Process')
[Environment]::SetEnvironmentVariable('Path', $buildPath, 'Process')
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -all -property installationPath
$vsVersion = & $vswhere -latest -products '*' -all -property installationVersion
if (!$vs) { throw 'Install Visual Studio 2022 Build Tools with Desktop development with C++.' }
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (!(Test-Path $cmake)) { $cmake = 'cmake' }
foreach ($arch in @('x64','Win32')) {
    $build = Join-Path $root "build-$arch"
    & $cmake -S $root -B $build -G 'Visual Studio 17 2022' -A $arch "-DCMAKE_GENERATOR_INSTANCE=$vs,version=$vsVersion"
    if ($LASTEXITCODE) { throw "Configure failed: $arch" }
    & $cmake --build $build --config $Configuration --parallel
    if ($LASTEXITCODE) { throw "Build failed: $arch" }
    & $cmake --build $build --config $Configuration --target RUN_TESTS
    if ($LASTEXITCODE) { throw "Tests failed: $arch" }
}
& "$PSScriptRoot/package.ps1" -Configuration $Configuration -PackageName $PackageName
