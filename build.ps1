$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = $env:SIFLI_SDK
if (-not $sdk) {
    $sdk = Join-Path (Split-Path -Parent $root) "SiFli-SDK"
}
if (-not (Test-Path $sdk)) {
    throw "SiFli-SDK not found: $sdk.  Set SIFLI_SDK or clone it next to this project."
}
$env:SIFLI_SDK = $sdk
$env:RTT_CC = "gcc"

if (-not (Get-Command scons -ErrorAction SilentlyContinue)) {
    $scons = Get-ChildItem "$env:USERPROFILE\.sifli\envs\default" -Recurse -Filter scons.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    $gccBin = Get-ChildItem "$env:USERPROFILE\.sifli\tools\arm-none-eabi-gcc" -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "bin" } |
        Where-Object { Test-Path (Join-Path $_ "arm-none-eabi-gcc.exe") } |
        Select-Object -First 1

    if (-not $scons -or -not $gccBin) {
        Write-Host "SiFli build environment is not active."
        Write-Host "Run this once in the SDK directory, then re-run this script:"
        Write-Host "  cd `"$sdk`""
        Write-Host "  . .\export.ps1"
        exit 1
    }

    $env:PATH = "$($scons.Directory.FullName);$gccBin;$env:PATH"
    $env:RTT_EXEC_PATH = $gccBin
}

if (-not $env:RTT_EXEC_PATH) {
    $gccBin = Get-ChildItem "$env:USERPROFILE\.sifli\tools\arm-none-eabi-gcc" -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "bin" } |
        Where-Object { Test-Path (Join-Path $_ "arm-none-eabi-gcc.exe") } |
        Select-Object -First 1
    if ($gccBin) { $env:RTT_EXEC_PATH = $gccBin }
}

$env:PYTHONPATH = "$sdk/tools/build;$sdk/tools/build/default"

$boardPath = Join-Path $root "board"
$projectPath = Join-Path $root "app\project"

Push-Location $projectPath
try {
    & scons --board=sf32lb57-p045vr04 --board_search_path=$boardPath -j 8
} finally {
    Pop-Location
}
