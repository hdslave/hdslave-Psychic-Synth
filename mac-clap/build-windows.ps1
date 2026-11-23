$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $Root "build\windows"

cmake -S $Root -B $BuildDir -A x64
cmake --build $BuildDir --config Release --parallel

$TestExe = Join-Path $BuildDir "Release\host_test.exe"
$Clap = Join-Path $BuildDir "clap\Release\Psychic.clap"
if (-not (Test-Path $Clap)) {
    $Clap = Join-Path $BuildDir "clap\Psychic.clap"
}

& $TestExe $Clap
Write-Host "Built Psychic.clap! Copy it to: C:\Program Files\Common Files\CLAP\"
