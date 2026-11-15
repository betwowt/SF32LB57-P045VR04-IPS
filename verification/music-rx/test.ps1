$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$sdkRoot = Join-Path (Split-Path -Parent $projectRoot) 'SiFli-SDK'
$testExe = Join-Path $PSScriptRoot 'source_test.exe'
$sourceRoot = Join-Path $projectRoot 'app/src/xiaozhi'
& gcc -std=c11 -Wall -Wextra -Werror -DAUDIOPROC_H `
    '-I' (Join-Path $PSScriptRoot 'host/include') `
    '-I' (Join-Path $sdkRoot 'middleware/audio/include') `
    '-I' $sourceRoot `
    (Join-Path $sourceRoot 'xiaozhi_music_source.c') `
    (Join-Path $PSScriptRoot 'host/source_test.c') '-o' $testExe
if ($LASTEXITCODE -ne 0) { throw 'Adapter test compilation failed' }
& $testExe
if ($LASTEXITCODE -ne 0) { throw 'Adapter regression test failed' }
