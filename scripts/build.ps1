param([string]$Version = '0.0.1')
$ErrorActionPreference = 'Stop'
cmake -S . -B build -A x64 "-DMC_VERSION=$Version"
if ($LASTEXITCODE) { throw 'Configure failed' }
cmake --build build --config Release --parallel
if ($LASTEXITCODE) { throw 'Build failed' }
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
& "$PSScriptRoot/verify-binary.ps1"
& "$PSScriptRoot/package.ps1" -Version $Version
